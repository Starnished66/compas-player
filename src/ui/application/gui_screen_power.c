#include "gui.h"
#include "../text/i18n/i18n.h"
#include "gui_screen_off_memory.h"
#include "../../core/ui/display/ui_wake.h"
#include "../input/event_input.h"
#include "../network/gui_network.h"
#include "../player/screen/gui_lyrics.h"
#include "../player/transport/player_seek_input.h"
#include "../network/subsonic/gui_subsonic.h"
#include "../../hardware/indicators/backlight.h"
#include "../../hardware/power/battery.h"
#include "../../network/firmware/firmware_ota.h"
#include "../../hardware/input/hw_buttons.h"
#include "../../core/system/logging/db_log.h"
#include "../../network/store/catalog/emulator_store.h"
#include "../../hardware/indicators/led_control.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include "../../hardware/power/power_suspend.h"
#include "../../library/playlists/playlist_files.h"
#include "../../core/system/lifecycle/screenshot.h"
#include <string.h>
#include "gui_internal.h"

/* Full radio suspend after a long stretch with the screen off and nothing
 * going on -- deliberately separate from (and much longer than) the
 * screen-timeout backlight-off state itself: that can fire as often as
 * every 30s from ordinary momentary inactivity during otherwise-continuous
 * use, and WiFi/Bluetooth reassociation has a real cost, so tying radio
 * suspend to the same short timer would punish a user who's still actively
 * using the device, just with the screen dimming between taps. Skipped
 * entirely while music is playing, either DAC receive mode is on (both need
 * their radio to stay up to receive), or external power is connected or
 * unknown. A USB power source can remain connected while the battery reports
 * discharging, so charger status alone is not enough to identify that case.
 * Whichever radios were actually on get suspended, and only those get
 * resumed on wake -- a radio the user already had off before sleeping stays
 * off. */
#define RADIO_SUSPEND_DELAY_MS (10 * 60 * 1000)
static uint32_t screen_off_since_tick = 0;
static bool inactivity_dimmed = false;
static bool lyrics_timeout_held = false;

/* Earliest possible inactivity age for the interactive UI. LVGL's display
 * inactivity timestamp can predate gui_init()'s splash -> Home transition
 * (and, on the target, lv_display_trigger_activity() alone has now been
 * observed not to reliably discard that startup age). Clamp the value used
 * by dimming/timeout to elapsed interactive time so startup can never spend
 * the user's timeout budget. After this age grows past LVGL's normally-reset
 * inactivity value the clamp becomes a no-op, preserving ordinary touch and
 * hardware-button timeout behavior. */
static uint32_t interactive_ui_start_tick = 0;
static bool interactive_ui_started = false;

void gui_reset_interactive_timeout_baseline(void) {
    interactive_ui_start_tick = lv_tick_get();
    interactive_ui_started = true;
    lv_display_trigger_activity(NULL);
}

static bool radios_suspended = false;
static bool wifi_was_on_before_suspend = false;
static bool bt_was_on_before_suspend = false;

/* Tracks whether audio was playing as of the last tick the screen was off,
 * so the radio-suspend/idle-shutdown clocks (both anchored on
 * screen_off_since_tick) can be restarted when playback actually stops --
 * see the reset logic where this is used, just below. */
static bool screen_off_playback_active = false;

/* Idle shutdown: a full poweroff (see idle_shutdown.h) after
 * current_settings.idle_shutdown_minutes with the screen off and nothing
 * going on -- same gating as radio-suspend above (not playing, no external
 * power, not charging, no DAC receive mode active) since none of those should
 * ever be interrupted by the device turning itself off. */
/* Idle shutdown logic:
 *
 * Automatically power off if the device is inactive for a user-defined period
 * with the screen off. Shutdown is blocked if music is playing, external
 * power is connected or unknown, the battery is charging, or external audio
 * (USB/WiFi/BT DAC) is active.
 *
 * In addition to WiFi and Bluetooth DAC modes, USB DAC mode (USB_MODE_DAC)
 * is checked to prevent idle suspend or radio disconnect while receiving USB audio. */
static bool idle_shutdown_attempted = false;

/* Receiver modes that need their radio or gadget up to keep receiving. */
static bool dac_receive_mode_active(void) {
    return current_settings.wifi_dac_mode_enabled || current_settings.bt_dac_mode_enabled ||
           gui_shell_bt_dac_busy() || gui_network_airplay_enable_pending() ||
           gui_network_usb_dac_pending() || current_settings.usb_mode == (int) USB_MODE_DAC;
}

/* Idle radio suspend and idle shutdown never interrupt a DAC mode, playback,
 * external power or charging. */
static bool idle_power_action_blocked(bool external_power_blocks_idle) {
    return dac_receive_mode_active() || audio_is_playing() || external_power_blocks_idle ||
           battery_is_charging();
}

bool shutdown_background_work_active(void) {
    return gui_lock_screen_has_background_work(true) || gui_library_has_background_work() || gui_subsonic_has_background_work() ||
           gui_network_has_background_work() || gui_lyrics_has_background_work() ||
           gui_player_has_background_work() || gui_shell_has_background_work() ||
           plugin_manager_has_background_work() || playlist_files_has_active_write() ||
           screenshot_is_busy() || firmware_ota_busy() || plugin_store_busy() || emulator_store_busy();
}

/* Grace window after resuming from suspend. hw_buttons queues a tap event
 * on button release (handle_key_event, value==0). The physical button press that
 * wakes the kernel from suspend is read asynchronously by the evdev thread,
 * and its release event can be consumed after power_suspend_now() returns.
 * Any power-button press consumed within this window is treated as an echo of
 * the wake press and discarded to avoid toggling the screen back off. */
#define RESUME_POWER_DRAIN_WINDOW_MS 3000
static uint32_t resumed_from_suspend_tick = 0;
static bool resumed_from_suspend_pending = false;
/* power_suspend_now() returns from inside update_timer_cb(), after that
 * callback already sampled screen_was_on/screen_on_now. Remember that
 * out-of-band wake so the next control tick still runs every ordinary
 * screen_just_woke refresh/status/radio-reset action. */
static bool force_screen_just_woke = false;
/* A button or wake cancel turns the screen off between control ticks, so the
 * tick never samples it on. Remember it so the idle countdown restarts. */
