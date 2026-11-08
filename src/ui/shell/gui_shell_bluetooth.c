#include "gui_shell_internal.h"

static bool bt_toggle_active = false;
static bool bt_dac_power_off_pending;
static bool bt_toggle_target_enabled = false;
static bool bt_toggle_followup_pending = false;
static bool bt_toggle_followup_target_enabled = false;

/* Same treatment as refresh_wifi_icon(): the drawer's bt icon just reflects
 * powered-on/off, blue as soon as enabled -- only the top bar distinguishes
 * powered-but-nothing-paired from actually-connected, via
 * bt_control_is_connected() (checks each paired device's "Connected: yes"
 * state via bluetoothctl info, cheap -- no discovery scan). */
/* Mirrors current_settings.bt_dac_mode_enabled's real-world effect --
 * external_dac_block_reason() reads this (see its own comment) instead of
 * calling bt_control_is_powered() itself, since that's a subprocess spawn
 * (bluetoothctl show, potentially several seconds when Bluetooth actually is
 * powered on) and the block check runs on every play-button tap. Kept fresh
 * by refresh_bt_icon()'s existing periodic poll and by poll_bt_toggle()'s
 * immediate refresh after a manual toggle, rather than adding a new
 * subprocess call to the play hot path. */
bool bt_is_powered_cached = false;

/* The connected A2DP accessory's own MAC + live-negotiated codec, kept
 * fresh by the same background refresh_bt_icon_thread_func() poll as
 * bt_is_powered_cached above -- add_bt_device_row() (Bluetooth screen) uses
 * these to know which paired-device row is the actual A2DP-audio one (not
 * just "connected" -- a non-audio BLE peripheral could be connected too)
 * and what to print on its second line. Empty when nothing's A2DP-connected. */
char bt_connected_mac_cached[18] = "";
char bt_connected_codec_cached[32] = "";
unsigned int bt_connected_rate_cached = 0;

/* /usr/bin/bt_init's last line creates /tmp/bt_init_ok once chip firmware
 * flash and initialization complete. Because /tmp is tmpfs, this flag is
 * never stale from a prior boot.
 * Checked before attempting Bluetooth toggles to prevent concurrent UART
 * access while the system initialization script is running. */
#define BT_INIT_OK_FLAG_PATH "/tmp/bt_init_ok"

/* Bluetooth status is unknown, not off, while the stock asynchronous
 * S80_bt_init job is still flashing/attaching the controller.  No status
 * subprocess may start before its tmpfs completion marker appears: doing so
 * captures the real temporary powered state and flashes the icon on the first
 * Home frames.  This latch is polled with access() from the existing 500ms UI
 * timer; once true it stays true for this process lifetime and normal
 * authoritative background polling begins immediately. */
bool bt_startup_ready = false;

/* On AIC8800D40 units bt_init leaves the controller off and never writes the
 * marker; the boot-time driver load finishing is the equivalent signal. */
static bool bt_init_marker_present(void) {
    return access(BT_INIT_OK_FLAG_PATH, F_OK) == 0 || aic_radio_boot_settled();
}
static bool refresh_bt_startup_readiness(void) {
    if (!bt_startup_ready && bt_init_marker_present())
        bt_startup_ready = true;
    return bt_startup_ready;
}

/* Armed only by an app-driven enable path, never by the periodic boot-state
 * poll itself. poll_refresh_bt_icon() consumes it once that existing poll
 * reports an authoritative powered state. This adds no subprocesses to the
 * toggle worker, and hci0's transient boot-time powered window cannot arm
 * AVRCP by itself. */
static atomic_bool bt_media_player_enable_pending = false;
static void mark_bt_media_player_enable_pending(void) {
    atomic_store_explicit(&bt_media_player_enable_pending, true, memory_order_release);
}

/* Shared Bluetooth device scan results, referenced by both
 * refresh_bt_icon_thread_func() and the Bluetooth settings screen
 * (populate_bt_screen()). */
#define BT_MAX_RESULTS 32
bt_device_t bt_scan_results[BT_MAX_RESULTS];
int bt_scan_result_count = 0;

/* Written by refresh_bt_icon_thread_func() below, merged into
 * bt_scan_results by poll_refresh_bt_icon() -- see its own comment. */
static bt_device_t bt_paired_states_result[BT_MAX_RESULTS];
static int bt_paired_states_count = 0;

/* Bluetooth status checks (bt_control_is_powered / bt_control_is_connected)
 * run asynchronously in a worker thread to prevent bluetoothctl subprocess
 * execution from blocking the UI thread during streaming or slow state
 * transitions. */
static shell_job_t refresh_bt_icon_job;
static bool refresh_bt_icon_result_powered = false;
static bool refresh_bt_icon_result_connected = false;
static bool refresh_bt_icon_result_a2dp_connected = false;
static char refresh_bt_icon_result_mac[18] = "";
static char refresh_bt_icon_result_codec[32] = "";
static unsigned int refresh_bt_icon_result_rate = 0;

/* UI-thread owned Bluetooth audio state and disconnect generation latch */
bool bt_is_a2dp_connected_ui = false;
static uint32_t bt_disconnect_epoch = 0;
static uint32_t bt_worker_launch_epoch = 0;
static uint32_t bt_preference_epoch = 0;
static uint32_t bt_worker_preference_epoch = 0;
/* Bumped when a toggle finishes. A status check launched before that read
 * the pre-toggle power state, so its result is discarded and re-run. */
static uint32_t bt_toggle_epoch = 0;
static uint32_t bt_worker_toggle_epoch = 0;

/* Snapshotted on the UI thread in start_refresh_bt_icon(), before the worker
 * launches -- gui_navigation_is_top()/gui_network_get_bt_screen() touch the
 * navigation stack and must not be called from refresh_bt_icon_thread_func()
 * itself (a background thread), same reasoning as every other UI-state
 * value this worker reads via a snapshot rather than a live getter. */
static bool bt_worker_bt_screen_visible = false;

