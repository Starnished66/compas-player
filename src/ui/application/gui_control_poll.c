#include "gui.h"
#include "../settings/controls/gui_external_controls.h"
#include "../text/i18n/i18n.h"
#include "../../core/ui/display/ui_wake.h"
#include "../updates/gui_updates.h"
#include "../network/gui_network.h"
#include "../network/subsonic/gui_subsonic.h"
#include "../network/subsonic/gui_subsonic_covers.h"
#include "../player/screen/gui_lyrics.h"
#include "../player/screen/gui_track_info.h"
#include "../../hardware/indicators/backlight.h"
#include "../../network/services/dlna_control.h"
#include "../../hardware/power/battery.h"
#include "../../audio/output/audio_output.h"
#include "../../core/system/logging/db_log.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../../library/workers/track_metadata_worker.h"
#include "../../hardware/device/headphone_status.h"
#include "../plugins/manage/gui_plugin_manage.h"
#include "../plugins/manage/gui_plugin_store.h"
#include "../plugins/emulators/gui_emulator_store.h"
#include "../../hardware/indicators/led_control.h"
#include "../../hardware/power/charge_limiter.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include <stdlib.h>
#include <string.h>
#include "gui_internal.h"

#ifdef UI_PERF_TRACE
#include "../diagnostics/ui_frame_trace.h"
uint64_t ui_perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

/* Numeric control sections share the bounded tracer, including ordinary ticks
 * so idle invalidation and transition drains can be correlated. */
static uint64_t update_perf_wall_start, update_perf_cpu_start;
static unsigned update_perf_section;