static bool force_screen_just_slept = false;
/* A wake is prepared and painted with the backlight still off. */
static bool screen_wake_pending = false;
static bool screen_wake_unlock_requested = false;
static bool screen_wake_scanout_pending = false;
static bool screen_wake_notified = false;
static lv_timer_t *power_input_timer, *media_input_timer;
static bool screen_wake_frame_pending = false;
static bool screen_wake_flush_seen;
static bool screen_wake_reveal_pending;
static bool screen_wake_retained_fast;
static bool screen_wake_ram_staged;
static lv_obj_t *last_presented_screen;
static lv_obj_t *screen_wake_retained_screen;
static uint32_t screen_wake_phase_tick;
static uint32_t screen_wake_retry_tick;
#define SCREEN_WAKE_RETRY_MS 100
#define SCREEN_WAKE_TIMEOUT_MS 1000
static bool screen_wake_power_menu = false;
static void request_screen_power(bool on);

/* Restarts both clocks the wake phases are judged against. */
static void screen_wake_restart_phase(void) {
    screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
}

/* Repaints the active screen and makes LVGL's refresh timer due now. */
static void request_wake_frame_redraw(void) {
    full_redraw_async_cb(NULL);
    lv_timer_t * refresh = lv_display_get_refr_timer(lv_display_get_default());
    if (refresh) lv_timer_ready(refresh);
}

/* Drops every pending-wake flag and the retained-frame hold; callers keep
 * their own extras (backlight, runtime state, last_presented_screen). */
static void screen_wake_clear_state(void) {
    screen_wake_ram_staged = false;
    screen_wake_pending = false;
    screen_wake_unlock_requested = false;
    screen_wake_scanout_pending = false;
    screen_wake_notified = false;
    screen_wake_frame_pending = false;
    screen_wake_flush_seen = false;
    screen_wake_reveal_pending = false;
    screen_wake_power_menu = false;
    if (screen_wake_retained_fast)
        lv_display_enable_invalidation(lv_display_get_default(), true);
    screen_wake_retained_fast = false;
    screen_wake_retained_screen = NULL;
    gui_lock_screen_set_wake_unlock_override(false);
}

bool gui_display_refresh_allowed(void) {
    /* A prepared wake frame is drawn before the backlight is enabled. */
    return (backlight_screen_is_on() && !screen_wake_retained_fast) || screen_wake_frame_pending || screen_wake_ram_staged;
}

/* Keeps the three pieces of screen runtime state inseparable. Merely
 * enabling an indev does not restart its paused LVGL read timer, and merely
 * lighting the panel does not restart LVGL's paused refresh timer. Used by
 * both the ordinary on/off edge below and the special suspend-return path,
 * which changes backlight state too late in the current timer callback for
 * that ordinary edge detector to observe it. */
static void apply_screen_runtime_state(bool screen_on) {
    lv_indev_enable(NULL, screen_on);
    audio_set_low_power_mode(!screen_on);

    gui_shell_reset_drag_state();
    gui_library_reset_drag_state();
    gui_lock_screen_reset_drag_state();

    lv_timer_t * refr_timer = lv_display_get_refr_timer(lv_display_get_default());
    if (refr_timer) {
        if (screen_on) lv_timer_resume(refr_timer);
        else lv_timer_pause(refr_timer);
    }

#ifdef UI_PERF_TRACE
    int indev_timer_count = 0;
#endif
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
        if (!read_timer) continue;
        ui_input_set_runtime_timer(indev, screen_on);
#ifdef UI_PERF_TRACE
        indev_timer_count++;
#endif
    }

    if (screen_on) lv_async_call(full_redraw_async_cb, NULL);
#ifdef UI_PERF_TRACE
    printf("PERF screen_runtime screen_on=%d refr_timer=%d indev_timers=%d\n",
           screen_on, refr_timer != NULL, indev_timer_count);
#endif
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] apply_screen_runtime_state: screen_on=%d\n", screen_on);
#endif
}

/* Restores backlight, input devices, and inactivity timer state immediately
 * after waking from suspend. Shared between idle-shutdown and Car Mode suspend paths. */
static void resume_from_suspend_fixups(void) {
    /* Full suspend may reset framebuffer/PWM outside the player. */
    last_presented_screen = NULL;
    request_screen_power(true);
    lv_display_trigger_activity(NULL);
    resumed_from_suspend_tick = lv_tick_get();
    resumed_from_suspend_pending = true;
    DBG_LOG("resume: grace window armed at tick=%u\n", resumed_from_suspend_tick);
}

void gui_keep_screen_awake_for_prompt(void) {
    request_screen_power(true);
    lv_display_trigger_activity(NULL);
}

static button_mapping_context_t physical_button_mapping;

/* Setting a button's own binding again only drops its pending taps. */
static void physical_buttons_cancel_pending_taps(void) {
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
        button_mapping_set(&physical_button_mapping, (button_mapping_button_t)i,
                           &physical_button_mapping.bindings[i]);
}
static bool physical_volume_hold_cancelled[BUTTON_MAPPING_BUTTON_COUNT];
static button_mapping_action_t physical_button_default_action(
        button_mapping_button_t button, button_mapping_gesture_t gesture);

bool gui_button_bindings_allow_wake(const button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT]) {
    if (!bindings) return false;
    for (unsigned button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
#if !BOARD_HAS_PREVIOUS_BUTTON
        if (button == BUTTON_MAPPING_PREVIOUS) continue;
#endif
        const button_mapping_action_t actions[] = {
            bindings[button].tap, bindings[button].double_tap, bindings[button].hold
        };
        for (unsigned gesture = 0; gesture < 3; ++gesture) {
#if defined(BOARD_R3II_2025)
            if (gesture && (button == BUTTON_MAPPING_VOLUME_UP || button == BUTTON_MAPPING_VOLUME_DOWN))
                continue; /* Wheel detents cannot produce double/hold assignments. */
#endif
            button_mapping_action_t action = actions[gesture];
            if (action == BUTTON_MAPPING_ACTION_DEFAULT) {
                if (gesture == 1) continue; /* No double gesture configured. */
                action = physical_button_default_action((button_mapping_button_t)button,
                    gesture == 0 ? BUTTON_MAPPING_GESTURE_TAP : BUTTON_MAPPING_GESTURE_HOLD_FIRST);
            }
            if (action == BUTTON_MAPPING_ACTION_SCREEN_TOGGLE || action == BUTTON_MAPPING_ACTION_POWER_MENU)
                return true;
        }
    }
    return false;
}

