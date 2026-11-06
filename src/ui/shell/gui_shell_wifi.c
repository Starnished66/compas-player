#include "gui_shell_internal.h"

static bool wifi_toggle_active = false;
/* While a manual toggle or the ordinary screen-off radio restore is in
 * flight, the requested state is the UI source of truth. wifi_on.sh
 * tears down and recreates wpa_supplicant, so its control socket
 * temporarily disappears during a cold enable; this prevents the UI
 * from bouncing on -> off -> on during the transition. */
static bool wifi_toggle_target_enabled = false;

/* True only while the in-flight wifi_toggle_thread was kicked off by
 * gui_shell_suspend_connections() (the automatic idle-screen-off radio
 * power-save cycle) rather than the user's own quick_drawer_wifi_event_cb()
 * tap. Each of the three call sites that can start this thread (that one,
 * gui_shell_suspend_connections(), and gui_shell_resume_connections()) sets
 * this immediately before its own pthread_create(), so it always reflects
 * whichever attempt is actually in flight -- a failed launch never leaves a
 * stale value behind for a later, unrelated toggle to misread, since
 * wifi_toggle_active reverting to false means poll_wifi_toggle() never
 * consumes it for that failed attempt anyway.
 *
 * Consumed once by poll_wifi_toggle() to skip permanent cleanup of
 * DLNA and Remote Control during transient power-save radio suspend,
 * preserving their settings across screen-off sleep cycles. */
static bool wifi_toggle_is_radio_suspend = false;

/* A tap that arrives while a toggle is still in flight. wifi_on.sh/wifi_off.sh
 * take a couple of seconds, which is long enough to tap again, and dropping
 * that tap left the radio in the opposite state to the one the icon was
 * showing. Only the most recent request is kept: tapping twice settles on the
 * second target rather than replaying both, so the radio is never cycled just
 * to satisfy a request the user already changed their mind about. */
static bool wifi_toggle_queued = false;
static bool wifi_toggle_queued_target = false;

/* Read-only effective-Wi-Fi-state accessor for callers outside this file
 * (gui_network.c's Wi-Fi dependency guard for AirPlay/DLNA/Remote Control
 * and remote file import) that need the same "in-flight toggle counts as its
 * target state" logic refresh_wifi_icon() below already uses -- plain
 * wifi_control_is_enabled() can briefly still report the OLD hardware state
 * while wifi_toggle_active is true (see wifi_toggle_thread_func()'s own
 * comment), which would let a tap through for a moment right as Wi-Fi is
 * being turned off. Deliberately exposes only this bool, not wifi_toggle_
 * active/wifi_toggle_target_enabled themselves -- callers have no business
 * reading or driving this file's own toggle machinery directly. */
bool gui_shell_wifi_effective_enabled(void) {
    /* A queued request outranks the one in flight: it is the newer intent, it
     * is already what the icons show, and it is what the radio will be left
     * at. Without it here, any redraw between the tap and the relaunch (a
     * status refresh, a wake) would repaint the older target over it. */
    if (wifi_toggle_queued) return wifi_toggle_queued_target;
    return wifi_toggle_active ? wifi_toggle_target_enabled : wifi_control_is_enabled();
}

/* The drawer's wifi icon reflects radio on/off (highlighted when enabled),
 * while the top bar icon indicates connection status and signal strength. */
static shell_job_t wifi_status_job;
static bool wifi_status_result_connected;
static int wifi_status_result_level;
static bool wifi_status_connected;
static int wifi_status_level;
static unsigned wifi_status_generation;
static unsigned wifi_status_job_generation;
static bool wifi_status_enabled;
/* Set by the UI thread before the worker starts: query association only. */
static bool wifi_status_job_skip_signal;
static void * wifi_status_thread_func(void * arg) {
    (void)arg;
    int level = 0;
    if (wifi_status_job_skip_signal) {
        wifi_status_result_connected = wifi_get_connected();
        level = wifi_status_level; /* not measured: keep the last value */
    } else {
        wifi_status_result_connected = wifi_get_status(&level);
    }
    wifi_status_result_level = level;
    atomic_store_explicit(&wifi_status_job.done, true, memory_order_release);
    return NULL;
}