/* One persisted-device reconnect attempt is armed per authoritative
 * powered-on cycle. A link loss alone must not re-arm it; only a later
 * off->on observation (or the initial powered-on observation at startup)
 * starts a new cycle. */
static bool bt_reconnect_observed_powered = false;
static bool bt_reconnect_observed_power_valid = false;
static bool bt_reconnect_pending = false;
static bool bt_reconnect_cycle_suppressed = false;
static uint32_t bt_power_status_generation = 0;
static void cancel_bt_reconnect_for_cycle(bool suppress_cycle) {
    bt_reconnect_cancel();
    bt_reconnect_pending = false;
    if (suppress_cycle) bt_reconnect_cycle_suppressed = true;
}
void gui_shell_cancel_bt_reconnect(void) {
    /* Invalidate preference updates from an in-flight refresh. Manual
     * actions (especially Forget) must not let a pre-action positive A2DP
     * result repopulate the remembered preference when that worker returns.
     * Keep this separate from the audio-route epoch: cancelling a reconnect
     * is not evidence that the current headphones disconnected. */
    bt_preference_epoch++;
    cancel_bt_reconnect_for_cycle(true);
}

/* Forward declaration: route teardown is also required by immediate
 * disconnect/power-off paths, before the next authoritative poll lands. */