void gui_apply_button_bindings(void) {
    /* Saved files can also be edited outside the UI; recover a wake action
     * there as well as preventing the unsafe assignment in the selector. */
    if (!gui_button_bindings_allow_wake(current_settings.button_bindings)) {
        current_settings.button_bindings[BUTTON_MAPPING_POWER].tap = BUTTON_MAPPING_ACTION_DEFAULT;
        settings_save_async(&current_settings);
    }
    button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT];
    memcpy(bindings, current_settings.button_bindings, sizeof(bindings));
#if defined(BOARD_R3II_2025)
    bindings[BUTTON_MAPPING_VOLUME_UP].double_tap = BUTTON_MAPPING_ACTION_DEFAULT;
    bindings[BUTTON_MAPPING_VOLUME_UP].hold = BUTTON_MAPPING_ACTION_DEFAULT;
    bindings[BUTTON_MAPPING_VOLUME_DOWN].double_tap = BUTTON_MAPPING_ACTION_DEFAULT;
    bindings[BUTTON_MAPPING_VOLUME_DOWN].hold = BUTTON_MAPPING_ACTION_DEFAULT;
#endif
    hw_buttons_set_bindings(bindings);
    memset(physical_volume_hold_cancelled, 0, sizeof(physical_volume_hold_cancelled));
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        button_mapping_set(&physical_button_mapping, (button_mapping_button_t)i, &bindings[i]);
        bool legacy_double = i == BUTTON_MAPPING_PLAY_PAUSE && current_settings.play_pause_button_mode == 2 &&
            bindings[i].tap == BUTTON_MAPPING_ACTION_PLAY_PAUSE &&
            bindings[i].double_tap == BUTTON_MAPPING_ACTION_PREVIOUS;
        button_mapping_set_double_window_ms(&physical_button_mapping, (button_mapping_button_t)i,
                                           legacy_double ? 700 : 300);
    }
}

static void finish_screen_wake_reveal(void) {
    if (screen_wake_retained_fast) {
        lv_display_enable_invalidation(lv_display_get_default(), true);
        apply_screen_runtime_state(true);
    }
    screen_wake_pending = screen_wake_reveal_pending = false;
    screen_wake_retained_fast = false;
    screen_wake_power_menu = false;
    screen_wake_unlock_requested = false;
    screen_wake_scanout_pending = false;
    lv_indev_enable(NULL, true);
    gui_lock_screen_refresh(false);
    gui_control_poll_kick_player_ui();
    force_screen_just_woke = true;
    gui_library_screen_woke(); /* the page's chunk timer resumes now, not at the next 500 ms poll */
}

static void begin_screen_wake_reveal(void) {
    screen_wake_frame_pending = false;
    screen_wake_reveal_pending = true;
    screen_wake_restart_phase();
    backlight_set_screen_on(true);
}

/* Lua handlers select their wake surface before choosing the wake path.
 * Reveal it only after a successful last flush, without nested lv_refr_now(). */
static void screen_wake_frame_ready_cb(lv_event_t * event) {
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FLUSH_FINISH) {
        lv_display_t * display = lv_event_get_target(event);
        if (lv_display_flush_is_last(display)) {
#ifdef HOST_BUILD
            bool presented = true;
#else
            bool presented = lv_linux_fbdev_last_present_succeeded(display);
#endif
            last_presented_screen = presented && !gui_navigation_transition_in_progress() ?
                lv_screen_active() : NULL;
            if (screen_wake_frame_pending) screen_wake_flush_seen = presented;
        }
        return;
    }
    if (!screen_wake_frame_pending) return;
    if (screen_wake_ram_staged) return; /* RAM flush is not physical presentation. */
    if (code != LV_EVENT_REFR_READY || !screen_wake_flush_seen) return;
    /* Wait for the short in-memory album blur. File-backed photos can take
     * longer; show the prepared clock/gesture surface while they load. */
    if (gui_lock_screen_unlock_pending()) return;
    if (gui_lock_screen_is_showing() && gui_lock_screen_has_background_work(false)) return;
    begin_screen_wake_reveal();
}

static void prepare_screen_wake_frame(void) {
    if (!screen_wake_pending || gui_lock_screen_unlock_pending()) return;
    apply_screen_runtime_state(true);
    lv_indev_enable(NULL, false);
    if (screen_wake_power_menu) {
        screen_wake_power_menu = false;
        start_power_off_countdown();
    }
    screen_wake_frame_pending = true;
    screen_wake_flush_seen = false;
    screen_wake_restart_phase();
    request_wake_frame_redraw();
}

#ifndef HOST_BUILD
/* True while the pre-sleep frame on scanout can still be shown as is. */
static bool screen_wake_retained_frame_reusable(lv_display_t * display) {
    return screen_wake_retained_screen &&
        lv_screen_active() == screen_wake_retained_screen &&
        !gui_navigation_transition_in_progress() &&
        !gui_lock_screen_is_showing() && !gui_lock_screen_unlock_pending() &&
        !screen_wake_unlock_requested && !screen_wake_power_menu &&
        lv_linux_fbdev_last_frame_reusable(display);
}
#endif

#ifndef HOST_BUILD
/* Returns true when the retained pre-sleep frame was kept for the reveal. */
static bool screen_wake_try_retained_fast(lv_display_t *display, bool reusable_before) {
    if (!(reusable_before && screen_wake_retained_frame_reusable(display))) return false;
    /* Keep the exact pre-sleep normal frame on scanout while hardware
     * unblanks; ordinary refresh resumes after the visible acknowledgement. */
    screen_wake_retained_fast = true;
    /* Invalidating a widget resumes LVGL refresh even when its timer is
     * paused. Hold invalidation too; the reveal requests a full redraw. */
    lv_display_enable_invalidation(display, false);
    screen_wake_scanout_pending = false;
    screen_wake_reveal_pending = true;
    screen_wake_restart_phase();
    backlight_set_screen_on(true);
    return true;
}