/* UI-thread only: stale replies from a previous radio state are discarded. */
static void wifi_status_observe_enabled(bool enabled) {
    if (enabled == wifi_status_enabled) return;
    wifi_status_enabled = enabled;
    ++wifi_status_generation;
    wifi_status_connected = false;
    wifi_status_level = 0;
}
static void render_wifi_icon(void) {
    /* Keep an in-flight enable visually enabled even before wlan0's
     * wpa_supplicant socket exists.  Association is still queried below,
     * so the topbar naturally advances from the existing disconnected
     * icon to signal strength without exposing an "enabling" state.  Once
     * poll_wifi_toggle() clears wifi_toggle_active, this immediately goes
     * back to the authoritative backend state and can still report a real
     * failure normally. */
    bool enabled = gui_shell_wifi_effective_enabled();
    if (quick_drawer_wifi_icon) {
        if (quick_drawer_set_toggle_image(quick_drawer_wifi_icon,
                                          quick_drawer_toggle_src(QD_TOGGLE_WIFI, enabled)))
            quick_drawer_mark_snapshot_dirty();
    }
    quick_drawer_set_toggle_state(0, enabled);

    if (!enabled) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
        return;
    }
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    sync_topbar_status_icon_positions();

    int level = wifi_status_level;
    if (wifi_status_connected) {
        static const char * const assets[] = {
            "wifi_zero", "wifi_low",
            "wifi_high", "wifi",
        };
        if (level < 0) level = 0;
        if (level > 3) level = 3;
        if (topbar_set_image(wifi_icon, topbar_icon(assets[level])))
            layout_wifi_connected_topbar_image(wifi_icon);
    } else if (topbar_set_image(wifi_icon, topbar_icon("wifi_off"))) {
        layout_radio_topbar_image(wifi_icon);
    }
}

/* UI thread only (poll_wifi_status() is never run from the worker): hand the
 * cached state to the plugin manager, which dispatches "network_changed" only
 * when it differs from what it last saw. */
static void publish_network_state(void) {
    plugin_manager_set_network_state(wifi_status_connected, wifi_status_enabled);
}
void poll_wifi_status(void) {
    wifi_status_observe_enabled(gui_shell_wifi_effective_enabled());
    if (!shell_job_reap(&wifi_status_job)) { /* the worker has already finished its query */
        publish_network_state();
        return;
    }
    if (wifi_status_enabled && wifi_status_job_generation == wifi_status_generation) {
        wifi_status_connected = wifi_status_result_connected;
        wifi_status_level = wifi_status_result_level;
        render_wifi_icon();
    }
    publish_network_state();
}
static void start_wifi_status_query(bool skip_signal) {
    if (!wifi_status_enabled || wifi_status_job.active || wifi_toggle_active) return;
    wifi_status_job_skip_signal = skip_signal;
    wifi_status_job_generation = wifi_status_generation;
    shell_job_start(&wifi_status_job, wifi_status_thread_func, NULL);
}
void refresh_wifi_icon(void) {
    poll_wifi_status();
    render_wifi_icon();
    /* Preserve the existing ~5s refresh cadence, with at most one query. */
    start_wifi_status_query(false);
}

/* The top bar (and so refresh_wifi_icon()) only runs with the screen on, which
 * would freeze the cached "connected" state that plugins see. While the screen
 * is off and a plugin listens for network_changed, run the same asynchronous
 * query about every 45 s; poll_wifi_status() joins and publishes the result.
 * Never started when Wi-Fi is disabled. UI thread only, never blocks. */
#define WIFI_OFF_SCREEN_QUERY_MS 45000
void poll_wifi_status_screen_off(void) {
    static uint32_t last_query_tick;
    static bool have_last;
    if (backlight_screen_is_on() || !plugin_manager_has_network_subscribers()) {
        have_last = false;
        return;
    }
    if (!wifi_status_enabled || wifi_status_job.active || wifi_toggle_active) return;
    if (have_last && lv_tick_elaps(last_query_tick) < WIFI_OFF_SCREEN_QUERY_MS) return;
    last_query_tick = lv_tick_get();
    have_last = true;
    start_wifi_status_query(true);
}
/* Defined much later, alongside the rest of the new Wi-Fi/Bluetooth
 * screens -- long-pressing the drawer's wifi/bt icons opens the real
 * settings screen for that radio, matching Android's quick-settings
 * convention (tap toggles, long-press opens the full screen). */

/* Long-press handlers for the drawer's wifi/bt icons -- hides the drawer
 * instantly (no slide-out animation; the settings screen navigation is
 * about to slide in over it anyway) then opens the real settings screen.
 *
 * LVGL still sends LV_EVENT_CLICKED on release even after LV_EVENT_LONG_PRESSED
 * fired. The long_press_fired flags ensure click handlers do not inadvertently
 * toggle radios when a long-press has already opened settings. */