static void clear_bt_audio_route_now(void);
bool gui_shell_is_bt_audio_connected(void) { return bt_is_a2dp_connected_ui; }
bool gui_shell_get_bt_audio_codec(char * out, size_t out_size) {
    if (!out || out_size == 0 || !bt_is_a2dp_connected_ui || !bt_connected_codec_cached[0]) return false;
    snprintf(out, out_size, "%s", bt_connected_codec_cached);
    return out[0] != '\0';
}
static void bt_drop_audio_link(void) {
    bt_disconnect_epoch++;
    bt_is_a2dp_connected_ui = false;
    bt_connected_mac_cached[0] = '\0';
    bt_connected_codec_cached[0] = '\0';
}
static void topbar_hide_bt_audio_icons(void) {
    if (a2dp_status_icon) lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    if (bt_codec_status_icon) lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    invalidate_bt_codec_status_cache();
}
void gui_shell_notify_bt_audio_disconnected(void) {
    bt_drop_audio_link();
    bt_connected_rate_cached = 0;
    clear_bt_audio_route_now();
    topbar_hide_bt_audio_icons();
    sync_bt_codec_status_icon();
}
static void * refresh_bt_icon_thread_func(void * arg) {
    (void) arg;
    bool powered = bt_control_is_powered();
    refresh_bt_icon_result_powered = powered;

    /* Same background thread/cadence as everything else here -- one more
     * subprocess call (bluealsactl list-pcms) alongside the bluetoothctl
     * calls below, not a separate poll loop. */
    refresh_bt_icon_result_a2dp_connected = powered && bt_control_is_a2dp_source_connected();
    if (powered && !refresh_bt_icon_result_a2dp_connected) {
        /* A sample-rate change recreates the BlueALSA PCM (observed: 90 ms
         * from removal to replacement). A poll in that gap must not clear
         * the route or stop the disconnect watcher. Confirm absence after
         * the same grace interval as the watcher, entirely off the UI thread. */
        usleep(BT_OUTPUT_RECONFIGURE_GRACE_MS * 1000U);
        refresh_bt_icon_result_a2dp_connected = bt_control_is_a2dp_source_connected();
        /* A codec switch of ours can take up to 3 s to bring the PCM back
         * (BT_SSC_SWITCH_CAP_MS): keep the route while one is in flight. */
        for (uint32_t hold = bt_control_reconfigure_hold_ms();
             !refresh_bt_icon_result_a2dp_connected && hold > 0;
             hold = bt_control_reconfigure_hold_ms()) {
            usleep((hold < 250 ? hold : 250) * 1000U);
            refresh_bt_icon_result_a2dp_connected = bt_control_is_a2dp_source_connected();
        }
    }

    /* Two more subprocess calls (bluealsactl info, reusing the same PCM
     * path lookup bt_control_is_a2dp_source_connected() just did) -- only
     * worth paying when something's actually A2DP-connected. Both left at
     * "" (not stale) when nothing is, so add_bt_device_row() never shows a
     * leftover codec line for a device that just disconnected. */
    refresh_bt_icon_result_mac[0] = '\0';
    refresh_bt_icon_result_codec[0] = '\0';
    refresh_bt_icon_result_rate = 0;
    if (refresh_bt_icon_result_a2dp_connected) {
        bt_control_get_connected_device_mac(refresh_bt_icon_result_mac, sizeof(refresh_bt_icon_result_mac));
        bt_control_get_connected_device_stream(refresh_bt_icon_result_codec, sizeof(refresh_bt_icon_result_codec),
                                              &refresh_bt_icon_result_rate);
    }

    if (powered) {
        if (bt_worker_bt_screen_visible) {
            /* bt_control_list_paired_states(), not bt_control_is_connected() --
             * same per-device `bluetoothctl info` cost either way, but this also
             * hands back the full breakdown poll_refresh_bt_icon() merges into
             * bt_scan_results below, instead of throwing it away. -1 (the query
             * itself failed, not "genuinely 0 paired") is normalized to 0 here
             * for the any_connected scan below (an empty loop either way), but
             * poll_refresh_bt_icon() checks the raw value separately before
             * treating "nothing here" as authoritative -- see its own comment. */
            bt_paired_states_count = bt_control_list_paired_states(bt_paired_states_result, BT_MAX_RESULTS);
            bool any_connected = false;
            for (int i = 0; i < bt_paired_states_count; i++) {
                if (bt_paired_states_result[i].connected) { any_connected = true; break; }
            }
            refresh_bt_icon_result_connected = any_connected;
        } else {
            /* Bluetooth screen isn't open, so nothing reads the per-device
             * breakdown this cycle -- skip the O(paired devices) fork loop
             * entirely and ask for just the boolean the topbar icon actually
             * needs. -1 (not -- as opposed to 0 devices/none connected --
             * refreshed this cycle) makes poll_refresh_bt_icon() skip the
             * bt_scan_results merge below and keep whatever it last had,
             * same "skip merge, retain current state" convention the -1
             * query-failure case above already relies on. */
            bt_paired_states_count = -1;
            int any_connected_now = bt_control_any_paired_connected();
            /* -1 means this cycle couldn't tell (see the .c file's own
             * comment) -- leave refresh_bt_icon_result_connected at its
             * last known value instead of guessing "nothing connected". */
            if (any_connected_now >= 0) refresh_bt_icon_result_connected = any_connected_now != 0;
        }
    } else {
        bt_paired_states_count = 0;
        refresh_bt_icon_result_connected = false;
    }

    atomic_store_explicit(&refresh_bt_icon_job.done, true, memory_order_release); /* written last -- poll_refresh_bt_icon only checks this flag */
    return NULL;
}
void start_refresh_bt_icon(void) {
    if (!refresh_bt_startup_readiness()) return;
    if (refresh_bt_icon_job.active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    bt_worker_launch_epoch = bt_disconnect_epoch;
    bt_worker_preference_epoch = bt_preference_epoch;
    bt_worker_toggle_epoch = bt_toggle_epoch;
    bt_worker_bt_screen_visible = gui_navigation_is_top(gui_network_get_bt_screen());
    shell_job_start(&refresh_bt_icon_job, refresh_bt_icon_thread_func, NULL);
}

/* Only this lifecycle worker starts/stops the monitors. A rapid reconnect
 * updates the desired state without making LVGL join a live monitor. */
static shell_job_t bt_monitor_lifecycle_job;
static bool bt_monitor_want_output, bt_monitor_want_volume;
static bool bt_monitor_job_output, bt_monitor_job_volume;
static bool bt_monitor_applied_output, bt_monitor_applied_volume;
static bool bt_monitor_refresh_requested;
static void clear_bt_audio_route_now(void) {
    audio_set_bt_output(false);
    usb_dac_bridge_set_bt_output(false);
    bt_monitor_want_output = false;
    bt_monitor_want_volume = false;
    /* A desired/applied mismatch already schedules the required stop after an
     * in-flight lifecycle job completes. Only force a retry when no lifecycle
     * job is active but a monitor is still actually running; repeated
     * authoritative-off polls must not spawn redundant stop workers. */
    bt_monitor_refresh_requested = !bt_monitor_lifecycle_job.active &&
        (bt_control_source_volume_sync_is_running() ||
         bt_control_output_disconnect_watch_is_running());
    poll_bt_monitor_lifecycle();
}
static void * bt_monitor_lifecycle_worker(void * unused) {
    (void)unused;
    if (bt_monitor_job_volume) bt_control_source_volume_sync_start();
    else bt_control_source_volume_sync_stop();
    if (bt_monitor_job_output) bt_control_output_disconnect_watch_start();
    else bt_control_output_disconnect_watch_stop();
    atomic_store_explicit(&bt_monitor_lifecycle_job.done, true, memory_order_release);
    return NULL;
}
void poll_bt_monitor_lifecycle(void) {
    if (bt_monitor_lifecycle_job.active) {
        if (!shell_job_reap(&bt_monitor_lifecycle_job)) return;
        bt_monitor_applied_output = bt_monitor_job_output;
        bt_monitor_applied_volume = bt_monitor_job_volume;
    }
    if (!bt_monitor_refresh_requested && bt_monitor_want_output == bt_monitor_applied_output &&
        bt_monitor_want_volume == bt_monitor_applied_volume) return;
    bt_monitor_job_output = bt_monitor_want_output;
    bt_monitor_job_volume = bt_monitor_want_volume;
    if (shell_job_start(&bt_monitor_lifecycle_job, bt_monitor_lifecycle_worker, NULL))
        bt_monitor_refresh_requested = false;
}

/* A new power cycle: observed OFF, with any suppression cleared. */
static void bt_reconnect_begin_new_cycle(void) {
    bt_reconnect_observed_power_valid = true;
    bt_reconnect_observed_powered = false;
    bt_reconnect_cycle_suppressed = false;
}
void poll_refresh_bt_icon(void) {
    if (!shell_job_reap(&refresh_bt_icon_job)) return;

    /* While a manual toggle is in-flight (bt_toggle_active), ignore the
     * background poll result to prevent overwriting the optimistic state
     * with stale pre-toggle hardware readings. */
    if (bt_toggle_active) return;
    if (bt_worker_toggle_epoch != bt_toggle_epoch) {
        start_refresh_bt_icon();
        return;
    }

    bool display_powered = refresh_bt_icon_result_powered;

    bool powered_rising = display_powered &&
        (!bt_reconnect_observed_power_valid || !bt_reconnect_observed_powered);
    if (!display_powered) {
        /* An authoritative power-off ends the old cycle. Clear suppression so
         * the next real power-on can arm a fresh, single reconnect attempt. */
        cancel_bt_reconnect_for_cycle(false);
        bt_reconnect_begin_new_cycle();
    } else {
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = true;
        if (powered_rising && !bt_reconnect_cycle_suppressed &&
            !current_settings.bt_dac_mode_enabled)
            bt_reconnect_pending = true;
    }

#ifndef HOST_BUILD
    if (display_powered &&
        atomic_exchange_explicit(&bt_media_player_enable_pending, false, memory_order_acq_rel))
        bt_media_player_init();
#endif

    /* Discard stale A2DP/codec results if a disconnect occurred while or after the worker launched */
    bool a2dp_connected = display_powered && refresh_bt_icon_result_a2dp_connected;
    if (bt_worker_launch_epoch != bt_disconnect_epoch) {
        a2dp_connected = false;
    }

    /* Confirmed off->on, not the optimistic flip: BlueZ answers by now. */
    static bool last_confirmed_powered;
    bool bt_turned_on = display_powered && !last_confirmed_powered;
    last_confirmed_powered = display_powered;
    bt_is_powered_cached = display_powered;
    bt_is_a2dp_connected_ui = a2dp_connected;
    bt_power_status_generation++;

    char previous_mac[sizeof(bt_connected_mac_cached)];
    snprintf(previous_mac, sizeof(previous_mac), "%s", bt_connected_mac_cached);
    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", a2dp_connected ? refresh_bt_icon_result_mac : "");
    if (strcmp(previous_mac, bt_connected_mac_cached) != 0) {
        /* The right transport rate belongs to the accessory, so the effective
         * one follows whatever is connected: 44.1 kHz headphones and a 96 kHz
         * LDAC speaker each keep their own. Only on a real change, so a
         * rate being applied right now is not overwritten by a routine
         * refresh that happens to see the link mid-cycle. */
        bt_control_set_sample_rate(settings_bt_rate_for(&current_settings, bt_connected_mac_cached));
        gui_network_notify_bt_device_changed();
    }
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", a2dp_connected ? refresh_bt_icon_result_codec : "");
    bt_connected_rate_cached = a2dp_connected ? refresh_bt_icon_result_rate : 0;
    if (quick_drawer_bt_icon) {
        if (quick_drawer_set_toggle_image(quick_drawer_bt_icon,
                                          quick_drawer_toggle_src(QD_TOGGLE_BT, display_powered)))
            quick_drawer_mark_snapshot_dirty();
    }
    quick_drawer_set_toggle_state(1, display_powered);

    if (!display_powered) {
        clear_bt_audio_route_now();
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        topbar_hide_bt_audio_icons();
        sync_topbar_status_icon_positions();
        /* Bluetooth screen's own toggle row + everything gated on it reads
         * bt_is_powered_cached too -- rebuilt only while that screen is visible. */
        if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
        return;
    }
    lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    if (topbar_set_image(bt_status_icon, topbar_icon(refresh_bt_icon_result_connected ?
                         "bluetooth" : "bluetooth_off")))
        layout_bluetooth_topbar_image(bt_status_icon);
    sync_topbar_status_icon_positions();
    lv_obj_set_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN, !a2dp_connected);
    sync_bt_codec_status_icon();

    /* Update scan results in-place by MAC match against the fresh paired states
     * snapshot from the background poll. If bt_paired_states_count is negative,
     * the query failed, so skip merge to retain current state. */
    if (bt_paired_states_count >= 0) {
        for (int j = 0; j < bt_scan_result_count; j++) {
            bool found = false;
            for (int i = 0; i < bt_paired_states_count; i++) {
                if (strcmp(bt_scan_results[j].mac, bt_paired_states_result[i].mac) == 0) {
                    bt_scan_results[j].paired = bt_paired_states_result[i].paired;
                    bt_scan_results[j].connected = bt_paired_states_result[i].connected;
                    found = true;
                    break;
                }
            }
            if (!found) {
                bt_scan_results[j].paired = false;
                bt_scan_results[j].connected = false;
            }
        }
    }
    /* Rebuild Bluetooth screen only when it is currently on top to avoid
     * UI stutter on other screens. */
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
    if (bt_turned_on) gui_network_bt_powered_on();

    /* Route audio to Bluetooth when connected, unless Bluetooth DAC mode is
     * enabled (which runs bluealsa as an A2DP sink rather than source). */
    bool use_bt_output = a2dp_connected && !current_settings.bt_dac_mode_enabled;
    audio_set_bt_output(use_bt_output);

    /* Volume synchronization with Bluetooth audio output devices. */
    /* Keep the BlueALSA source monitor alive for local software gain too;
     * only its inbound AVRCP-to-player direction is mode-gated. */
    bt_monitor_want_volume = use_bt_output;
    bt_monitor_want_output = use_bt_output;
    /* Retry missing monitors, without creating a thread on every healthy
     * radio poll. Failed starts and unexpected EOF are both retried here. */
    bt_monitor_refresh_requested =
        (bt_monitor_want_volume && !bt_control_source_volume_sync_is_running()) ||
        (bt_monitor_want_output && !bt_control_output_disconnect_watch_is_running());
    poll_bt_monitor_lifecycle();

    /* Mirror Bluetooth output setting to USB DAC bridge when active. */
    usb_dac_bridge_set_bt_output(use_bt_output);

    /* Persist only a verified A2DP snapshot from this refresh worker. The
     * epoch check above prevents a stale pre-disconnect result from becoming
     * the remembered output device. Disconnections intentionally retain the
     * preference until the user forgets it. */
    if (a2dp_connected && bt_worker_preference_epoch == bt_preference_epoch &&
        refresh_bt_icon_result_mac[0] &&
        strcmp(current_settings.bt_last_output_mac, refresh_bt_icon_result_mac) != 0 &&
        strlen(refresh_bt_icon_result_mac) == 17) {
        snprintf(current_settings.bt_last_output_mac,
                 sizeof(current_settings.bt_last_output_mac), "%s", refresh_bt_icon_result_mac);
        settings_save_async(&current_settings);
    }
}
static bool quick_drawer_bt_long_press_fired = false;
/* See quick_drawer_wifi_pressed_cb(). */
void quick_drawer_bt_pressed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    quick_drawer_bt_long_press_fired = false;
}
void quick_drawer_bt_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_bt_long_press_fired = true;
    quick_drawer_dismiss_for_navigation();
    open_bluetooth_screen();
}