static void screen_wake_begin_ram_stage(lv_display_t *display) {
    /* Render the complete LVGL composition into RAM during panel startup.
     * The driver leaves both physical pages black until the explicit commit. */
    lv_refr_scroll_blit_reset(); /* the stage replaces the invalid-area queue */
    if (lv_linux_fbdev_begin_ram_stage(display)) {
        screen_wake_ram_staged = true;
        prepare_screen_wake_frame();
    }
}
#endif

/* Called only while input/display refresh are stopped. A compositor still
 * owning scanout must finish before we can safely power up the panel. */
static bool prepare_screen_wake_scanout(void) {
#ifndef HOST_BUILD
    lv_display_t *display = lv_display_get_default();
    bool reusable_before = screen_wake_retained_frame_reusable(display);
#endif
    if (!screen_wake_notified) {
        screen_wake_notified = true;
        gui_lock_screen_set_wake_unlock_override(screen_wake_unlock_requested);
        plugin_manager_notify_screen_woke();
        gui_lock_screen_set_wake_unlock_override(false);
    }
#ifndef HOST_BUILD
    if (screen_wake_try_retained_fast(display, reusable_before)) return true;
    lv_refr_scroll_blit_reset(); /* the page contents are about to be replaced */
    if (!lv_linux_fbdev_clear_scanout(display)) return false;
#endif
    screen_wake_scanout_pending = false;
    screen_wake_restart_phase();
    backlight_prepare_screen_on();
#ifndef HOST_BUILD
    screen_wake_begin_ram_stage(display);
#endif
    return true;
}

static void request_screen_power(bool on) {
    if (on) {
        if (backlight_screen_is_on() || screen_wake_pending) return;
        physical_buttons_cancel_pending_taps();
        /* Discard stale scanout before panel power-up. PWM zero is also
         * forced by backlight_prepare_screen_on(), but OEM unblank must
         * never expose the previous screen even if it restores brightness. */
        screen_wake_retained_screen = NULL;
#ifndef HOST_BUILD
        lv_display_t *display = lv_display_get_default();
        if (!gui_navigation_transition_in_progress() && !gui_lock_screen_is_showing() &&
            !gui_lock_screen_unlock_pending() && lv_screen_active() == last_presented_screen &&
            lv_linux_fbdev_last_frame_reusable(display))
            screen_wake_retained_screen = lv_screen_active();
#endif
        apply_screen_runtime_state(false);
        screen_wake_notified = false;
        screen_wake_retained_fast = false;
        screen_wake_scanout_pending = true;
        screen_wake_pending = true;
        if (power_input_timer) lv_timer_resume(power_input_timer);
        screen_wake_restart_phase();
        if (!prepare_screen_wake_scanout())
            fprintf(stderr, "Screen wake: waiting to clear stale scanout\n");
    } else {
#ifndef HOST_BUILD
        if (screen_wake_ram_staged &&
            !lv_linux_fbdev_end_ram_stage(lv_display_get_default())) return;
#endif
        physical_buttons_cancel_pending_taps();
        screen_wake_clear_state();
        backlight_set_screen_on(false);
        apply_screen_runtime_state(false);
        force_screen_just_slept = true;
    }
}

void gui_sleep_timer_expired(void) {
    if (current_settings.bt_dac_mode_enabled || gui_shell_bt_dac_busy()) disable_bt_dac_mode();
    /* A deliberate sleep timer must still finish the session when pausing
     * alone no longer releases the visible-lyrics keep-awake hold. */
    if (current_settings.lyrics_keep_screen_on && gui_lyrics_is_visible())
        request_screen_power(false);
}

static uint32_t physical_button_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static button_mapping_action_t physical_button_default_action(
        button_mapping_button_t button, button_mapping_gesture_t gesture) {
    bool tap = gesture == BUTTON_MAPPING_GESTURE_TAP;
    if (button == BUTTON_MAPPING_POWER && gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT)
        return BUTTON_MAPPING_ACTION_NONE;
    switch (button) {
        case BUTTON_MAPPING_POWER:
            return tap ? BUTTON_MAPPING_ACTION_SCREEN_TOGGLE :
                         BUTTON_MAPPING_ACTION_POWER_MENU;
        case BUTTON_MAPPING_PLAY_PAUSE:
            return tap ? BUTTON_MAPPING_ACTION_PLAY_PAUSE : BUTTON_MAPPING_ACTION_NONE;
        case BUTTON_MAPPING_NEXT:
            return tap ? BUTTON_MAPPING_ACTION_NEXT : BUTTON_MAPPING_ACTION_SEEK_FORWARD;
        case BUTTON_MAPPING_PREVIOUS:
            return tap ? BUTTON_MAPPING_ACTION_PREVIOUS : BUTTON_MAPPING_ACTION_NONE;
        case BUTTON_MAPPING_VOLUME_UP: return BUTTON_MAPPING_ACTION_VOLUME_UP;
        case BUTTON_MAPPING_VOLUME_DOWN: return BUTTON_MAPPING_ACTION_VOLUME_DOWN;
        default: return BUTTON_MAPPING_ACTION_NONE;
    }
}

static button_mapping_action_t physical_button_resolve_action(
        button_mapping_action_t action, button_mapping_button_t button,
        button_mapping_gesture_t gesture) {
    if (hw_buttons_button_lock_is_locked()) {
        if (button != BUTTON_MAPPING_POWER) return BUTTON_MAPPING_ACTION_NONE;
        if (gesture == BUTTON_MAPPING_GESTURE_TAP || gesture == BUTTON_MAPPING_GESTURE_DOUBLE)
            return BUTTON_MAPPING_ACTION_SCREEN_TOGGLE;
        if (gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST)
            return BUTTON_MAPPING_ACTION_POWER_MENU;
        return BUTTON_MAPPING_ACTION_NONE;
    }
    if (action == BUTTON_MAPPING_ACTION_DEFAULT)
        return physical_button_default_action(button, gesture);
    return action;
}