static bool quick_drawer_wifi_long_press_fired = false;
/* A new press starts a fresh gesture: a long press whose release never
 * produced CLICKED must not swallow the next tap. */
void quick_drawer_wifi_pressed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    quick_drawer_wifi_long_press_fired = false;
}
void quick_drawer_wifi_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_wifi_long_press_fired = true;
    quick_drawer_dismiss_for_navigation();
    open_wifi_screen();
}

/* Tap-to-toggle handler for Wi-Fi. Runs asynchronously in a background thread
 * to avoid blocking the UI while enabling or disabling the radio. */
static pthread_t wifi_toggle_thread;
static atomic_bool wifi_toggle_done_flag = false;
static void * wifi_toggle_thread_func(void * arg) {
    (void) arg;
    bool turning_on = wifi_toggle_target_enabled;
    if (turning_on) wifi_control_enable();
    else wifi_control_disable();

    /* Wait for control socket to settle to avoid reading stale state immediately
     * after wifi_on.sh/wifi_off.sh execution. */
    for (int i = 0; i < 10 && wifi_control_is_enabled() != turning_on; i++) {
        usleep(300000);
    }

    atomic_store_explicit(&wifi_toggle_done_flag, true, memory_order_release); /* written last -- poll_wifi_toggle only checks this flag */
    return NULL;
}

/* Everything that makes a tap read as instant, applied before the radio has
 * actually changed. wifi_control_is_enabled() is a plain access() check (see
 * its own comment), not a subprocess spawn, so the surrounding state reads
 * here are cheap enough to do synchronously. The real toggle takes a couple
 * of seconds; poll_wifi_toggle() re-reads the hardware once the worker lands
 * and corrects all of this if the toggle failed. */