/* Same real tap-to-toggle treatment for Bluetooth, mirroring the wifi
 * mechanism above -- bluetoothctl's power on/off each block for about a
 * second, so this runs on its own thread too. Turning ON additionally
 * brings up the chip first via bt_control_init_chip() if it isn't already
 * (no-op once hci0 exists) -- see that function's own comment for why this,
 * not just bluetoothctl power on, is what actually makes the toggle work at
 * all on a fresh boot. */
static pthread_t bt_toggle_thread;
static atomic_bool bt_toggle_done_flag = false;


/* Set by bt_toggle_thread_func() when disabling Bluetooth while DAC mode
 * was on -- consumed by poll_bt_toggle() to turn the setting off and close
 * the DAC overlay screen if it's the one currently showing. */
static bool bt_toggle_forced_dac_off = false;

/* Encoded pointer values avoid allocating a one-bool thread argument. NULL
 * remains available for legacy/inferred callers, though all current launch
 * sites pass an explicit target so the worker never needs a potentially
 * 15-second bluetoothctl query just to decide which operation to perform. */
#define BT_TOGGLE_TARGET_ON  ((void *) (intptr_t) 1)
#define BT_TOGGLE_TARGET_OFF ((void *) (intptr_t) 2)
static void * bt_toggle_target_arg(bool enabled) {
    return enabled ? BT_TOGGLE_TARGET_ON : BT_TOGGLE_TARGET_OFF;
}
static void * bt_toggle_thread_func(void * arg) {
    bt_toggle_forced_dac_off = false;
    bool turning_on;
    if (arg == BT_TOGGLE_TARGET_ON) turning_on = true;
    else if (arg == BT_TOGGLE_TARGET_OFF) turning_on = false;
    else turning_on = !bt_control_is_powered();
    bool chip_wedged = false;
    if (!turning_on) {
        atomic_store_explicit(&bt_media_player_enable_pending, false, memory_order_release);
        /* Leave DAC mode before powering down: stop playback and restore
         * the source profile while keeping the shared pairing agent. */
        if (current_settings.bt_dac_mode_enabled) {
            bt_control_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
            bt_toggle_forced_dac_off = true;
        }
        bt_control_disable();
    } else {
        /* If chip initialization fails, skip calling bt_control_enable() to
         * avoid redundant timeouts against a non-existent controller. */
        if (bt_control_init_chip()) {
            bt_control_enable();
            mark_bt_media_player_enable_pending();
        } else chip_wedged = true;
    }

    /* Do not confirm via bt_control_is_powered() here. Each status query has
     * a legitimate 15-second timeout; after resume, Bluetooth was already
     * usable while several such confirmations kept bt_toggle_active true
     * for ~30 seconds and caused every disable tap to be discarded. Give
     * bluetoothd one short propagation interval, then let the existing
     * asynchronous authoritative refresh confirm/correct the optimistic UI.
     * A queued opposite request can start as soon as this worker is reaped. */
    if (!chip_wedged) usleep(500000);

    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

/* Queues Bluetooth enable intent if requested while bt_init is still running.
 * Waits for BT_INIT_OK_FLAG_PATH and stable off state before executing
 * the chip initialization and enable sequence, providing immediate visual
 * feedback via optimistic icon updates. */
#define BT_BOOT_ENABLE_MAX_WAIT_MS 30000
#define BT_BOOT_ENABLE_POLL_INTERVAL_MS 300
#define BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED 2
static void * bt_pending_enable_thread_func(void * arg) {
    (void) arg;
    uint32_t waited_ms = 0;
    unsigned int off_observations = 0;
    bool init_finished = false;
    while (waited_ms < BT_BOOT_ENABLE_MAX_WAIT_MS) {
        if (bt_init_marker_present()) {
            init_finished = true;
            if (!bt_control_is_powered()) {
                off_observations++;
                if (off_observations >= BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
                    if (bt_control_init_chip()) {
                        bt_control_enable();
                        mark_bt_media_player_enable_pending();
                    }
                    break;
                }
            } else {
                off_observations = 0;
            }
        }
        usleep(BT_BOOT_ENABLE_POLL_INTERVAL_MS * 1000);
        waited_ms += BT_BOOT_ENABLE_POLL_INTERVAL_MS;
    }
    /* If initialization finished but its state never produced two clean
     * off samples before the bounded wait elapsed, assert the requested
     * final state once anyway. Never do this without bt_init_ok: that would
     * reintroduce the unsafe concurrent UART initialization race. */
    if (init_finished && off_observations < BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
        if (bt_control_init_chip()) {
            bt_control_enable();
            mark_bt_media_player_enable_pending();
        }
    }
    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}
static void show_optimistic_bt_state(bool powered) {
    quick_drawer_apply_toggle_image(QD_TOGGLE_BT, quick_drawer_bt_icon, powered);
    quick_drawer_mark_snapshot_dirty();

    bt_drop_audio_link();
    if (powered) {
        lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        if (topbar_set_image(bt_status_icon, topbar_icon("bluetooth_off")))
            layout_bluetooth_topbar_image(bt_status_icon);
    } else {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    topbar_hide_bt_audio_icons();
    sync_topbar_status_icon_positions();

    bt_is_powered_cached = powered;
    if (!powered) clear_bt_audio_route_now();
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
}
void quick_drawer_bt_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_bt_long_press_fired) { /* see quick_drawer_bt_long_press_cb()'s own comment */
        quick_drawer_bt_long_press_fired = false;
        return;
    }
    gui_shell_bt_request_power(!bt_is_powered_cached);
}