/* Returns false when the gesture must be dropped before dispatch. */
static bool physical_button_gesture_allowed(button_mapping_action_t action,
        button_mapping_button_t button, button_mapping_gesture_t gesture) {
    /* The wake key can be replayed by evdev after suspend. Consume that
     * gesture without triggering its assignment a second time. */
    if (button == BUTTON_MAPPING_POWER && resumed_from_suspend_pending) {
        resumed_from_suspend_pending = false;
        return false;
    }
    bool volume_action = action == BUTTON_MAPPING_ACTION_VOLUME_UP ||
                         action == BUTTON_MAPPING_ACTION_VOLUME_DOWN;
    bool hold = gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST ||
                gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT;
    if (gesture != BUTTON_MAPPING_GESTURE_HOLD_REPEAT)
        physical_volume_hold_cancelled[button] = false;
    if (volume_action && current_settings.volume_lock_screen_off &&
        !backlight_screen_is_on()) {
        if (hold) physical_volume_hold_cancelled[button] = true;
        return false;
    }
    /* A mapped volume hold that crossed screen-off stays canceled until a
     * fresh gesture, matching the physical volume keys' wake behavior. */
    if (volume_action && hold && physical_volume_hold_cancelled[button]) return false;
    if (!current_settings.setup_complete && !volume_action &&
        action != BUTTON_MAPPING_ACTION_SCREEN_TOGGLE &&
        action != BUTTON_MAPPING_ACTION_POWER_MENU) return false;
    return true;
}

static void physical_button_screen_toggle(void) {
    bool turn_on = !(backlight_screen_is_on() || screen_wake_pending);
    request_screen_power(turn_on);
    if (turn_on) inactivity_dimmed = false;
}

static void physical_button_power_menu(void) {
    if (!backlight_screen_is_on()) {
        if (screen_wake_frame_pending || screen_wake_reveal_pending)
            start_power_off_countdown();
        else screen_wake_power_menu = true;
        request_screen_power(true);
        inactivity_dimmed = false;
    } else start_power_off_countdown();
}

static void physical_button_volume_step(button_mapping_action_t action, unsigned count) {
    int delta = (int)count * (action == BUTTON_MAPPING_ACTION_VOLUME_UP ? 1 : -1);
    /* Bluetooth accessories can change the effective audio volume
     * before the next UI reconciliation tick. Step from the audio
     * state so a physical button never starts from a stale slider. */
    int new_percent = audio_clamp_volume_percent(gui_capped_volume_percent() + delta);
    gui_player_set_volume_percent(new_percent);
    audio_set_volume((float)new_percent / 100.0f);
    gui_player_remember_volume_percent(new_percent);
    settings_save_async(&current_settings);
    show_volume_popup(new_percent);
    refresh_volume_topbar(new_percent);
}

/* Returns false when the action is not handled. */
static bool physical_button_dispatch(button_mapping_action_t action,
        button_mapping_gesture_t gesture, unsigned count, bool first) {
    switch (action) {
        case BUTTON_MAPPING_ACTION_SCREEN_TOGGLE: physical_button_screen_toggle(); break;
        case BUTTON_MAPPING_ACTION_POWER_MENU: physical_button_power_menu(); break;
        case BUTTON_MAPPING_ACTION_PLAY_PAUSE: if (!gui_player_transport_button(0)) toggle_play_pause(); break;
        case BUTTON_MAPPING_ACTION_NEXT: if (!gui_player_transport_button(1)) gui_player_step_manual(1); break;
        case BUTTON_MAPPING_ACTION_PREVIOUS: if (!gui_player_transport_button(-1)) gui_player_step_manual(-1); break;
        case BUTTON_MAPPING_ACTION_SEEK_FORWARD:
        case BUTTON_MAPPING_ACTION_SEEK_BACKWARD:
            gui_player_hw_seek_steps(action == BUTTON_MAPPING_ACTION_SEEK_FORWARD ? 1 : -1,
                (int)count, first || gesture == BUTTON_MAPPING_GESTURE_TAP ||
                gesture == BUTTON_MAPPING_GESTURE_DOUBLE);
            break;
        case BUTTON_MAPPING_ACTION_VOLUME_UP:
        case BUTTON_MAPPING_ACTION_VOLUME_DOWN: physical_button_volume_step(action, count); break;
        default: return false;
    }
    return true;
}

/* R3 II 2025 wheel clicks mapped to seek add up instead of seeking per click. */
static bool physical_button_wheel_seek(button_mapping_action_t action,
        button_mapping_button_t button, button_mapping_gesture_t gesture) {
#if defined(BOARD_R3II_2025)
    bool seek = action == BUTTON_MAPPING_ACTION_SEEK_FORWARD ||
                action == BUTTON_MAPPING_ACTION_SEEK_BACKWARD;
    bool wheel = button == BUTTON_MAPPING_VOLUME_UP || button == BUTTON_MAPPING_VOLUME_DOWN;
    if (!seek || !wheel || gesture != BUTTON_MAPPING_GESTURE_TAP) return false;
    player_seek_input_hw_click(action == BUTTON_MAPPING_ACTION_SEEK_FORWARD ? 1 : -1);
    return true;
#else
    (void)action; (void)button; (void)gesture;
    return false;
#endif
}

static void physical_button_action(button_mapping_action_t action,
        button_mapping_button_t button, button_mapping_gesture_t gesture,
        unsigned count, bool first, uint32_t timestamp_ms, void *userdata) {
    (void)timestamp_ms;
    (void)userdata;
    if (gesture != BUTTON_MAPPING_GESTURE_HOLD_REPEAT) count = 1;
    if (count > 100) count = 100;
    action = physical_button_resolve_action(action, button, gesture);
    if (action == BUTTON_MAPPING_ACTION_NONE) return;
    if (!physical_button_gesture_allowed(action, button, gesture)) return;
    if (!physical_button_wheel_seek(action, button, gesture) &&
        !physical_button_dispatch(action, gesture, count, first)) return;
    lv_display_trigger_activity(NULL);
}