static void wifi_toggle_apply_optimistic_ui(bool will_be_enabled) {
    quick_drawer_apply_toggle_image(QD_TOGGLE_WIFI, quick_drawer_wifi_icon, will_be_enabled);
    quick_drawer_mark_snapshot_dirty();

    if (will_be_enabled) {
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        if (topbar_set_image(wifi_icon, topbar_icon("wifi_off")))
            layout_radio_topbar_image(wifi_icon);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

    /* Do not clean/rebuild wifi_list from inside the clicked row's own event
     * callback: that deletes the event target while LVGL is still dispatching
     * through it. gui_network_show_wifi_toggle_pending() defers the rebuild by
     * one UI turn; poll_wifi_toggle() performs the authoritative one. */
    if (gui_navigation_is_top(gui_network_get_wifi_screen()))
        gui_network_show_wifi_toggle_pending(will_be_enabled);
}
static void wifi_remember_user_target(bool enabled) {
    if (current_settings.wifi_last_on == enabled) return;
    current_settings.wifi_last_on = enabled;
    settings_save_async(&current_settings);
}

void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) {
        /* Queue rather than drop: the in-flight attempt owns the radio until
         * it settles, but the user's newest intent still has to survive.
         * poll_wifi_toggle() launches this once the current attempt lands,
         * and only if it actually disagrees with the settled state. */
        wifi_toggle_queued_target = !gui_shell_wifi_effective_enabled();
        wifi_remember_user_target(wifi_toggle_queued_target);
        wifi_toggle_queued = true;
        wifi_toggle_apply_optimistic_ui(wifi_toggle_queued_target);
        return;
    }
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    wifi_remember_user_target(wifi_will_be_enabled);
    wifi_toggle_active = true;
    wifi_toggle_is_radio_suspend = false; /* a real user tap, not the idle radio-suspend cycle */
    atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
    wifi_toggle_target_enabled = wifi_will_be_enabled;

    wifi_toggle_apply_optimistic_ui(wifi_will_be_enabled);

    if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
        wifi_toggle_active = false;
        refresh_wifi_icon();
        start_bt_dac_startup_reapply_if_needed();
        gui_network_wifi_toggle_completed(wifi_control_is_enabled());
    }
}
bool gui_shell_wifi_ensure_enabled(void) {
    wifi_remember_user_target(true);
    if (wifi_toggle_active) {
        if (wifi_toggle_queued && wifi_toggle_queued_target) return true;
        if (!wifi_toggle_queued && wifi_toggle_target_enabled) return true;
        wifi_toggle_queued_target = true;
        wifi_toggle_queued = true;
        wifi_toggle_apply_optimistic_ui(true);
        return true;
    }

    if (wifi_control_is_enabled()) return true;
    wifi_toggle_active = true;
    wifi_toggle_is_radio_suspend = false;
    atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
    wifi_toggle_target_enabled = true;
    wifi_toggle_apply_optimistic_ui(true);
    if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
        wifi_toggle_active = false;
        refresh_wifi_icon();
        start_bt_dac_startup_reapply_if_needed();
        gui_network_wifi_toggle_completed(wifi_control_is_enabled());
        return false;
    }
    return true;
}
bool gui_shell_wifi_enabled_and_settled(void) {
    return !wifi_toggle_active && !wifi_toggle_queued && wifi_control_is_enabled();
}
bool gui_shell_wifi_connected(void) {
    return gui_shell_wifi_enabled_and_settled() && wifi_status_enabled && wifi_status_connected;
}
void poll_wifi_toggle(void) {
    if (!wifi_toggle_active || !atomic_load_explicit(&wifi_toggle_done_flag, memory_order_acquire)) return;
    wifi_toggle_active = false;
    pthread_join(wifi_toggle_thread, NULL);
    bool enabled = wifi_control_is_enabled();
    refresh_wifi_icon(); /* re-reads the real state -- updates both the status bar and drawer icons */
    gui_network_wifi_toggle_completed(enabled); /* authoritative rows + scan state */
    if (enabled != wifi_toggle_target_enabled) {
        show_error_toast(TR("Wi-Fi failed to change state"));
    }

    /* Only shut AirPlay/DLNA/Remote Control/Import down once the disable is
     * AUTHORITATIVELY confirmed via the real wifi_control_is_enabled() read
     * above, not merely because wifi_toggle_target_enabled asked for OFF --
     * a failed disable (enabled != wifi_toggle_target_enabled, toast just
     * above) must leave those features running exactly as they were.
     *
     * Also excludes the automatic idle radio-suspend disable (wifi_toggle_
     * is_radio_suspend, set by gui_shell_suspend_connections()) -- that one
     * shares this exact same toggle-thread/poll mechanism as the user's own
     * tap, but is a transient, self-reversing power-save blip (gui_shell_
     * resume_connections() brings the radio back the moment the screen
     * wakes), not a deliberate "turn Wi-Fi off" the user asked for. Running
     * the permanent cleanup for it would stop DLNA/Remote Control (AirPlay/
     * BT DAC are already excluded from ever reaching radio suspend at all,
     * via gui.c's own radios_suspended gate) and permanently clear their
     * persisted settings every time the screen idles, with no way for the
     * plain radio-restore afterward to ever turn them back on. See wifi_
     * toggle_is_radio_suspend's own comment above for the full reasoning. */
    bool was_radio_suspend = wifi_toggle_is_radio_suspend;
    wifi_toggle_is_radio_suspend = false;

    /* A queued request that disagrees with where the radio actually landed is
     * relaunched here, once the previous attempt has fully released it. */
    bool relaunch = wifi_toggle_queued && wifi_toggle_queued_target != enabled;
    wifi_toggle_queued = false;

    /* Runs for a queued re-enable too, so a disable behaves the same whether
     * the user reverses it quickly or slowly. The features this tears down
     * are gated on Wi-Fi being connected rather than merely enabled, so an
     * off state that is about to be undone has nothing live to take away. */
    if (!enabled && !was_radio_suspend) gui_network_handle_wifi_disabled();

    if (relaunch) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = false;
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = wifi_toggle_queued_target;
        wifi_toggle_apply_optimistic_ui(wifi_toggle_queued_target);
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
            wifi_toggle_active = false;
            refresh_wifi_icon();
            gui_network_wifi_toggle_completed(wifi_control_is_enabled());
        }
    }
}
#ifndef HOST_BUILD
void wifi_resume(bool wifi_was_on) {
    if (wifi_was_on && wifi_toggle_active) {
        wifi_toggle_queued_target = true;
        wifi_toggle_queued = true;
    } else if (wifi_was_on && !wifi_control_is_enabled()) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = false; /* restoring, not the suspend disable itself */
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = true;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
}
void wifi_suspend_launch(bool wifi_was_on) {
    if (wifi_was_on && !wifi_toggle_active) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = true; /* transient power-save disable, not a user-requested one -- see its own comment */
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = false;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
}
#endif
bool wifi_background_active(void) {
    return wifi_toggle_active || wifi_status_job.active;
}
void wifi_background_cancel_status(void) {
    shell_job_cancel(&wifi_status_job);
    ++wifi_status_generation;
    wifi_status_connected = false;
}
void wifi_background_cancel_toggle(void) {
    if (wifi_toggle_active) {
        pthread_join(wifi_toggle_thread, NULL);
        wifi_toggle_active = false;
    }
}