void gui_shell_bt_request_power(bool bt_will_be_powered) {
    if (bt_will_be_powered) bt_dac_power_off_pending = false;
    if (!bt_will_be_powered && (current_settings.bt_dac_mode_enabled || gui_shell_bt_dac_busy())) {
        bt_dac_power_off_pending = true;
        gui_shell_bt_dac_remember(true);
        current_settings.bt_dac_mode_enabled = false;
        gui_shell_bt_dac_release();
        start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        return;
    }
    if (current_settings.bt_last_on != bt_will_be_powered) {
        current_settings.bt_last_on = bt_will_be_powered;
        settings_save_async(&current_settings);
    }

    if (!bt_will_be_powered) {
        current_settings.bt_dac_mode_enabled = false;
        gui_shell_bt_dac_release();
        /* Explicit OFF is user intent immediately, before the slow toggle
         * worker has produced its next authoritative snapshot. */
        gui_shell_cancel_bt_reconnect();
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = false;
    } else {
        /* A rapid user OFF->ON is a new requested cycle even if the polling
         * cadence did not happen to observe the intermediate OFF state. */
        bt_reconnect_begin_new_cycle();
    }

    /* Last intent wins while a slow resume/enable is still finishing. The
     * running operation cannot be safely interrupted while it owns the chip
     * mutex, but an opposite tap is remembered and launched immediately
     * after it completes instead of being silently discarded. Repeated taps
     * collapse back to the in-flight target when appropriate. */
    if (bt_toggle_active) {
        bt_toggle_followup_target_enabled = bt_will_be_powered;
        bt_toggle_followup_pending = (bt_will_be_powered != bt_toggle_target_enabled);
        show_optimistic_bt_state(bt_will_be_powered);
        return;
    }

    /* If turning on before /tmp/bt_init_ok exists, queue behind
     * bt_pending_enable_thread_func() to wait for initialization to complete.
     * Disabling is D-Bus-only and safe to run directly. */
    bool bt_pending_now = bt_will_be_powered && !bt_init_marker_present();

    bt_toggle_active = true;
    bt_toggle_target_enabled = bt_will_be_powered;
    bt_toggle_followup_pending = false;
    atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);

    /* Optimistically update topbar and drawer icons, and rebuild the Bluetooth
     * settings screen if visible. Confirmed by authoritative poll on completion. */
    show_optimistic_bt_state(bt_will_be_powered);

    /* Runs fully in the background, same as the stock player -- no busy
     * screen. */
    void * (*thread_func)(void *) = bt_pending_now ? bt_pending_enable_thread_func : bt_toggle_thread_func;
    void * thread_arg = bt_pending_now ? NULL : bt_toggle_target_arg(bt_will_be_powered);
    if (pthread_create(&bt_toggle_thread, NULL, thread_func, thread_arg) != 0) {
        bt_toggle_active = false;
        bt_is_powered_cached = !bt_will_be_powered;
        if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
        show_info_toast(TR("Failed to toggle Bluetooth"));
    }
}
void poll_bt_toggle(void) {
    if (!bt_toggle_active || !atomic_load_explicit(&bt_toggle_done_flag, memory_order_acquire)) return;
    bt_toggle_active = false;
    pthread_join(bt_toggle_thread, NULL);
    bt_toggle_epoch++;

    if (bt_toggle_forced_dac_off) {
        current_settings.bt_dac_mode_enabled = false;
        settings_save(&current_settings);

    }

    if (bt_toggle_followup_pending) {
        bool target = bt_toggle_followup_target_enabled;
        bt_toggle_followup_pending = false;
        bt_toggle_target_enabled = target;
        bt_toggle_active = true;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func,
                           bt_toggle_target_arg(target)) == 0)
            return;
        bt_toggle_active = false;
        show_info_toast(TR("Failed to toggle Bluetooth"));
    }

    /* start_refresh_bt_icon() only starts the background check -- it
     * hasn't updated bt_is_powered_cached yet by the time this returns, so
     * populate_bt_screen() (which now reads that cache, not a fresh
     * bt_control_is_powered() call -- see its own comment) can't be called
     * here too or the Bluetooth screen's toggle row would show stale
     * (pre-toggle) state until something else happens to repopulate it.
     * poll_refresh_bt_icon() calls populate_bt_screen() itself once the
     * cache is actually fresh. */
    start_refresh_bt_icon(); /* re-reads the real state -- updates the status bar/drawer icons and (once done) the Bluetooth screen's toggle row */
}