static void process_physical_button_events(void) {
    if (resumed_from_suspend_pending &&
        lv_tick_elaps(resumed_from_suspend_tick) >= RESUME_POWER_DRAIN_WINDOW_MS)
        resumed_from_suspend_pending = false;
    if (hw_buttons_consume_button_lock_changed()) {
        physical_buttons_cancel_pending_taps();
        bool locked = hw_buttons_button_lock_is_locked();
        bool visual_lock = gui_lock_screen_is_showing();
        bool unlock = !locked && hw_buttons_button_lock_is_enabled();
        if (unlock && (!backlight_screen_is_on() || screen_wake_pending))
            screen_wake_unlock_requested = true;
        gui_lock_screen_button_lock_changed(locked);
        if (unlock && (!backlight_screen_is_on() || screen_wake_pending)) {
            request_screen_power(true);
            /* If this wake already selected a frame, require a new frame
             * after the visual unlock before the reveal gate can finish. */
            screen_wake_flush_seen = false;
            request_wake_frame_redraw();
        }
        if (!visual_lock)
            show_info_toast(locked ? TR("Buttons locked") : TR("Buttons unlocked"));
        plugin_manager_notify_button_lock_changed(locked);
    }
    hw_button_event_t event;
    while (hw_buttons_consume_event(&event)) {
        /* Any key event ends a scroll-blit request in progress: key handling shows popups and moves state
         * through paths that were not audited against the pixel copy (belt and braces over the
         * foreign-invalidation rule). */
        lv_refr_scroll_blit_poison(LV_SB_GUARD_KEY);
        if (hw_buttons_button_lock_is_locked() && event.button == BUTTON_MAPPING_POWER)
            physical_button_action(BUTTON_MAPPING_ACTION_DEFAULT, event.button, event.gesture,
                                   event.count, true, event.timestamp_ms, NULL);
        else
            button_mapping_event(&physical_button_mapping, event.button, event.gesture,
                                 event.count, event.timestamp_ms, physical_button_action, NULL);
    }
    /* Drain source events before expiring singles: a busy GUI must still
     * recognize two rapid hardware presses using their original times. */
    button_mapping_poll(&physical_button_mapping, physical_button_now_ms(),
                        physical_button_action, NULL);
}

#ifndef HOST_BUILD
/* Drives a RAM-staged wake frame; the caller returns after this. */
static void screen_wake_step_ram_stage(lv_display_t *display, uint32_t age, bool retry_due) {
    if (!screen_wake_frame_pending && !gui_lock_screen_unlock_pending())
        prepare_screen_wake_frame();
    if (backlight_screen_is_prepared() &&
        !gui_lock_screen_unlock_pending() &&
        (!gui_lock_screen_is_showing() || !gui_lock_screen_has_background_work(false)) &&
        lv_linux_fbdev_ram_stage_ready(display) &&
        lv_linux_fbdev_present_ram_stage(display)) {
        if (!lv_linux_fbdev_end_ram_stage(display)) return;
        screen_wake_ram_staged = false;
        last_presented_screen = lv_screen_active();
        full_redraw_async_cb(NULL);
        begin_screen_wake_reveal();
    } else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
        /* Allocation/format failures already use the ordinary path;
         * abandon a stalled render/present without revealing black. */
        if (!lv_linux_fbdev_end_ram_stage(display)) return;
        screen_wake_ram_staged = false;
        screen_wake_frame_pending = false;
        apply_screen_runtime_state(false);
        screen_wake_restart_phase();
    } else if (!backlight_screen_is_prepared() && retry_due) {
        screen_wake_retry_tick = lv_tick_get();
        backlight_prepare_screen_on();
    }
}

/* Returns true when the timer callback must return afterwards. */
static bool screen_wake_service_ram_stage(uint32_t age, bool retry_due) {
    lv_display_t *display = lv_display_get_default();
    if (!lv_linux_fbdev_ram_stage_valid(display)) {
        /* Finish a rotation/resolution cancellation while rendering is idle. */
        apply_screen_runtime_state(false);
        if (!lv_linux_fbdev_end_ram_stage(display)) return true;
        screen_wake_ram_staged = false;
        screen_wake_frame_pending = false;
        screen_wake_restart_phase();
        return false;
    }
    screen_wake_step_ram_stage(display, age, retry_due);
    return true;
}
#endif

static void screen_wake_service_reveal(uint32_t age, bool retry_due) {
    if (backlight_screen_is_visible()) finish_screen_wake_reveal();
    else if (screen_wake_retained_fast) {
        /* Retained scanout must stay gated until hardware confirms visible. */
        if (retry_due) {
            screen_wake_retry_tick = lv_tick_get();
            backlight_set_screen_on(true);
        }
    }
    else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
        fprintf(stderr, "Screen wake: brightness restore timed out\n");
        backlight_set_screen_on(true);
        finish_screen_wake_reveal();
    } else if (retry_due) {
        screen_wake_retry_tick = lv_tick_get();
        backlight_set_screen_on(true);
    }
}

static void screen_wake_service_frame_prep(uint32_t age, bool retry_due) {
    if (backlight_screen_is_prepared()) prepare_screen_wake_frame();
    else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
        fprintf(stderr, "Screen wake: dark preparation timed out, using plain unblank\n");
        backlight_set_screen_on(true);
        prepare_screen_wake_frame();
    } else if (retry_due) {
        screen_wake_retry_tick = lv_tick_get();
        backlight_prepare_screen_on();
    }
}

static void power_button_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (!screen_wake_pending) {
        if (ui_wake_enabled() && timer) lv_timer_pause(timer);
        return;
    }
    uint32_t age = lv_tick_elaps(screen_wake_phase_tick);
    bool retry_due = lv_tick_elaps(screen_wake_retry_tick) >= SCREEN_WAKE_RETRY_MS;
    if (screen_wake_scanout_pending) {
        if (retry_due) {
            screen_wake_retry_tick = lv_tick_get();
            (void) prepare_screen_wake_scanout();
        }
        return;
    }
#ifndef HOST_BUILD
    if (screen_wake_ram_staged && screen_wake_service_ram_stage(age, retry_due)) return;
#endif
    if (screen_wake_reveal_pending) {
        screen_wake_service_reveal(age, retry_due);
    } else if (!screen_wake_frame_pending) {
        screen_wake_service_frame_prep(age, retry_due);
    } else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
        fprintf(stderr, "Screen wake: frame presentation timed out\n");
        begin_screen_wake_reveal();
    } else if (!screen_wake_flush_seen && retry_due) {
        screen_wake_retry_tick = lv_tick_get();
        request_wake_frame_redraw();
    }
}

/* Keep input consumption separate from periodic status and service work. */
void media_button_timer_cb(lv_timer_t *timer) {
    (void)timer;
    process_physical_button_events();
    if (ui_wake_enabled() && media_input_timer) {
        bool pending = false;
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
            pending |= physical_button_mapping.pending_taps[i].pending;
        if (pending) lv_timer_resume(media_input_timer);
        else lv_timer_pause(media_input_timer);
    }
}

