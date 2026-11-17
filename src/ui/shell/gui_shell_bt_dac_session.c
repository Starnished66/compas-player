#include "gui_shell_internal.h"
#include "../../hardware/power/power_suspend.h"

static shell_job_t session_job;
static bool owns_output;
static bool session_error;
static uint32_t consumed_generation;
static atomic_uint request_generation;
static atomic_bool receiving;
static bool worker_failed;
static bool worker_closed;
static unsigned int worker_generation;
static bt_dac_stream_info_t owner;
static bool restore_on_power;
static bool restore_saw_off;

bool gui_shell_bt_dac_suspended(void) { return restore_on_power; }

void gui_shell_bt_dac_remember(bool restore) { restore_on_power = restore; restore_saw_off = false; }
void gui_shell_bt_dac_powered_off(void) { restore_saw_off = true; }
/* Back from a suspend or an aborted power off that left Bluetooth powered: no power edge will
 * come. The restore still waits for a post-resume Bluetooth restore that is bringing the chip back. */
void gui_shell_bt_dac_resumed(void) { if (bt_is_powered_cached) restore_saw_off = true; }

/* Only after Bluetooth has really gone off and come back, and only if no other receiver took over. */
static void restore_after_power_on(void) {
    if (!restore_on_power) return;
    if (!bt_is_powered_cached) { restore_saw_off = true; return; }
    if (!restore_saw_off || bt_workers_active() || power_suspend_bt_restore_pending() || gui_shell_bt_dac_owns_output()) return;
    restore_on_power = false;
    if (current_settings.wifi_dac_mode_enabled || gui_network_airplay_enable_pending() ||
        current_settings.usb_mode == USB_MODE_DAC || gui_network_usb_dac_pending()) return;
    if (current_settings.bt_dac_mode_enabled) enable_bt_dac_mode();
}

bool gui_shell_bt_dac_owns_output(void) { return owns_output; }
/* The route lives only while the session owns the output, so owns_output covers it without
 * bt_control_dac_route_alive(), which takes a mutex that profile changes hold for seconds. */
bool gui_shell_bt_dac_in_use(void) {
    bt_dac_stream_info_t info;
    bt_control_get_dac_stream_info(&info);
    return info.available || owns_output || bt_workers_active();
}
/* True once when the DAC stops being in use, so the idle countdown restarts from there. */
bool gui_shell_bt_dac_went_idle(void) {
    static bool was_in_use;
    bool in_use = gui_shell_bt_dac_in_use();
    bool went_idle = was_in_use && !in_use;
    was_in_use = in_use;
    return went_idle;
}
bool gui_shell_bt_dac_receiving(void) { return owns_output && atomic_load(&receiving); }
bool gui_shell_bt_dac_failed(void) { return session_error || gui_shell_bt_dac_profile_failed(); }

void gui_shell_bt_dac_release(void) {
    atomic_fetch_add(&request_generation, 1);
    atomic_store(&receiving, false);
}

static bool session_current(unsigned int generation) {
    bt_dac_stream_info_t info;
    bt_control_get_dac_stream_info(&info);
    return atomic_load(&request_generation) == generation && info.running &&
           info.generation == owner.generation && !strcmp(info.pcm_path, owner.pcm_path);
}

static bool session_cancelled(void * ctx) {
    return atomic_load(&request_generation) != *(unsigned int *) ctx;
}

static void * handoff_worker(void * arg) {
    (void) arg;
    unsigned int generation = worker_generation;
    worker_failed = false;
    worker_closed = false;
    int waited = 0;
    while (session_current(generation) && !audio_is_idle() && waited < 2000) {
        usleep(10000); waited += 10;
    }
    if (session_current(generation)) {
        if (!audio_is_idle() || !bt_control_dac_route_start(&owner, session_cancelled, &generation)) worker_failed = true;
        else {
            /* Let immediate exec/startup failures surface before showing receiving. */
            for (int ms = 0; ms < 100 && session_current(generation); ms += 10) usleep(10000);
            while (session_current(generation)) {
                if (!bt_control_dac_route_alive()) { worker_failed = true; break; }
                atomic_store(&receiving, true);
                usleep(200000); /* a release only needs to land within a fifth of a second */
            }
        }
    }
    atomic_store(&receiving, false);
    worker_closed = bt_control_dac_route_stop();
    atomic_store_explicit(&session_job.done, true, memory_order_release);
    return NULL;
}

void poll_bt_dac_session(void) {
    restore_after_power_on();
    bt_dac_stream_info_t info;
    bt_control_get_dac_stream_info(&info);
    /* Another phone never takes over; the same phone resuming is picked up after cleanup. */
    if (owns_output && info.generation != owner.generation && strcmp(info.mac, owner.mac))
        consumed_generation = info.generation;
    if (owns_output && (!current_settings.bt_dac_mode_enabled || !bt_is_powered_cached ||
            info.generation != owner.generation || !info.running)) gui_shell_bt_dac_release();
    if (shell_job_reap(&session_job)) {
        session_error = worker_failed || !worker_closed;
        if (worker_closed) { owns_output = false; bt_control_dac_set_reserved(false); }
    }
    if (owns_output || session_job.active || !gui_shell_bt_dac_profile_ready() ||
        gui_network_usb_dac_pending() || gui_shell_usb_dac_owns_output()) return;
#ifndef HOST_BUILD
    bt_media_player_set_phone_target(info.mac);
#endif
    if (!info.running || info.generation == consumed_generation) return;
    consumed_generation = info.generation;
    owner = info;
    owns_output = true;
    bt_control_dac_set_reserved(true);
    session_error = false;
    worker_generation = atomic_fetch_add(&request_generation, 1) + 1;
#ifndef HOST_BUILD
    bt_media_player_set_phone_target(owner.mac);
#endif
    gui_player_stop_for_external_session();
    if (!shell_job_start(&session_job, handoff_worker, NULL)) {
        owns_output = false;
        bt_control_dac_set_reserved(false);
        session_error = true;
    }
}

bool gui_shell_bt_dac_shutdown(bool restore_source) {
    if (!current_settings.bt_dac_mode_enabled && !gui_shell_bt_dac_busy()) return true;
    const bool was_remembered = restore_on_power;
    gui_shell_bt_dac_remember(current_settings.bt_dac_mode_enabled);
    gui_shell_bt_dac_release();
    shell_job_cancel(&session_job);
    if (bt_control_dac_route_stop()) { owns_output = false; bt_control_dac_set_reserved(false); }
    if (!restore_source) return true; /* powering off: nothing to bring back */
    /* Radio suspend may already have powered Bluetooth off: nothing to apply then. */
    bool done = !owns_output;
    if (bt_is_powered_cached) {
        gui_shell_bt_dac_finish_profiles();
        done = done && !gui_shell_bt_dac_profile_failed();
    }
    if (!done && !was_remembered) gui_shell_bt_dac_remember(false); /* stays in DAC mode, nothing to restore */
    return done;
}