/* Applies bt_dac_mode_enabled configuration asynchronously during the
 * startup window, launching the necessary bluealsa and bt-agent processes
 * without blocking the UI thread. */
static shell_job_t bt_dac_startup_reapply_job;
static bool bt_dac_startup_reapply_started = false;
static void * bt_dac_startup_reapply_thread_func(void * arg) {
    (void) arg;
    bt_control_init_chip();
    bt_control_enable();
    mark_bt_media_player_enable_pending();
    bt_control_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    atomic_store_explicit(&bt_dac_startup_reapply_job.done, true, memory_order_release); /* written last -- poll_bt_dac_startup_reapply only checks this flag */
    return NULL;
}

/* Runs at most once, and only if DAC mode was switched on before the
 * Bluetooth startup gate opened. bt_dac_mode_enabled is not persisted, so
 * this never fires on a normal boot; a toggle-on tap after that gate goes
 * through the Bluetooth DAC toggle directly and doesn't need this. */
void start_bt_dac_startup_reapply_if_needed(void) {
    if (!current_settings.bt_dac_mode_enabled || bt_dac_startup_reapply_started ||
        !refresh_bt_startup_readiness()) return;
    bt_dac_startup_reapply_started = true;
    if (!shell_job_start(&bt_dac_startup_reapply_job, bt_dac_startup_reapply_thread_func, NULL))
        bt_dac_startup_reapply_started = false;
}
void poll_bt_dac_startup_reapply(void) {
    if (!shell_job_reap(&bt_dac_startup_reapply_job)) return;
    start_refresh_bt_icon();
}

/* Asynchronously executes bt_control_apply_output_settings() on a background
 * thread to avoid blocking the UI thread with process restarts (bluealsa,
 * bt-agent, bluealsa-aplay) and sleeps. */