static void poll_screen_inactivity_timeout(void) {
    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);
    if (interactive_ui_started) {
        uint32_t interactive_age_ms = lv_tick_elaps(interactive_ui_start_tick);
        if (screen_inactive_ms > interactive_age_ms) screen_inactive_ms = interactive_age_ms;
    }
    bool screen_on_before_timeout = backlight_screen_is_on();
    /* The option keeps visible lyrics readable, including while paused.
     * Leaving lyrics or disabling it restores a full normal countdown.
     * Never wake a screen the user explicitly turned off. */
    bool lyrics_screen_active = current_settings.lyrics_keep_screen_on &&
        gui_lyrics_is_visible();
    if (lyrics_timeout_held && !lyrics_screen_active && screen_on_before_timeout) {
        gui_reset_interactive_timeout_baseline();
        screen_inactive_ms = 0;
    }
    lyrics_timeout_held = lyrics_screen_active && screen_on_before_timeout;
    uint32_t active_dim_delay_ms = (uint32_t) current_settings.screen_dim_delay_seconds * 1000;
    if (current_settings.screen_timeout_enabled && active_dim_delay_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        active_dim_delay_ms = UINT32_MAX; /* Prevent dimming if delay exceeds or equals timeout */
    }
    if (current_settings.screen_dimming_enabled && screen_on_before_timeout &&
        !inactivity_dimmed && !lyrics_screen_active && screen_inactive_ms >= active_dim_delay_ms) {
        backlight_set_dimmed(true);
        inactivity_dimmed = true;
    } else if (screen_on_before_timeout && inactivity_dimmed &&
               (!current_settings.screen_dimming_enabled || lyrics_screen_active || screen_inactive_ms < active_dim_delay_ms)) {
        backlight_set_dimmed(false);
        inactivity_dimmed = false;
    }
#ifndef HOST_BUILD
    /* The simulator has no power button to wake it, and a sleeping screen
     * disables every input, so the host never times out. */
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        !lyrics_screen_active &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        request_screen_power(false);
        inactivity_dimmed = false;
    }
#endif
}

/* Restarts the idle countdown when the screen just went dark, or when playback
 * ended while it was already dark. */
static void track_screen_off_start(bool just_slept, bool playing) {
    if (just_slept) {
        /* Just went to sleep this tick -- the 10-minute countdown starts
         * from here, not from whenever the inactivity that caused it
         * began. */
        screen_off_since_tick = lv_tick_get();
    } else if (screen_off_playback_active && !playing) {
        /* If playback ended while the screen was already off, restart the idle
         * timer from this moment so radio suspend and idle shutdown wait for
         * the full configured delay after playback stops. */
        screen_off_since_tick = lv_tick_get();
    }
    screen_off_playback_active = playing;
}

/* Brings the radios back when something needs them, and suspends them once
 * the screen has been off long enough with nothing going on. */
static void poll_radio_suspend(bool playing_now_for_idle, bool external_power_blocks_idle) {
    if (!radios_suspended && (playing_now_for_idle || external_power_blocks_idle || battery_is_charging() ||
        dac_receive_mode_active()))
        gui_network_resume_radio_services();
    /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
     * the latter forks a process, and this condition is checked every
     * tick while the screen is off, not throttled like refresh_bt_icon()
     * is. The cached value is frozen at whatever it was when the screen
     * went dark, which is fine: nothing in this app can change BT power
     * state without a UI the user can't reach with the screen off. */
    if (!radios_suspended && !idle_power_action_blocked(external_power_blocks_idle) &&
        lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS &&
        !shutdown_background_work_active() &&
        gui_network_prepare_radio_sleep()) {
        gui_shell_suspend_connections(&wifi_was_on_before_suspend, &bt_was_on_before_suspend);
        radios_suspended = true;
    }
}

#ifdef TEST_BUILD_TAG
/* Diagnostic logging for idle suspend/shutdown blockers. When an idle
 * deadline expires but background work is active, log which subsystems
 * are reporting busy (throttled to once every 30s). */
static void log_idle_blockers(bool external_power_blocks_idle) {
    uint32_t idle_elapsed_ms = lv_tick_elaps(screen_off_since_tick);
    bool radio_suspend_due = !radios_suspended && idle_elapsed_ms >= RADIO_SUSPEND_DELAY_MS;
    bool idle_action_due = !idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
                           idle_elapsed_ms >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000;
    if ((radio_suspend_due || idle_action_due) &&
        !idle_power_action_blocked(external_power_blocks_idle)) {
        bool library_busy = gui_library_has_background_work();
        bool subsonic_busy = gui_subsonic_has_background_work();
        bool network_busy = gui_network_has_background_work();
        bool lyrics_busy = gui_lyrics_has_background_work();
        bool player_busy = gui_player_has_background_work();
        bool shell_busy = gui_shell_has_background_work();
        bool plugins_busy = plugin_manager_has_background_work();
        bool playlist_write_busy = playlist_files_has_active_write() || gui_player_queue_write_busy();
        if (library_busy || subsonic_busy || network_busy || lyrics_busy || player_busy || shell_busy ||
            plugins_busy || playlist_write_busy) {
            static uint32_t last_busy_log_tick = 0;
            if (last_busy_log_tick == 0 || lv_tick_elaps(last_busy_log_tick) >= 30000) {
                last_busy_log_tick = lv_tick_get();
                DBG_LOG("gui: idle suspend/shutdown blocked -- library=%d subsonic=%d network=%d lyrics=%d "
                        "player=%d shell=%d plugins=%d playlist_write=%d\n",
                        library_busy, subsonic_busy, network_busy, lyrics_busy, player_busy, shell_busy,
                        plugins_busy, playlist_write_busy);
            }
        }
    }
}
#endif /* TEST_BUILD_TAG */

/* Powers the device off (or suspends it) once the configured idle time has
 * passed with the screen off. */