static uint64_t update_perf_cpu_us(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static void update_perf_begin(void) {
    update_perf_section = 0;
    update_perf_wall_start = ui_perf_now_us();
    update_perf_cpu_start = update_perf_cpu_us();
}

static void update_perf_mark(const char * section) {
    (void) section; /* IDs follow call order, documented by the host decoder. */
    uint64_t wall = ui_perf_now_us(), cpu = update_perf_cpu_us();
    uint32_t id = ++update_perf_section;
    uint64_t wall_delta = wall - update_perf_wall_start;
    uint64_t cpu_delta = cpu - update_perf_cpu_start;
    /* Always retain the topbar tick; other sections are sparse long-work
     * attribution rather than dozens of records per unchanged status tick. */
    if (id == 2 || wall_delta >= 1000 || cpu_delta >= 1000) {
        ui_frame_trace_event(UI_FRAME_TRACE_EVENT_CONTROL_POLL, id,
                             wall_delta > UINT32_MAX ? UINT32_MAX : (uint32_t) wall_delta);
        ui_frame_trace_event(UI_FRAME_TRACE_EVENT_CONTROL_POLL, id | 0x80000000u,
                             cpu_delta > UINT32_MAX ? UINT32_MAX : (uint32_t) cpu_delta);
    }
    update_perf_wall_start = wall;
    update_perf_cpu_start = cpu;
}

#define PERF_MARK(section) update_perf_mark(section)
#else
#define PERF_MARK(section) ((void) 0)
#endif

static lv_timer_t *player_ui_timer;
static uint32_t player_ui_timer_period_ms;

void gui_control_poll_kick_player_ui(void) {
    if (player_ui_timer) lv_timer_ready(player_ui_timer);
}

/* A worker published a result. Before the timers: the library's "is it done"
 * poll timers become due now. After them: whatever depends on what they just
 * applied (a populated page starts its lookups and probes, a landed artist
 * summary starts its probe batch) runs in the same pass, so one render and one
 * layout follow for all of it. Nothing here runs more than the 500 ms housekeeping
 * would have; it only runs sooner. */
void gui_jobs_dispatch_begin(void) {
    gui_library_jobs_ready_timers();
    /* Subsonic workers only set a done flag: reap them now, not at the 500 ms tick. */
    poll_subsonic_download();
    poll_subsonic_library_download();
    poll_subsonic_connect();
    poll_subsonic_browse();
    poll_subsonic_refresh();
    poll_subsonic_covers();
}

/* A slide shows a frozen snapshot: page updates wait for it to end, like the
 * other landings do (take_deferred() then asks the loop for another pass). */
static bool jobs_end_deferred;

void gui_jobs_dispatch_end(void) {
    if (gui_navigation_transition_in_progress()) {
        jobs_end_deferred = true;
        return;
    }
    jobs_end_deferred = false;
    gui_library_poll_track_probes();
}

bool gui_jobs_take_deferred(void) {
    if (!jobs_end_deferred || gui_navigation_transition_in_progress()) return false;
    jobs_end_deferred = false;
    return true;
}

void gui_process_input_wake(void) {
    media_button_timer_cb(NULL);
    poll_cover_decode();
    gui_control_poll_kick_player_ui();
}

static void sync_accessory_volume(void) {
    /* Accessory volume is applied by the BlueALSA monitor thread. Keep its
     * UI/persisted mirror on this fast player cadence, including while the
     * screen is off. Leave the pending signal queued during a slider drag;
     * after the drag, mirror the current audio value so an older queued
     * accessory percentage cannot roll back a newer local adjustment. */
    if (!gui_player_volume_is_being_adjusted()) {
        int ignored_bt_percent;
        if (bt_control_source_volume_sync_consume_percent(&ignored_bt_percent)) {
            int bt_volume_percent = gui_capped_volume_percent();
            gui_player_set_volume_percent(bt_volume_percent);
            gui_player_remember_volume_percent(bt_volume_percent);
            settings_save_async(&current_settings);
            show_volume_popup(bt_volume_percent);
            refresh_volume_topbar(bt_volume_percent);
        }
    }
}

static void consume_playback_events(void) {
    uint64_t playback_err_gen = 0;
    audio_error_t playback_err = audio_consume_error_ex(&playback_err_gen);
    if (playback_err != AUDIO_ERROR_NONE) {
        gui_player_handle_playback_error_ex(playback_err, playback_err_gen);
    } else if (audio_consume_track_advanced()) {
        gui_player_handle_auto_advance();
    } else if (audio_consume_track_finished()) {
        gui_player_handle_track_finished();
    }
}

/* Run player completion at 50 ms while awake and 500 ms while off; the latter
 * keeps correctness work running at the system housekeeping cadence. */
static void player_ui_timer_cb(lv_timer_t * timer) {
    (void) timer;
    uint32_t period_ms = backlight_screen_is_on() ? 50 : 500;
    /* A stream link is on a track's start path: dispatch at this cadence,
     * not the 500 ms housekeeping one. */
    plugin_manager_dispatch_remote_streams();
    if (player_ui_timer_period_ms != period_ms) {
        player_ui_timer_period_ms = period_ms;
        lv_timer_set_period(player_ui_timer, period_ms);
    }

    sync_accessory_volume();
    consume_playback_events();

    gui_player_poll_track_metadata();
    poll_cover_decode();
    gui_lyrics_poll_load();
    gui_lyrics_poll_backdrop();
    gui_player_poll_confirmed_playback();
    gui_player_poll_waveform();

    if (!backlight_screen_is_on()) return;
    if (gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress()) return;
    if (!gui_player_has_active_track() || gui_player_is_seeking()) return;

    gui_player_update_progress();
    refresh_format_badge();
}

/* DLNA/UPnP-AV cast playback:
 * When a cast track is downloaded and ready, hand it to on_file_selected()
 * for local playback using the file's embedded metadata tags. */
static void poll_dlna_control(void) {
    char path[512], title[256], artist[256], album[256];
    if (dlna_control_consume_ready_track(path, sizeof(path), title, sizeof(title),
                                          artist, sizeof(artist), album, sizeof(album))) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(path);
        clear_player_source(); /* cast from another device -- no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (dlna_control_consume_stop_requested()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    dlna_control_notify_status(audio_is_playing(), audio_is_paused());
}

static void poll_hardware_status(void) {
    /* Also unconditional on screen state -- charging happens with the
     * screen off far more often than on, so gating this the same way as the
     * topbar refreshes above would leave it capped at whatever percent the
     * screen happened to be on last. charge_limiter_poll() throttles its
     * own actual sysfs work internally, so calling it every tick here is
     * cheap. */
    charge_limiter_poll(current_settings.charge_limiter_enabled, false);
    PERF_MARK("charge_limit");
    safe_charging_poll(current_settings.safe_charging_enabled, false);
    PERF_MARK("safe_charge");
    led_control_poll(current_settings.led_indicator_enabled);
    PERF_MARK("led_status");
    plugin_manager_notify_volume_changed((int) (audio_get_volume() * 100.0f + 0.5f));
    plugin_manager_poll_battery();
    PERF_MARK("plugin_battery");
    headphone_status_refresh_earpods_adc();
    PERF_MARK("hardware");
}

static void poll_network_jobs(void) {
    poll_subsonic_download();
    poll_firmware_ota();
    poll_plugin_store();
    poll_emulator_store();
    gui_plugin_manage_poll();
    poll_subsonic_library_download();
    poll_dlna_control();
    poll_subsonic_connect();
    poll_subsonic_browse();
    poll_subsonic_refresh();
    poll_subsonic_covers();
    poll_wifi_scan();
    poll_wifi_connect();
    poll_wifi_connect_saved();
    poll_wifi_disconnect();
    poll_wifi_forget();
    gui_shell_poll();
    plugin_manager_poll();
    poll_bt_scan();
    poll_bt_rate_query();
    poll_bt_rate_apply();
    poll_bt_connect();
    poll_bt_forget();
    poll_bt_pairing();
    poll_library_rescan();
    poll_sd_format();
    poll_remote_import();
    PERF_MARK("network_jobs");
}

static void poll_storage_jobs(void) {
    poll_usb_mode_switch();
    PERF_MARK("usb_mode");
    poll_usb_storage_hotplug();
    PERF_MARK("usb_hotplug");
    poll_sd_card_hotplug();
    PERF_MARK("sd_hotplug");
    gui_library_poll_playlists();
    gui_library_poll_track_probes();
    PERF_MARK("storage_jobs");
}

static void poll_playback_view_jobs(void) {
    gui_player_service_artwork_cache();
    poll_search_job();
    gui_navigation_poll_snapshot_rebuild();
    gui_library_poll_now_playing_indicators();
}

static void notify_persistence_failures(void) {
    static bool queue_checkpoint_failure_notified;
    if (gui_player_queue_checkpoint_failed()) {
        if (!queue_checkpoint_failure_notified) {
            show_error_toast(TR("Queue checkpoint failed; storage may be read-only"));
            queue_checkpoint_failure_notified = true;
        }
    } else queue_checkpoint_failure_notified = false;
    static bool numeric_failure_notified;
    if (metadata_db_numeric_write_failed() || track_metadata_history_failed()) {
        if (!numeric_failure_notified) {
            show_error_toast(TR("Playback history could not be saved"));
            numeric_failure_notified = true;
        }
    } else numeric_failure_notified = false;
}

static void poll_queue_and_info_jobs(void) {
    gui_queue_poll();
    gui_network_poll_airplay_overlay();
    gui_track_info_poll();
    gui_library_poll_boot_prompt();
    PERF_MARK("player_jobs");
}

static void poll_background_jobs(void) {
    /* Polling for in-flight async operations (wifi/bt connect, library scan,
     * subsonic sync, ...) keeps running regardless of screen state -- these
     * only do real work when a request the user already made is still
     * pending, so gating them on screen state would leave that operation
     * stuck until the user wakes the screen back up. */
    poll_network_jobs();
    poll_storage_jobs();
    poll_playback_view_jobs();
    notify_persistence_failures();
    poll_queue_and_info_jobs();
}

#ifndef HOST_BUILD
static bool bt_output_connected_debounced(void) {
    /* Bluetooth disconnect is debounced on wall-clock time (12s) to prevent
     * transient bluealsactl busy polling failures from falsely cutting playback.
     * Wired headphone disconnects remain immediate via direct sysfs reads. */
#define BT_OUTPUT_DISCONNECT_DEBOUNCE_MS 12000
    static uint32_t bt_disconnected_since_tick = 0; /* 0 = currently connected (or never sampled) */
    if (gui_shell_is_bt_audio_connected()) {
        bt_disconnected_since_tick = 0;
    } else if (bt_disconnected_since_tick == 0) {
        bt_disconnected_since_tick = lv_tick_get();
    }
    /* The monitor confirms a removal after a short reconfiguration
     * grace period: BlueALSA can remove/re-add its PCM while changing
     * sample rates without disconnecting the headphones. Only a
     * confirmed removal bypasses the slower polling fallback above.
     * The event is consumed once; reconnection never auto-resumes. */
    bool bt_disconnect_event = bt_control_output_disconnect_consume();
    if (bt_disconnect_event) {
        gui_shell_notify_bt_audio_disconnected();
    }
    bool bt_connected_debounced = !bt_disconnect_event &&
        (gui_shell_is_bt_audio_connected() ||
         lv_tick_elaps(bt_disconnected_since_tick) < BT_OUTPUT_DISCONNECT_DEBOUNCE_MS);
    return bt_connected_debounced;
}

/* Auto-stop on headphone-output loss: this hardware has no built-in
 * speaker, so a mid-playback disconnect (headphone jack pulled, or the
 * connected Bluetooth A2DP headphone drops) leaves nowhere for the
 * audio to go -- stop outright rather than leaving it silently playing
 * into nothing. Checked every tick, deliberately NOT gated on
 * screen_on_now below (background, screen-off playback needs the same
 * protection). get_headphone_state() is a cheap sysfs read (no
 * subprocess); the A2DP half reuses whatever poll_refresh_bt_icon()
 * last found rather than re-querying bluealsactl here every tick --
 * that's already throttled to its own ~5s cadence (see
 * refresh_bt_icon_thread_func()), and a few seconds of extra latency on
 * just the Bluetooth half is an acceptable tradeoff against forking a
 * process every single tick.
 *
 * Target-only: HOST_BUILD has no real jack/BT hardware to detect at
 * all (get_headphone_state() is unconditionally HEADPHONE_STATE_NONE there, see
 * headphone_status.h), so this would otherwise fire a spurious "stop"
 * on the very first tick after starting playback in the simulator --
 * there's no real disconnect to protect against on host, since the dev
 * machine's own speakers are the actual output regardless of this
 * app's simulated jack/BT state. */
static void poll_output_connection(void) {
    bool bt_connected_debounced = bt_output_connected_debounced();

    static bool last_output_connected = true; /* starts true so nothing fires before any real state has been sampled */
    /* Headphone detection covers both single-ended and balanced jacks. */
    bool output_connected = get_headphone_state() != HEADPHONE_STATE_NONE || bt_connected_debounced;
    /* Pause playback on disconnect while preserving decoder and position
     * so playback can resume when an output is reconnected. Only triggers
     * if audio is actively playing to avoid toggling an already-paused track. */
    bool was_playing = audio_is_playing();
    if (last_output_connected && !output_connected && was_playing) {
        DBG_LOG("gui: output disconnected while playing -- pausing playback\n");
        audio_toggle_pause();
        set_play_button_state(false);
        show_error_toast(TR("Paused: headphones disconnected"));
    }
    last_output_connected = output_connected;

    /* Synchronize balanced output routing for hardware supporting balanced jacks. */
    audio_output_sync_balanced_output();
}

enum { CAR_POWER_REMOVAL_DEBOUNCE_POLLS = 3 };

typedef struct {
    bool power_sample_initialized;
    bool last_external_power;
    unsigned int power_absent_polls;
    bool car_shutdown_pending;
} car_mode_state_t;

static car_mode_state_t car_mode_state;

static void car_mode_reset_baseline(car_mode_state_t *s) {
    /* Re-sample from a clean baseline if Car Mode is enabled later;
     * enabling it while already unplugged is not itself a removal
     * edge and must not immediately power the device off. */
    s->power_sample_initialized = false;
    s->power_absent_polls = 0;
    s->car_shutdown_pending = false;
}

static void car_mode_sample_power(car_mode_state_t *s,
                                  battery_external_power_state_t power_state) {
    if (power_state == BATTERY_EXTERNAL_POWER_UNKNOWN) {
        /* Consecutive means consecutive valid samples.  A sysfs
         * failure is not evidence of either state and must not let
         * two old off samples plus one later off sample confirm a
         * removal.  An already-confirmed pending shutdown is left
         * intact; only a positive connected sample cancels it. */
        s->power_absent_polls = 0;
        return;
    }
    bool external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
    if (!s->power_sample_initialized) {
        s->last_external_power = external_power;
        s->power_sample_initialized = true;
        s->power_absent_polls = 0;
        DBG_LOG("car_mode: initial external power=%d\n", external_power);
    } else if (external_power) {
        if (!s->last_external_power || s->power_absent_polls > 0 || s->car_shutdown_pending)
            DBG_LOG("car_mode: external power present; removal cancelled\n");
        s->last_external_power = true;
        s->power_absent_polls = 0;
        s->car_shutdown_pending = false;
    } else if (s->last_external_power) {
        if (s->power_absent_polls < CAR_POWER_REMOVAL_DEBOUNCE_POLLS) s->power_absent_polls++;
        if (s->power_absent_polls >= CAR_POWER_REMOVAL_DEBOUNCE_POLLS) {
            s->last_external_power = false;
            s->car_shutdown_pending = true;
            DBG_LOG("car_mode: external power removal confirmed; shutdown_pending=%d\n",
                    s->car_shutdown_pending);
        }
    }
}

static void car_mode_shutdown_if_pending(const car_mode_state_t *s) {
    if (s->car_shutdown_pending && !shutdown_background_work_active()) {
        /* Stopped audio has no live position; keep the saved one. */
        if (audio_is_playing() || audio_is_paused())
            current_settings.last_position = audio_get_resume_position_seconds();
        /* This save is intentionally synchronous: idle_shutdown_now() is
         * imminent, so the final playback checkpoint must reach storage
         * before power is removed. */
        settings_save(&current_settings);
        (void) gui_shell_bt_dac_shutdown(false);
        idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
    }
}

/* Car Mode:
 * When external power is disconnected, save current playback position and
 * power off the device (idle_shutdown_now()). When power returns, the device
 * boots and auto-resumes playback.
 *
 * External power presence is edge-triggered using physical power supply state
 * rather than battery charging status, which is a charger-activity signal, not
 * a cable-presence one (see battery_is_charging()'s own doc comment). Power
 * removal must persist for three consecutive 500ms control ticks to filter
 * transient fluctuations.
 *
 * Powers off whatever is running: local playback, stopped, AirPlay or a DAC mode. */
static void poll_car_mode(void) {
    car_mode_state_t *s = &car_mode_state;
    if (!current_settings.car_mode_enabled) {
        car_mode_reset_baseline(s);
    } else {
        /* Keep the sysfs directory/file reads completely out of the
         * normal 500ms control path when Car Mode is disabled. */
        /* No charge_limiter_is_holding()-based fallback for an UNKNOWN
         * read here (there used to be one): under the old on/off
         * percentage limiter, "holding" only became true once the
         * charger had actually been disabled near 85% SoC while
         * plugged in -- a reasonable proxy for "must be on external
         * power". Under the current voltage-cap design, "holding" just
         * means the 4.2V register write succeeded, which happens
         * throughout an entire charge session regardless of whether
         * power is still connected at the moment of THIS poll (the
         * AXP2101 is I2C-reachable off the system rail either way) --
         * it is no longer meaningful evidence of a physical cable.
         * Treating UNKNOWN as ambiguous (see the branch below) is the
         * existing safe default already. */
        car_mode_sample_power(s, battery_get_external_power_state());
    }

    car_mode_shutdown_if_pending(s);
}

#endif

static void update_timer_cb(lv_timer_t * timer) {
    (void) timer;
#ifdef UI_PERF_TRACE
    update_perf_begin();
#endif

    gui_external_controls_poll();

#ifndef HOST_BUILD
    poll_output_connection();
    poll_car_mode();
#endif

    screen_poll_result_t screen = poll_screen_power();

    PERF_MARK("controls");
    if (screen.on) {
        gui_shell_update_topbar(screen.just_woke);
    }
    PERF_MARK("topbar");

    poll_hardware_status();

    if (current_settings.remote_control_enabled) {
        gui_player_sync_remote_queue();
        gui_external_controls_publish_status(favorite_is_set);
    }

    poll_background_jobs();
}

void gui_control_poll_start_timers(void) {
    lv_timer_create(update_timer_cb, 500, NULL);
    remote_track_stream_set_request_notify(ui_wake_notify);
    player_ui_timer_period_ms = backlight_screen_is_on() ? 50 : 500;
    player_ui_timer = lv_timer_create(player_ui_timer_cb, player_ui_timer_period_ms, NULL);
}

void gui_control_poll_deinit(void) {
    if (player_ui_timer) {
        lv_timer_delete(player_ui_timer);
        player_ui_timer = NULL;
        player_ui_timer_period_ms = 0;
    }
}