static shell_job_t bt_apply_output_settings_job;
static shell_job_t bt_source_codec_reconcile_job;
static bool bt_source_codec_reconcile_started = false;
static bool bt_source_codec_reconcile_succeeded = false;
static bool bt_source_codec_reconcile_retry_pending = true;
static uint32_t bt_source_codec_reconcile_failed_generation = 0;
static void * bt_source_codec_reconcile_thread_func(void * arg) {
    (void) arg;
    bool success = false;
    if (!current_settings.bt_dac_mode_enabled && bt_control_is_powered())
        success = bt_control_reconcile_source_settings();
    bt_source_codec_reconcile_succeeded = success;
    bt_source_codec_reconcile_retry_pending = !success;
    atomic_store_explicit(&bt_source_codec_reconcile_job.done, true, memory_order_release);
    return NULL;
}
void start_bt_source_codec_reconcile_if_needed(void) {
    if (bt_source_codec_reconcile_job.active || bt_source_codec_reconcile_succeeded ||
        current_settings.bt_dac_mode_enabled ||
        bt_reconnect_busy() || gui_network_bt_user_operation_active() ||
        !refresh_bt_startup_readiness()) return;
    /* The first attempt is allowed to query the authoritative state even if
     * the cached status is still false. After a transient failure, wait for a
     * newer completed status refresh before retrying; this avoids a retry
     * storm while still recovering when Bluetooth becomes ready later. A
     * deferred reconcile is different: while the cached A2DP link remains
     * up, retrying every newer status generation would just repeat the
     * connected-accessory guard. Wait for the cached link to go away before
     * trying again, so the next attempt can actually restart the daemon. */
    if (bt_source_codec_reconcile_started &&
        (!bt_source_codec_reconcile_retry_pending ||
         bt_source_codec_reconcile_failed_generation == bt_power_status_generation ||
         !bt_is_powered_cached ||
         gui_shell_is_bt_audio_connected())) return;
    bt_source_codec_reconcile_started = true;
    if (!shell_job_start(&bt_source_codec_reconcile_job, bt_source_codec_reconcile_thread_func, NULL)) {
        bt_source_codec_reconcile_retry_pending = true;
        bt_source_codec_reconcile_failed_generation = bt_power_status_generation;
    }
}
void poll_bt_source_codec_reconcile(void) {
    if (!shell_job_reap(&bt_source_codec_reconcile_job)) return;
    if (!bt_source_codec_reconcile_succeeded)
        bt_source_codec_reconcile_failed_generation = bt_power_status_generation;
}
void poll_bt_reconnect(void) {
    bt_reconnect_poll();
    if (bt_ssc_settings_sync(&current_settings) | bt_rc_trust_settings_sync(&current_settings))
        settings_save_async(&current_settings);
    char correction_mac[18];
    unsigned int correction_rate = 0;
    if (bt_control_ssc_discovery_recovery_peek(correction_mac, sizeof(correction_mac))) {
        /* Keyed on the pending device's own MAC, not on the top bar's cached
         * one or its power observation: those are refreshed only while the
         * screen is on, and the recovery must behave the same with it off. */
        bool blocked = current_settings.bt_dac_mode_enabled ||
                       bt_toggle_active || bt_source_codec_reconcile_job.active ||
                       bt_dac_startup_reapply_job.active || bt_apply_output_settings_job.active ||
                       bt_reconnect_busy() || gui_network_bt_user_operation_active();
        if (blocked) return;
        if (bt_ssc_policy_recovery_verdict(current_settings.bt_codec, bt_connected_mac_cached,
                                           correction_mac) == BT_SSC_RECOVERY_CANCEL) {
            bt_control_ssc_discovery_recovery_cancel();
        } else {
            (void) bt_reconnect_start_ssc_discovery(correction_mac,
                settings_bt_rate_for(&current_settings, correction_mac));
            return;
        }
    }
    if (bt_control_ssc_rate_correction_peek(correction_mac, sizeof(correction_mac), &correction_rate)) {
        bool blocked = !bt_reconnect_observed_powered || current_settings.bt_dac_mode_enabled ||
                       bt_toggle_active || bt_source_codec_reconcile_job.active ||
                       bt_dac_startup_reapply_job.active || bt_apply_output_settings_job.active ||
                       bt_reconnect_busy() || gui_network_bt_user_operation_active();
        if (blocked) return;
        if (bt_connected_mac_cached[0] &&
                (strcmp(correction_mac, bt_connected_mac_cached) != 0 ||
                 settings_bt_rate_for(&current_settings, correction_mac) != correction_rate)) {
            bt_control_ssc_rate_correction_cancel();
            return;
        }
        if (bt_connected_mac_cached[0]) {
            (void) bt_reconnect_start_ssc_rate_correction(correction_mac, correction_rate);
            return;
        }
    }
    if (!bt_reconnect_pending || bt_reconnect_cycle_suppressed ||
        bt_reconnect_busy() || !bt_reconnect_observed_powered ||
        current_settings.bt_dac_mode_enabled || bt_toggle_active ||
        bt_source_codec_reconcile_job.active ||
        bt_dac_startup_reapply_job.active || bt_apply_output_settings_job.active)
        return;

    /* Consume the arm before starting. A failed start or exhausted worker is
     * deliberately not retried until a new authoritative power cycle. */
    bt_reconnect_pending = false;
    const char * preferred_mac = current_settings.bt_last_output_mac[0] ?
                                 current_settings.bt_last_output_mac : NULL;
    unsigned int rate = preferred_mac ? settings_bt_rate_for(&current_settings, preferred_mac) :
                        current_settings.bt_sample_rate;
    bt_reconnect_start_with_rate(preferred_mac, rate);
}
static bool bt_apply_succeeded;
static bool bt_profile_applied;
static bool bt_profile_failed;
typedef struct {
    bool dac_mode_enabled;
    bool volume_sync_enabled;
    bool close_source;
} bt_apply_output_settings_request_t;
static void * bt_apply_output_settings_thread_func(void * arg) {
    bt_apply_output_settings_request_t * req = (bt_apply_output_settings_request_t *) arg;
    int waited = 0;
    while (req->close_source && !audio_is_idle() && waited < 2000) { usleep(10000); waited += 10; }
    bt_apply_succeeded = (!req->close_source || audio_is_idle()) &&
        bt_control_apply_output_settings(req->dac_mode_enabled, req->volume_sync_enabled);
    free(req);
    atomic_store_explicit(&bt_apply_output_settings_job.done, true, memory_order_release); /* written last -- poll_bt_apply_output_settings only checks this flag */
    return NULL;
}

/* Overlap (another apply already in flight) is not queued: the caller has
 * already written current_settings, so the request is remembered and
 * poll_bt_apply_output_settings() re-applies the then-current settings once
 * the in-flight apply lands. Only a request that differs from the one in
 * flight needs that; a repeat of it is a no-op. A failed launch is
 * remembered the same way and retried by the poll. */