static void poll_idle_shutdown(bool external_power_blocks_idle) {
    /* Deliberately independent of radios_suspended -- idle_shutdown_minutes
     * is a separate, normally-longer setting than RADIO_SUSPEND_DELAY_MS,
     * and shutdown should still happen on its own schedule even if radio
     * suspend is impossible right now (e.g. wifi_dac_mode_enabled) --
     * except it explicitly shares the same DAC-mode/playing/external-
     * power/charging gate, since none of those should ever be interrupted
     * by the device powering itself off out from under them. */
    if (!idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
        !idle_power_action_blocked(external_power_blocks_idle) &&
        lv_tick_elaps(screen_off_since_tick) >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000 &&
        !shutdown_background_work_active() &&
        gui_network_prepare_radio_sleep()) {
        if (current_settings.idle_suspend_enabled) {
            if (!gui_shell_bt_dac_shutdown(true)) return;
            plugin_manager_notify_suspending();
            led_control_suspend();
            power_suspend_now();
            led_control_resume(current_settings.led_indicator_enabled);

            /* Restore UI state and reset inactivity timers on resume so the
             * display does not immediately time out again. */
            resume_from_suspend_fixups();
            plugin_manager_notify_system_resumed();

            /* Unlike idle_shutdown_now() (which never returns --
             * poweroff ends the process), this call CAN legitimately
             * return without a real user wake (e.g. a spurious IRQ) --
             * screen_just_woke won't fire to reset this flag in that
             * case, so reset it here instead. Worst case if the device
             * really did just wake for a moment is one immediate
             * re-suspend on the next tick, not getting stuck awake
             * indefinitely after a single spurious wake. */
            idle_shutdown_attempted = false;
        } else {
            (void) gui_shell_bt_dac_shutdown(false);
            idle_shutdown_attempted = true;
            idle_shutdown_now();
        }
    }
}

/* One control tick while the screen is off and no wake is pending. */
static void poll_screen_off_idle(bool screen_just_slept) {
    bool playing_now_for_idle = audio_is_playing();
    track_screen_off_start(screen_just_slept, playing_now_for_idle);
    gui_screen_off_memory_tick(screen_just_slept, playing_now_for_idle || audio_is_paused(), lv_tick_get());
    /* Cable presence and charger status are separate signals: a USB
     * source can remain connected while the battery reports discharging.
     * Treat UNKNOWN conservatively so background power actions never
     * suspend or power off while external power may still be present. */
    battery_external_power_state_t external_power_state = battery_get_external_power_state();
    bool external_power_blocks_idle =
        external_power_state != BATTERY_EXTERNAL_POWER_DISCONNECTED;
    poll_radio_suspend(playing_now_for_idle, external_power_blocks_idle);
#ifdef TEST_BUILD_TAG
    log_idle_blockers(external_power_blocks_idle);
#endif
    poll_idle_shutdown(external_power_blocks_idle);
}

static void poll_screen_transition_work(bool screen_on_now, bool screen_just_woke,
                                        bool screen_just_slept) {
    if (screen_just_woke) {
        gui_screen_off_memory_wake();
        inactivity_dimmed = false;
        idle_shutdown_attempted = false;
        if (radios_suspended) {
            gui_shell_resume_connections(wifi_was_on_before_suspend, bt_was_on_before_suspend);
            radios_suspended = false;
        }
    } else if (!screen_on_now && screen_wake_pending) {
        gui_screen_off_memory_restart_wait(lv_tick_get());
    } else if (!screen_on_now && !screen_wake_pending) {
        poll_screen_off_idle(screen_just_slept);
    }
}

screen_poll_result_t poll_screen_power(void) {
    bool screen_was_on = backlight_screen_is_on();

    /* Physical button events are consumed by media_button_timer_cb(). The power
     * action overlay itself has no timer to poll -- Power Off/Reboot fire
     * immediately on tap. */

    poll_screen_inactivity_timeout();

    /* Battery: skip every bit of UI-only refresh work below (label updates,
     * and especially the wpa_cli/bluetoothctl subprocess forks behind
     * refresh_wifi_icon()/refresh_bt_icon()) while nobody can see the
     * screen -- otherwise a music session left playing overnight with the
     * screen asleep still forks a bluetoothctl process every
     * WIFI_POLL_TICKS ticks for pixels nobody's looking at. Force one
     * refresh right on wake so nothing looks stale afterwards instead of
     * waiting up to WIFI_POLL_TICKS ticks to catch up. */
    bool screen_on_now = backlight_screen_is_on();
    bool screen_just_woke = screen_on_now && (!screen_was_on || force_screen_just_woke);
    if (screen_just_woke) force_screen_just_woke = false;
    bool screen_just_slept = !screen_on_now && (screen_was_on || force_screen_just_slept);
    force_screen_just_slept = false;
    if (screen_just_woke) gui_control_poll_kick_player_ui();
    if (screen_just_woke) gui_library_screen_woke();
    /* A wake can precede completion of the background RC stop. Retry its
     * restoration without blocking input or losing the saved preference. */
    if (screen_on_now) gui_network_resume_radio_services();

    /* Touch input and display refresh follow screen on/off state. Disabling indev
     * devices when screen is off prevents accidental touch events. Physical buttons
     * continue operating via their dedicated evdev thread. */
    if (!screen_wake_pending && screen_on_now != screen_was_on) {
        apply_screen_runtime_state(screen_on_now);
    }

    poll_screen_transition_work(screen_on_now, screen_just_woke, screen_just_slept);

    return (screen_poll_result_t){screen_on_now, screen_just_woke};
}

void gui_screen_power_init(void) {
    lv_display_add_event_cb(lv_display_get_default(), screen_wake_frame_ready_cb,
                            LV_EVENT_ALL, NULL);
}

void gui_screen_power_deinit(void) {
#ifndef HOST_BUILD
    if (screen_wake_ram_staged)
        lv_linux_fbdev_end_ram_stage(lv_display_get_default());
#endif
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), screen_wake_frame_ready_cb, NULL);
    screen_wake_clear_state();
    last_presented_screen = NULL;
    button_mapping_reset(&physical_button_mapping);
}

void gui_screen_power_start_timers(void) {
    power_input_timer = lv_timer_create(power_button_timer_cb, 25, NULL);
    media_input_timer = lv_timer_create(media_button_timer_cb, 16, NULL);
    if (ui_wake_enabled()) {
        lv_timer_pause(power_input_timer);
        lv_timer_pause(media_input_timer);
    }
}

void gui_screen_power_reset_idle_state(void) {
    backlight_set_dimmed(false);
    inactivity_dimmed = false;
    idle_shutdown_attempted = false;
}

void gui_buttons_init(void) {
    button_mapping_init(&physical_button_mapping);
}
