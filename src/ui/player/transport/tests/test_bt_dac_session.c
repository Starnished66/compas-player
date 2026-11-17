/* Run queued jobs explicitly to exercise cancellation before worker entry. */
#define GUI_SHELL_INTERNAL_H
#define HOST_BUILD
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "src/network/bluetooth/control/bluetooth_control.h"
typedef struct { atomic_bool done; bool active; } shell_job_t;
enum { USB_MODE_DAC = 1 };
static struct { bool bt_dac_mode_enabled, wifi_dac_mode_enabled; int usb_mode; } current_settings;
static bool bt_is_powered_cached, idle, start_ok, stop_ok;
static bt_dac_stream_info_t snapshot;
static int starts, stops, checkpoints, waits;
static int enables;
static bool allow_unowned_stop, usb_owned, usb_pending;
static bool cancel_wait, cancel_spawn, stop_timeout;
static void *(*pending)(void *);
static void *pending_arg;
static void gui_shell_bt_dac_release(void);
static bool shell_job_start(shell_job_t *job, void *(*fn)(void *), void *arg) {
    job->active=true; atomic_store(&job->done,false); pending=fn; pending_arg=arg; return true;
}
static bool shell_job_reap(shell_job_t *job) {
    if(!job->active || !atomic_load(&job->done)) return false;
    job->active=false; return true;
}
static void shell_job_cancel(shell_job_t *job) { (void)job; }
void bt_control_get_dac_stream_info(bt_dac_stream_info_t *out) { *out=snapshot; }
static bool audio_is_idle(void) { return idle; }
static bool gui_shell_bt_dac_busy(void);
bool gui_shell_bt_dac_owns_output(void);
static bool gui_shell_bt_dac_profile_failed(void) { return false; }
static bool gui_shell_bt_dac_profile_ready(void) { return current_settings.bt_dac_mode_enabled && bt_is_powered_cached; }
static int finishes;
static void gui_shell_bt_dac_finish_profiles(void) { finishes++; }
static bool bt_restore_pending;
bool power_suspend_bt_restore_pending(void) { return bt_restore_pending; }
static bool workers_busy;
static bool bt_workers_active(void) { return workers_busy; }
static void enable_bt_dac_mode(void) { enables++; current_settings.bt_dac_mode_enabled = true; }
static bool gui_network_airplay_enable_pending(void) { return false; }
static bool gui_network_usb_dac_pending(void) { return usb_pending; }
static bool gui_shell_usb_dac_owns_output(void) { return usb_owned; }
static void gui_player_stop_for_external_session(void) { checkpoints++; }
void bt_control_dac_set_reserved(bool reserved) { (void)reserved; }
static void fake_sleep(unsigned int delay);
#define usleep fake_sleep
bool bt_control_dac_route_start(const bt_dac_stream_info_t *info, bt_control_cancel_callback_t cancelled, void *ctx) {
    assert(idle && info->running && !cancelled(ctx)); starts++;
    if(cancel_spawn) gui_shell_bt_dac_release();
    return start_ok;
}
bool bt_control_dac_route_alive(void) { return true; }
bool bt_control_dac_route_stop(void);
#include "src/ui/shell/gui_shell_bt_dac_session.c"
static bool gui_shell_bt_dac_busy(void) { return owns_output; }
static void fake_sleep(unsigned int delay) {
    waits++;
    if(delay==10000) { if(cancel_wait) gui_shell_bt_dac_release(); else if (!stop_timeout) idle=true; }
    else gui_shell_bt_dac_release();
}
bool bt_control_dac_route_stop(void) {
    assert(gui_shell_bt_dac_owns_output() || allow_unowned_stop); stops++; return stop_ok;
}
static void reset(void) {
    memset(&session_job,0,sizeof(session_job)); owns_output=false; session_error=false;
    consumed_generation=0; atomic_store(&request_generation,0); atomic_store(&receiving,false);
    current_settings.bt_dac_mode_enabled=true; current_settings.wifi_dac_mode_enabled=false;
    current_settings.usb_mode=0; bt_is_powered_cached=true;
    idle=false; start_ok=stop_ok=true; cancel_wait=cancel_spawn=stop_timeout=false; starts=stops=checkpoints=waits=0;
    usb_owned=usb_pending=false;
    workers_busy=false; bt_restore_pending=false; finishes=0; enables=0; allow_unowned_stop=false; restore_on_power=false; restore_saw_off=false;
    snapshot=(bt_dac_stream_info_t){.available=true,.running=true,.generation=1,
        .mac="AA:BB:CC:DD:EE:FF",.pcm_path="/owner"};
}
int main(void) {
    reset(); usb_pending=true; poll_bt_dac_session();
    assert(!owns_output && checkpoints==0);
    usb_pending=false; usb_owned=true; poll_bt_dac_session();
    assert(!owns_output && checkpoints==0);
    usb_owned=false; poll_bt_dac_session();
    assert(owns_output && checkpoints==1);
    pending(pending_arg); poll_bt_dac_session();
    reset(); poll_bt_dac_session(); assert(owns_output && checkpoints==1 && starts==0);
    pending(pending_arg); assert(waits && starts==1 && stops==1 && owns_output);
    poll_bt_dac_session(); assert(!owns_output); /* Only reaping releases reservation. */
    poll_bt_dac_session(); assert(starts==1 && checkpoints==1); /* No retry of same edge. */
    reset(); poll_bt_dac_session(); gui_shell_bt_dac_release();
    pending(pending_arg); assert(starts==0 && stops==1);
    poll_bt_dac_session(); assert(!owns_output);
    reset(); cancel_wait=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==0 && stops==1 && owns_output); poll_bt_dac_session(); assert(!owns_output);
    reset(); stop_timeout=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==0 && stops==1 && owns_output && worker_failed);
    poll_bt_dac_session(); assert(!owns_output && session_error);
    reset(); cancel_spawn=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==1 && stops==1 && !gui_shell_bt_dac_receiving());
    poll_bt_dac_session(); assert(!owns_output);
    reset(); poll_bt_dac_session();
    snapshot.generation=2; snprintf(snapshot.mac,sizeof(snapshot.mac),"11:22:33:44:55:66");
    snprintf(snapshot.pcm_path,sizeof(snapshot.pcm_path),"/second");
    poll_bt_dac_session(); pending(pending_arg); poll_bt_dac_session();
    assert(!owns_output && starts==0 && checkpoints==1); /* No queued second-phone takeover. */
    reset(); poll_bt_dac_session(); snapshot.generation=2; snprintf(snapshot.pcm_path,sizeof(snapshot.pcm_path),"/resumed");
    poll_bt_dac_session(); pending(pending_arg); poll_bt_dac_session();
    assert(owns_output && owner.generation==2 && checkpoints==2); /* Same phone resuming before the reap. */
    reset(); start_ok=false; poll_bt_dac_session(); pending(pending_arg);
    poll_bt_dac_session(); assert(!owns_output && session_error && starts==1);
    poll_bt_dac_session(); assert(starts==1 && checkpoints==1);
    reset(); stop_ok=false; poll_bt_dac_session(); pending(pending_arg);
    poll_bt_dac_session(); assert(owns_output && session_error); /* Unsafe closure keeps lock. */
    reset(); snapshot.running = false; gui_shell_bt_dac_remember(true);
    poll_bt_dac_session(); assert(current_settings.bt_dac_mode_enabled); /* Still on: intent remains enabled. */
    bt_is_powered_cached = false; poll_bt_dac_session(); assert(current_settings.bt_dac_mode_enabled);
    bt_is_powered_cached = true; poll_bt_dac_session();
    assert(current_settings.bt_dac_mode_enabled && !restore_on_power && enables==1);
    reset(); snapshot.running = false; gui_shell_bt_dac_remember(true);
    bt_is_powered_cached = false; poll_bt_dac_session();
    current_settings.bt_dac_mode_enabled = false; gui_shell_bt_dac_remember(false); /* Explicit off cancels pending restore. */
    bt_is_powered_cached = true; poll_bt_dac_session();
    assert(!current_settings.bt_dac_mode_enabled && !restore_on_power && enables==0);
    reset(); snapshot.running = false; gui_shell_bt_dac_remember(true);
    bt_is_powered_cached = false; poll_bt_dac_session(); current_settings.wifi_dac_mode_enabled = true;
    bt_is_powered_cached = true; poll_bt_dac_session(); /* AirPlay chosen meanwhile wins. */
    assert(current_settings.bt_dac_mode_enabled && !restore_on_power && enables==0);
    reset(); snapshot.running = false; gui_shell_bt_dac_remember(true);
    gui_shell_bt_dac_powered_off(); /* The cached radio state missed the brief OFF edge. */
    poll_bt_dac_session();
    assert(current_settings.bt_dac_mode_enabled && !restore_on_power && enables==1);
    reset(); snapshot.running = false; allow_unowned_stop=true;
    assert(gui_shell_bt_dac_shutdown(false));
    assert(current_settings.bt_dac_mode_enabled && restore_on_power);
    bt_is_powered_cached = false; poll_bt_dac_session();
    bt_is_powered_cached = true; poll_bt_dac_session();
    assert(current_settings.bt_dac_mode_enabled && !restore_on_power && enables==1);
    /* Idle sleep: in use only with a phone connected, audio owned or a profile apply running. */
    reset(); snapshot=(bt_dac_stream_info_t){0}; assert(!gui_shell_bt_dac_in_use()); /* Mode on, nothing connected. */
    snapshot.available=true; assert(gui_shell_bt_dac_in_use()); /* Phone connected. */
    snapshot=(bt_dac_stream_info_t){0}; owns_output=true; assert(gui_shell_bt_dac_in_use()); /* DAC owns output. */
    owns_output=false; workers_busy=true; assert(gui_shell_bt_dac_in_use()); /* Apply job mid-flight. */
    reset(); current_settings.bt_dac_mode_enabled=false; snapshot=(bt_dac_stream_info_t){0};
    assert(!gui_shell_bt_dac_in_use()); /* Mode off: monitor stopped, nothing to hold awake. */
    /* Idle suspend with the radio still powered: the remembered mode returns with no power edge. */
    reset(); snapshot=(bt_dac_stream_info_t){0}; allow_unowned_stop=true;
    assert(gui_shell_bt_dac_shutdown(true) && restore_on_power);
    poll_bt_dac_session(); assert(restore_on_power && enables==0);
    gui_shell_bt_dac_resumed(); poll_bt_dac_session();
    assert(current_settings.bt_dac_mode_enabled && !restore_on_power && enables==1);
    /* The idle countdown restarts once, on the in-use to idle edge. */
    reset(); snapshot=(bt_dac_stream_info_t){0}; (void)gui_shell_bt_dac_went_idle();
    assert(!gui_shell_bt_dac_went_idle());
    snapshot.available=true; assert(!gui_shell_bt_dac_went_idle()); /* Phone connected. */
    snapshot=(bt_dac_stream_info_t){0}; assert(gui_shell_bt_dac_went_idle()); /* Phone gone. */
    assert(!gui_shell_bt_dac_went_idle());
    /* Radio suspend already powered Bluetooth off: no second source-profile apply. */
    reset(); snapshot=(bt_dac_stream_info_t){0}; bt_is_powered_cached=false; allow_unowned_stop=true;
    assert(gui_shell_bt_dac_shutdown(true) && finishes==0 && restore_on_power);
    bt_is_powered_cached=true; assert(gui_shell_bt_dac_shutdown(true) && finishes==1);
    /* A failed idle suspend stays in DAC mode with nothing left remembered... */
    reset(); allow_unowned_stop=true; owns_output=true; stop_ok=false;
    assert(!gui_shell_bt_dac_shutdown(true) && !restore_on_power && !gui_shell_bt_dac_suspended());
    /* ...but keeps a mode that radio suspend had already set aside. */
    reset(); allow_unowned_stop=true; owns_output=true; stop_ok=false; restore_on_power=true;
    assert(!gui_shell_bt_dac_shutdown(true) && restore_on_power);
    /* The restore waits for the post-resume Bluetooth restore, then runs once. */
    reset(); snapshot=(bt_dac_stream_info_t){0}; allow_unowned_stop=true;
    assert(gui_shell_bt_dac_shutdown(true)); bt_restore_pending=true;
    gui_shell_bt_dac_resumed(); poll_bt_dac_session(); poll_bt_dac_session();
    assert(restore_on_power && enables==0);
    bt_restore_pending=false; poll_bt_dac_session();
    assert(!restore_on_power && enables==1);
    puts("Bluetooth DAC session ordering tests passed");
}