static bool bt_apply_output_settings_rerun;
static bool bt_apply_inflight_dac_mode;
static bool bt_apply_inflight_volume_sync;

void start_bt_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    if (!dac_mode_enabled) gui_shell_bt_dac_release();
    if (gui_shell_bt_dac_owns_output()) { bt_apply_output_settings_rerun = true; return; }
#ifndef HOST_BUILD
    bt_media_player_set_phone_control_enabled(dac_mode_enabled);
#endif
    if (bt_apply_output_settings_job.active) {
        bt_apply_output_settings_rerun = dac_mode_enabled != bt_apply_inflight_dac_mode ||
                                         volume_sync_enabled != bt_apply_inflight_volume_sync;
        return;
    }
    bt_apply_output_settings_rerun = false;
    bt_apply_output_settings_request_t * req = malloc(sizeof(*req));
    if (!req) {
        bt_apply_output_settings_rerun = true;
        return;
    }
    gui_shell_cancel_bt_reconnect();
    req->close_source = dac_mode_enabled && gui_shell_is_bt_audio_connected();
    if (req->close_source) gui_player_stop_for_external_session();
    req->dac_mode_enabled = dac_mode_enabled;
    req->volume_sync_enabled = volume_sync_enabled;
    bt_apply_inflight_dac_mode = dac_mode_enabled;
    bt_apply_inflight_volume_sync = volume_sync_enabled;
    if (!shell_job_start(&bt_apply_output_settings_job, bt_apply_output_settings_thread_func, req)) {
        free(req);
        bt_apply_output_settings_rerun = true;
    }
}
void poll_bt_apply_output_settings(void) {
    bool reaped = shell_job_reap(&bt_apply_output_settings_job);
    if (reaped) {
        bt_profile_failed = !bt_apply_succeeded;
        if (bt_apply_succeeded) bt_profile_applied = bt_apply_inflight_dac_mode;
    }
    if (bt_apply_output_settings_rerun && !bt_apply_output_settings_job.active)
        start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    if (bt_dac_power_off_pending && !gui_shell_bt_dac_busy()) {
        bt_dac_power_off_pending = false;
        gui_shell_bt_request_power(false);
    }
    if (reaped) populate_bt_dac_screen(); /* the DAC screen's own toggle rows need the post-apply state */
}
#ifndef HOST_BUILD
void bt_resume(bool bt_was_on) {
    if (bt_was_on && bt_toggle_active) {
        bt_reconnect_begin_new_cycle();
        bt_toggle_followup_target_enabled = true;
        bt_toggle_followup_pending = !bt_toggle_target_enabled;
    } else if (bt_was_on && (!bt_is_powered_cached || !bt_toggle_target_enabled)) {
        /* Resume is a new power-on cycle even if no status poll ran while
         * suspended to observe the intermediate OFF state. */
        bt_reconnect_begin_new_cycle();
        bt_toggle_active = true;
        bt_toggle_target_enabled = true;
        bt_toggle_followup_pending = false;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, BT_TOGGLE_TARGET_ON) != 0) {
            bt_toggle_active = false;
        }
    }
}
void bt_suspend_launch(bool bt_was_on) {
    if (bt_was_on && !bt_toggle_active) {
        bt_toggle_active = true;
        bt_toggle_target_enabled = false;
        bt_toggle_followup_pending = false;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, BT_TOGGLE_TARGET_OFF) != 0)
            bt_toggle_active = false;
    }
}
#endif

/* Per-radio pieces of the background-work queries and cancellation. The
 * public functions below call them in the same order as the original
 * single expressions/sequences. */
bool bt_workers_active(void) {
    return bt_toggle_active || bt_dac_startup_reapply_job.active || bt_apply_output_settings_job.active ||
           bt_source_codec_reconcile_job.active || refresh_bt_icon_job.active;
}
bool bt_monitor_or_reconnect_active(void) {
    return bt_monitor_lifecycle_job.active ||
           bt_reconnect_busy();
}
void bt_background_cancel_monitors(void) {
    bt_reconnect_shutdown();
    bt_reconnect_pending = false;
    shell_job_cancel(&bt_monitor_lifecycle_job);
    /* Explicit teardown may wait; ordinary UI interactions never do. */
    bt_control_source_volume_sync_stop();
    bt_control_output_disconnect_watch_stop();
    bt_monitor_want_output = bt_monitor_want_volume = false;
    bt_monitor_applied_output = bt_monitor_applied_volume = false;
    bt_monitor_refresh_requested = false;
}
void bt_background_cancel_toggle(void) {
    if (bt_toggle_active) {
        pthread_join(bt_toggle_thread, NULL);
        bt_toggle_active = false;
    }
}
void bt_background_cancel_workers(void) {
    shell_job_cancel(&bt_dac_startup_reapply_job);
    shell_job_cancel(&bt_apply_output_settings_job);
    bt_apply_output_settings_rerun = false;
    shell_job_cancel(&bt_source_codec_reconcile_job);
    shell_job_cancel(&refresh_bt_icon_job);
}

bool gui_shell_bt_dac_busy(void) {
    return gui_shell_bt_dac_owns_output() || bt_apply_output_settings_job.active ||
           bt_apply_output_settings_rerun || bt_profile_applied || bt_profile_failed;
}
bool gui_shell_bt_dac_profile_ready(void) {
    return current_settings.bt_dac_mode_enabled && bt_is_powered_cached && bt_profile_applied &&
           !bt_profile_failed && !bt_apply_output_settings_job.active && !bt_apply_output_settings_rerun;
}
void gui_shell_bt_dac_finish_profiles(void) {
    shell_job_cancel(&bt_apply_output_settings_job);
    bt_apply_output_settings_rerun = false;
    bt_profile_failed = !bt_control_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
    if (!bt_profile_failed) bt_profile_applied = false;
}

bool gui_shell_bt_dac_profile_failed(void) { return bt_profile_failed; }
