#include "gui.h"
#include "gui_external_controls.h"
#include "app_clock.h"
#include "i18n.h"
#include "gui_library.h"
#include "gui_lock_screen.h"
#include "gui_queue.h"
#include "ui_wake.h"
#include "event_input.h"
#include "gui_player.h"
#include "gui_plugins.h"
#include "gui_shell.h"
#include "gui_navigation.h"
#include "gui_setup.h"

#define SUBSONIC_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"
#include "gui_theme.h"
#include "gui_themes.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_network.h"
#include "gui_books.h"
#include "gui_text_input.h"
#include "gui_text_view.h"
#include "gui_lyrics.h"
#include "gui_track_info.h"
#include "gui_subsonic.h"
#include "assets.h"
#include "backlight.h"
#include "debug_log.h"
#include "airplay_control.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "battery.h"
#include "firmware_ota.h"
#include "plugin_store.h"
#include "wifi_status.h"
#include "audio.h"
#include "audio_output.h"
#include "file_browser.h"
#include "hw_buttons.h"
#include "metadata.h"
#include "metadata_db.h"
#include "peq.h"
#include "screen_builders.h"
#include "settings.h"
#include "db_log.h"
#include "subsonic_client.h"
#include "http_client.h"
#include "cover_decode.h"
#include "albumart.h"
#include "lyrics.h"
#include "lyrics_layout.h"
#include "remote_track.h"
#include "transition_compositor.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "hiby_sys_server.h"
#include "track_metadata_worker.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#include "bt_cover_art.h"
#endif
#include "headphone_status.h"
#include "plugin_manager.h"
#include "gui_plugin_manage.h"
#include "gui_plugin_store.h"
#include "gui_emulator_store.h"
#include "emulator_store.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "idle_shutdown.h"
#include "power_suspend.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "subprocess.h"
#include "timezone_data.h"
#include "timezone_apply.h"
#include "hostname_apply.h"
#include "../core/screenshot.h"

gui_busy_handle_t wifi_connect_token = 0;
gui_busy_handle_t wifi_connect_saved_token = 0;

#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/core/lv_obj_style.h"
#include "src/core/lv_obj_style_gen.h"
#include "src/layouts/flex/lv_flex.h"
#include "src/misc/lv_area.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "src/core/lv_refr.h"
#include "src/widgets/image/lv_image.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef UI_PERF_TRACE
#include "ui_frame_trace.h"
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
#endif

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point, per the layout the stock hiby_player uses. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* Persistent startup checkpoints share main.c's retained boot history. */
  extern void boot_checkpoint(const char * step);

  /* main.c's own best-effort `mount -t vfat .../sd_0` retry (see its own
   * comment on why this needs to exist at all -- no hotplug mechanism for
   * the internal SD card slot exists anywhere on this firmware). Called
   * once at boot from main.c already; poll_sd_card_hotplug() below calls
   * it again periodically so re-inserting the card after boot eventually
   * gets mounted too, not just the very first insertion main.c catches. */
  extern void mount_sd_card_if_needed(void);
#endif

lv_obj_t * stream_media_screen;
bool favorite_is_set = false;

/* Defined with the rest of the DLNA logic further down;
 * forward-declared here since poll_dlna_control belongs alongside
 * every other per-tick background check in update_timer_cb. */
static void poll_dlna_control(void);
void poll_wifi_connect_saved(void);
void poll_wifi_forget(void);
void poll_bt_scan(void);
void poll_bt_rate_query(void);
void poll_bt_rate_apply(void);
void poll_bt_connect(void);
void poll_bt_forget(void);

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

/* The panel dominates power while lit. Dim well before the configured full
 * timeout, but keep enough time for reading and never override an explicit
 * user's screen-off duration. */
#define VISIBLE_STATUS_POLL_TICKS 4 /* 2 seconds at the 500 ms control tick */
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

/* Set when gui_release_memory_for_external_session() dropped the snapshots,
 * so the resume rebuilds them only after a session that freed them. */
static bool external_session_released_memory;

/* Memory counters of this process for the emulator launch log; a field the
 * kernel does not report stays -1. */
static void process_memory_kb(long * rss, long * data, long * swap) {
    *rss = *data = *swap = -1;
    FILE * f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", rss) == 1) continue;
        if (sscanf(line, "VmData: %ld kB", data) == 1) continue;
        (void) sscanf(line, "VmSwap: %ld kB", swap);
    }
    fclose(f);
}

/* Called on the UI thread right before an emulator is spawned. The emulator
 * can need as much memory as the player holds (only emulators whose manifest
 * sets free_player_memory ask for this), and on a 64 MB device the
 * kernel would otherwise swap the player out. Frees every cache that is
 * rebuilt on use: transition and drawer snapshots, decoded images, thumbnails
 * and lock screen art. The library, plugins and audio stay. The screen is
 * repainted afterwards, and gui_resume_after_external_session() restarts the
 * snapshot rebuilds. Large buffers are mmap-sized in musl's allocator and go
 * back to the kernel on free; there is no malloc_trim in this musl. */
void gui_release_memory_for_external_session(void) {
    long rss_before = -1, data_before = -1, swap_before = -1;
    if (db_log_enabled()) process_memory_kb(&rss_before, &data_before, &swap_before);
    external_session_released_memory = true;
    gui_navigation_release_snapshots();
    transition_compositor_discard_vertical_base();
    gui_shell_release_memory_for_external_session();
    gui_player_release_memory_for_external_session();
    gui_library_release_memory_for_external_session();
    gui_lock_screen_release_memory_for_external_session();
    gui_lyrics_release_memory_for_external_session();
    /* Last, once no widget points at a descriptor that may be cached. */
    lv_image_cache_drop(NULL);
    if (db_log_enabled()) {
        long rss_after, data_after, swap_after;
        process_memory_kb(&rss_after, &data_after, &swap_after);
        db_log("EMULATOR", "release_memory rss_kb=%ld->%ld data_kb=%ld->%ld swap_kb=%ld->%ld",
               rss_before, rss_after, data_before, data_after, swap_before, swap_after);
        db_log_flush();
    }
}

/* Called on the UI thread after an external full-screen session (an
 * emulator) that blocked it and owned the framebuffer and input devices. The
 * session consumed the user's attention without any input reaching LVGL, so
 * inactivity, dimming and idle-shutdown state restart from now; button and
 * screenshot events queued by the evdev threads are stale; and the whole
 * screen is repainted over whatever the session left in the framebuffer. */
void gui_resume_after_external_session(void) {
    bool touch_held = main_loop_discard_pointer_input();
    gui_shell_reset_drag_state();
    gui_shell_set_external_touch_held(touch_held);
    main_loop_discard_elapsed_time();
    gui_reset_interactive_timeout_baseline();
    backlight_set_dimmed(false);
    inactivity_dimmed = false;
    idle_shutdown_attempted = false;
    hw_button_event_t event;
    while (hw_buttons_consume_event(&event)) {}
    (void) hw_buttons_consume_screenshot();
    /* Rebuilds the snapshots gui_release_memory_for_external_session() freed
     * (static screens in sliced timer passes, the rest asynchronously), and
     * the drawer's. */
    if (external_session_released_memory) {
        external_session_released_memory = false;
        gui_navigation_invalidate_theme_snapshots();
        quick_drawer_mark_snapshot_dirty();
    }
    full_redraw_async_cb(NULL);
}


static bool shutdown_background_work_active(void) {
    return gui_lock_screen_has_background_work(true) || gui_library_has_background_work() || gui_subsonic_has_background_work() ||
           gui_network_has_background_work() || gui_lyrics_has_background_work() ||
           gui_player_has_background_work() || gui_shell_has_background_work() ||
           plugin_manager_has_background_work() || playlist_files_has_active_write() ||
           gui_player_queue_write_busy() || screenshot_is_busy() || firmware_ota_busy() || plugin_store_busy() || emulator_store_busy();
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
/* A wake is prepared and painted with the backlight still off. */
static bool screen_wake_pending = false;
static bool screen_wake_unlock_requested = false;
static bool screen_wake_scanout_pending = false;
static bool screen_wake_notified = false;
static lv_timer_t *power_input_timer, *media_input_timer;
static lv_timer_t *player_ui_timer;
static uint32_t player_ui_timer_period_ms;
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

    int indev_timer_count = 0;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
        if (!read_timer) continue;
        ui_input_set_runtime_timer(indev, screen_on);
        indev_timer_count++;
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

static button_mapping_context_t physical_button_mapping;
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
    if (player_ui_timer) lv_timer_ready(player_ui_timer);
    force_screen_just_woke = true;
}

static void begin_screen_wake_reveal(void) {
    screen_wake_frame_pending = false;
    screen_wake_reveal_pending = true;
    screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
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
    full_redraw_async_cb(NULL);
    screen_wake_frame_pending = true;
    screen_wake_flush_seen = false;
    screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
    lv_timer_t * refresh = lv_display_get_refr_timer(lv_display_get_default());
    if (refresh) lv_timer_ready(refresh);
}

/* Called only while input/display refresh are stopped. A compositor still
 * owning scanout must finish before we can safely power up the panel. */
static bool prepare_screen_wake_scanout(void) {
#ifndef HOST_BUILD
    lv_display_t *display = lv_display_get_default();
    bool reusable_before = screen_wake_retained_screen &&
        lv_screen_active() == screen_wake_retained_screen &&
        !gui_navigation_transition_in_progress() &&
        !gui_lock_screen_is_showing() && !gui_lock_screen_unlock_pending() &&
        !screen_wake_unlock_requested && !screen_wake_power_menu &&
        lv_linux_fbdev_last_frame_reusable(display);
#endif
    if (!screen_wake_notified) {
        screen_wake_notified = true;
        gui_lock_screen_set_wake_unlock_override(screen_wake_unlock_requested);
        plugin_manager_notify_screen_woke();
        gui_lock_screen_set_wake_unlock_override(false);
    }
#ifndef HOST_BUILD
    if (reusable_before && lv_screen_active() == screen_wake_retained_screen &&
        !gui_navigation_transition_in_progress() &&
        !gui_lock_screen_is_showing() && !gui_lock_screen_unlock_pending() &&
        !screen_wake_unlock_requested && !screen_wake_power_menu &&
        lv_linux_fbdev_last_frame_reusable(display)) {
        /* Keep the exact pre-sleep normal frame on scanout while hardware
         * unblanks; ordinary refresh resumes after the visible acknowledgement. */
        screen_wake_retained_fast = true;
        /* Invalidating a widget resumes LVGL refresh even when its timer is
         * paused. Hold invalidation too; the reveal requests a full redraw. */
        lv_display_enable_invalidation(display, false);
        screen_wake_scanout_pending = false;
        screen_wake_reveal_pending = true;
        screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
        backlight_set_screen_on(true);
        return true;
    }
    if (!lv_linux_fbdev_clear_scanout(display)) return false;
#endif
    screen_wake_scanout_pending = false;
    screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
    backlight_prepare_screen_on();
#ifndef HOST_BUILD
    /* Render the complete LVGL composition into RAM during panel startup.
     * The driver leaves both physical pages black until the explicit commit. */
    if (lv_linux_fbdev_begin_ram_stage(display)) {
        screen_wake_ram_staged = true;
        prepare_screen_wake_frame();
    }
#endif
    return true;
}

static void request_screen_power(bool on) {
    if (on) {
        if (backlight_screen_is_on() || screen_wake_pending) return;
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
            button_mapping_set(&physical_button_mapping, (button_mapping_button_t)i,
                               &physical_button_mapping.bindings[i]);
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
        screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
        if (!prepare_screen_wake_scanout())
            fprintf(stderr, "Screen wake: waiting to clear stale scanout\n");
    } else {
#ifndef HOST_BUILD
        if (screen_wake_ram_staged &&
            !lv_linux_fbdev_end_ram_stage(lv_display_get_default())) return;
#endif
        screen_wake_ram_staged = false;
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
            button_mapping_set(&physical_button_mapping, (button_mapping_button_t)i,
                               &physical_button_mapping.bindings[i]);
        screen_wake_pending = false;
        screen_wake_unlock_requested = false;
        screen_wake_scanout_pending = false;
        screen_wake_notified = false;
        gui_lock_screen_set_wake_unlock_override(false);
        screen_wake_frame_pending = false;
        screen_wake_flush_seen = false;
        screen_wake_reveal_pending = false;
        if (screen_wake_retained_fast)
            lv_display_enable_invalidation(lv_display_get_default(), true);
        screen_wake_retained_fast = false;
        screen_wake_retained_screen = NULL;
        screen_wake_power_menu = false;
        backlight_set_screen_on(false);
        apply_screen_runtime_state(false);
    }
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

static void physical_button_action(button_mapping_action_t action,
        button_mapping_button_t button, button_mapping_gesture_t gesture,
        unsigned count, bool first, uint32_t timestamp_ms, void *userdata) {
    (void)timestamp_ms;
    (void)userdata;
    if (gesture != BUTTON_MAPPING_GESTURE_HOLD_REPEAT) count = 1;
    if (count > 100) count = 100;
    if (hw_buttons_button_lock_is_locked()) {
        if (button != BUTTON_MAPPING_POWER) return;
        if (gesture == BUTTON_MAPPING_GESTURE_TAP || gesture == BUTTON_MAPPING_GESTURE_DOUBLE)
            action = BUTTON_MAPPING_ACTION_SCREEN_TOGGLE;
        else if (gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST)
            action = BUTTON_MAPPING_ACTION_POWER_MENU;
        else
            return;
    } else if (action == BUTTON_MAPPING_ACTION_DEFAULT)
        action = physical_button_default_action(button, gesture);
    if (action == BUTTON_MAPPING_ACTION_NONE) return;
    /* The wake key can be replayed by evdev after suspend. Consume that
     * gesture without triggering its assignment a second time. */
    if (button == BUTTON_MAPPING_POWER && resumed_from_suspend_pending) {
        resumed_from_suspend_pending = false;
        return;
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
        return;
    }
    /* A mapped volume hold that crossed screen-off stays canceled until a
     * fresh gesture, matching the physical volume keys' wake behavior. */
    if (volume_action && hold && physical_volume_hold_cancelled[button]) return;
    if (!current_settings.setup_complete && !volume_action &&
        action != BUTTON_MAPPING_ACTION_SCREEN_TOGGLE &&
        action != BUTTON_MAPPING_ACTION_POWER_MENU) return;
    switch (action) {
        case BUTTON_MAPPING_ACTION_SCREEN_TOGGLE: {
            bool turn_on = !(backlight_screen_is_on() || screen_wake_pending);
            request_screen_power(turn_on);
            if (turn_on) inactivity_dimmed = false;
            break;
        }
        case BUTTON_MAPPING_ACTION_POWER_MENU:
            if (!backlight_screen_is_on()) {
                if (screen_wake_frame_pending || screen_wake_reveal_pending)
                    start_power_off_countdown();
                else screen_wake_power_menu = true;
                request_screen_power(true);
                inactivity_dimmed = false;
            } else start_power_off_countdown();
            break;
        case BUTTON_MAPPING_ACTION_PLAY_PAUSE: toggle_play_pause(); break;
        case BUTTON_MAPPING_ACTION_NEXT: gui_player_step_manual(1); break;
        case BUTTON_MAPPING_ACTION_PREVIOUS: gui_player_step_manual(-1); break;
        case BUTTON_MAPPING_ACTION_SEEK_FORWARD:
        case BUTTON_MAPPING_ACTION_SEEK_BACKWARD:
            gui_player_hw_seek_steps(action == BUTTON_MAPPING_ACTION_SEEK_FORWARD ? 1 : -1,
                (int)count, first || gesture == BUTTON_MAPPING_GESTURE_TAP ||
                gesture == BUTTON_MAPPING_GESTURE_DOUBLE);
            break;
        case BUTTON_MAPPING_ACTION_VOLUME_UP:
        case BUTTON_MAPPING_ACTION_VOLUME_DOWN: {
            int delta = (int)count * (action == BUTTON_MAPPING_ACTION_VOLUME_UP ? 1 : -1);
            /* Bluetooth accessories can change the effective audio volume
             * before the next UI reconciliation tick. Step from the audio
             * state so a physical button never starts from a stale slider. */
            int current_percent = audio_clamp_volume_percent(
                (int)(audio_get_volume() * 100.0f + 0.5f));
            int new_percent = audio_clamp_volume_percent(current_percent + delta);
            gui_player_set_volume_percent(new_percent);
            audio_set_volume((float)new_percent / 100.0f);
            gui_player_remember_volume_percent(new_percent);
            settings_save_async(&current_settings);
            show_volume_popup(new_percent);
            refresh_volume_topbar(new_percent);
            break;
        }
        default: return;
    }
    lv_display_trigger_activity(NULL);
}

static void process_physical_button_events(void) {
    if (resumed_from_suspend_pending &&
        lv_tick_elaps(resumed_from_suspend_tick) >= RESUME_POWER_DRAIN_WINDOW_MS)
        resumed_from_suspend_pending = false;
    if (hw_buttons_consume_button_lock_changed()) {
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
            button_mapping_set(&physical_button_mapping, (button_mapping_button_t)i,
                               &physical_button_mapping.bindings[i]);
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
            full_redraw_async_cb(NULL);
            lv_timer_t * refresh = lv_display_get_refr_timer(lv_display_get_default());
            if (refresh) lv_timer_ready(refresh);
        }
        if (!visual_lock)
            show_info_toast(locked ? TR("Buttons locked") : TR("Buttons unlocked"));
        plugin_manager_notify_button_lock_changed(locked);
    }
    hw_button_event_t event;
    while (hw_buttons_consume_event(&event)) {
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
    if (screen_wake_ram_staged) {
        lv_display_t *display = lv_display_get_default();
        if (!lv_linux_fbdev_ram_stage_valid(display)) {
            /* Finish a rotation/resolution cancellation while rendering is idle. */
            apply_screen_runtime_state(false);
            if (!lv_linux_fbdev_end_ram_stage(display)) return;
            screen_wake_ram_staged = false;
            screen_wake_frame_pending = false;
            screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
        } else {
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
                screen_wake_phase_tick = screen_wake_retry_tick = lv_tick_get();
            } else if (!backlight_screen_is_prepared() && retry_due) {
                screen_wake_retry_tick = lv_tick_get();
                backlight_prepare_screen_on();
            }
            return;
        }
    }
#endif
    if (screen_wake_reveal_pending) {
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
    } else if (!screen_wake_frame_pending) {
        if (backlight_screen_is_prepared()) prepare_screen_wake_frame();
        else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
            fprintf(stderr, "Screen wake: dark preparation timed out, using plain unblank\n");
            backlight_set_screen_on(true);
            prepare_screen_wake_frame();
        } else if (retry_due) {
            screen_wake_retry_tick = lv_tick_get();
            backlight_prepare_screen_on();
        }
    } else if (age >= SCREEN_WAKE_TIMEOUT_MS) {
        fprintf(stderr, "Screen wake: frame presentation timed out\n");
        begin_screen_wake_reveal();
    } else if (!screen_wake_flush_seen && retry_due) {
        screen_wake_retry_tick = lv_tick_get();
        full_redraw_async_cb(NULL);
        lv_timer_t * refresh = lv_display_get_refr_timer(lv_display_get_default());
        if (refresh) lv_timer_ready(refresh);
    }
}

/* Keep input consumption separate from periodic status and service work. */
static void media_button_timer_cb(lv_timer_t *timer) {
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

void gui_process_input_wake(void) {
    media_button_timer_cb(NULL);
    poll_cover_decode();
    if (player_ui_timer) lv_timer_ready(player_ui_timer);
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

    /* Accessory volume is applied by the BlueALSA monitor thread. Keep its
     * UI/persisted mirror on this fast player cadence, including while the
     * screen is off. Leave the pending signal queued during a slider drag;
     * after the drag, mirror the current audio value so an older queued
     * accessory percentage cannot roll back a newer local adjustment. */
    if (!gui_player_volume_is_being_adjusted()) {
        int ignored_bt_percent;
        if (bt_control_source_volume_sync_consume_percent(&ignored_bt_percent)) {
            int bt_volume_percent = audio_clamp_volume_percent(
                (int)(audio_get_volume() * 100.0f + 0.5f));
            gui_player_set_volume_percent(bt_volume_percent);
            gui_player_remember_volume_percent(bt_volume_percent);
            settings_save_async(&current_settings);
            show_volume_popup(bt_volume_percent);
            refresh_volume_topbar(bt_volume_percent);
        }
    }

    uint64_t playback_err_gen = 0;
    audio_error_t playback_err = audio_consume_error_ex(&playback_err_gen);
    if (playback_err != AUDIO_ERROR_NONE) {
        gui_player_handle_playback_error_ex(playback_err, playback_err_gen);
    } else if (audio_consume_track_advanced()) {
        gui_player_handle_auto_advance();
    } else if (audio_consume_track_finished()) {
        gui_player_handle_track_finished();
    }

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

static void poll_hardware_status(void) {
    /* Also unconditional on screen state -- charging happens with the
     * screen off far more often than on, so gating this the same way as the
     * topbar refreshes above would leave it capped at whatever percent the
     * screen happened to be on last. charge_limiter_poll() throttles its
     * own actual sysfs work internally, so calling it every tick here is
     * cheap. */
    charge_limiter_poll(current_settings.charge_limiter_enabled, false);
#ifdef UI_PERF_TRACE
    update_perf_mark("charge_limit");
#endif
    safe_charging_poll(current_settings.safe_charging_enabled, false);
#ifdef UI_PERF_TRACE
    update_perf_mark("safe_charge");
#endif
    led_control_poll(current_settings.led_indicator_enabled);
#ifdef UI_PERF_TRACE
    update_perf_mark("led_status");
#endif
    int volume_percent = (int) (audio_get_volume() * 100.0f + 0.5f);
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    plugin_manager_notify_volume_changed(volume_percent);
    plugin_manager_poll_battery();
#ifdef UI_PERF_TRACE
    update_perf_mark("plugin_battery");
#endif
    headphone_status_refresh_earpods_adc();
#ifdef UI_PERF_TRACE
    update_perf_mark("hardware");
#endif
}

static void poll_background_jobs(void) {
    /* Polling for in-flight async operations (wifi/bt connect, library scan,
     * subsonic sync, ...) keeps running regardless of screen state -- these
     * only do real work when a request the user already made is still
     * pending, so gating them on screen state would leave that operation
     * stuck until the user wakes the screen back up. */
    poll_subsonic_download();
    poll_firmware_ota();
    poll_plugin_store();
    poll_emulator_store();
    gui_plugin_manage_poll();
    poll_subsonic_library_download();
    poll_dlna_control();
    poll_subsonic_connect();
    poll_subsonic_browse();
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
    poll_library_rescan();
    poll_sd_format();
    poll_remote_import();
#ifdef UI_PERF_TRACE
    update_perf_mark("network_jobs");
#endif
    poll_usb_mode_switch();
#ifdef UI_PERF_TRACE
    update_perf_mark("usb_mode");
#endif
    poll_usb_storage_hotplug();
#ifdef UI_PERF_TRACE
    update_perf_mark("usb_hotplug");
#endif
    poll_sd_card_hotplug();
#ifdef UI_PERF_TRACE
    update_perf_mark("sd_hotplug");
#endif
    gui_library_poll_playlists();
    gui_library_poll_track_probes();
#ifdef UI_PERF_TRACE
    update_perf_mark("storage_jobs");
#endif
    gui_player_service_artwork_cache();
    poll_search_job();
    gui_navigation_poll_snapshot_rebuild();
    gui_library_poll_now_playing_indicators();
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
    gui_queue_poll();
    gui_network_poll_airplay_overlay();
    gui_track_info_poll();
    gui_library_poll_boot_prompt();
#ifdef UI_PERF_TRACE
    update_perf_mark("player_jobs");
#endif
}

#ifndef HOST_BUILD
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
    /* Bluetooth disconnect is debounced on wall-clock time (12s) to prevent
     * transient bluealsactl busy polling failures from falsely cutting playback.
     * Wired headphone disconnects remain immediate via direct sysfs reads. */
#define BT_OUTPUT_DISCONNECT_DEBOUNCE_MS 12000
    {
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
    }

    /* Synchronize balanced output routing for hardware supporting balanced jacks. */
    audio_output_sync_balanced_output();
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
 * Only triggers if a track is actively playing or paused. */
static void poll_car_mode(void) {
    {
        enum { CAR_POWER_REMOVAL_DEBOUNCE_POLLS = 3 };
        static bool power_sample_initialized = false;
        static bool last_external_power = false;
        static unsigned int power_absent_polls = 0;
        static bool car_shutdown_pending = false;

        if (!current_settings.car_mode_enabled) {
            /* Re-sample from a clean baseline if Car Mode is enabled later;
             * enabling it while already unplugged is not itself a removal
             * edge and must not immediately power the device off. */
            power_sample_initialized = false;
            power_absent_polls = 0;
            car_shutdown_pending = false;
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
            battery_external_power_state_t power_state = battery_get_external_power_state();

            if (power_state == BATTERY_EXTERNAL_POWER_UNKNOWN) {
                /* Consecutive means consecutive valid samples.  A sysfs
                 * failure is not evidence of either state and must not let
                 * two old off samples plus one later off sample confirm a
                 * removal.  An already-confirmed pending shutdown is left
                 * intact; only a positive connected sample cancels it. */
                power_absent_polls = 0;
            } else {
                bool external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
                if (!power_sample_initialized) {
                    last_external_power = external_power;
                    power_sample_initialized = true;
                    power_absent_polls = 0;
                    DBG_LOG("car_mode: initial external power=%d\n", external_power);
                } else if (external_power) {
                    if (!last_external_power || power_absent_polls > 0 || car_shutdown_pending)
                        DBG_LOG("car_mode: external power present; removal cancelled\n");
                    last_external_power = true;
                    power_absent_polls = 0;
                    car_shutdown_pending = false;
                } else if (last_external_power) {
                    if (power_absent_polls < CAR_POWER_REMOVAL_DEBOUNCE_POLLS) power_absent_polls++;
                    if (power_absent_polls >= CAR_POWER_REMOVAL_DEBOUNCE_POLLS) {
                        last_external_power = false;
                        car_shutdown_pending = audio_is_playing() || audio_is_paused();
                        DBG_LOG("car_mode: external power removal confirmed; shutdown_pending=%d\n",
                                car_shutdown_pending);
                    }
                }
            }
        }

        if (car_shutdown_pending && !shutdown_background_work_active()) {
            current_settings.last_position = audio_get_resume_position_seconds();
            /* This save is intentionally synchronous: idle_shutdown_now() is
             * imminent, so the final playback checkpoint must reach storage
             * before power is removed. */
            settings_save(&current_settings);
            idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
        }
    }
}
#endif

typedef struct {
    bool on;
    bool just_woke;
} screen_poll_result_t;

static void poll_screen_inactivity_timeout(void) {
    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);
    if (interactive_ui_started) {
        uint32_t interactive_age_ms = lv_tick_elaps(interactive_ui_start_tick);
        if (screen_inactive_ms > interactive_age_ms) screen_inactive_ms = interactive_age_ms;
    }
    bool screen_on_before_timeout = backlight_screen_is_on();
    /* Keep the visible lyrics readable during playback only. A pause, page
     * exit or disabled option restores a full normal inactivity countdown.
     * Never wake a screen the user explicitly turned off. */
    lv_obj_t * lyrics_screen = gui_lyrics_get_screen();
    bool lyrics_screen_active = current_settings.lyrics_keep_screen_on && lyrics_screen &&
        lv_screen_active() == lyrics_screen && audio_is_playing();
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
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        !lyrics_screen_active &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        request_screen_power(false);
        inactivity_dimmed = false;
    }
}

static screen_poll_result_t poll_screen_power(void) {
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
    if (screen_just_woke && player_ui_timer) lv_timer_ready(player_ui_timer);

    /* Touch input and display refresh follow screen on/off state. Disabling indev
     * devices when screen is off prevents accidental touch events. Physical buttons
     * continue operating via their dedicated evdev thread. */
    if (!screen_wake_pending && screen_on_now != screen_was_on) {
        apply_screen_runtime_state(screen_on_now);
    }

    if (screen_just_woke) {
        inactivity_dimmed = false;
        idle_shutdown_attempted = false;
        if (radios_suspended) {
            gui_shell_resume_connections(wifi_was_on_before_suspend, bt_was_on_before_suspend);
            radios_suspended = false;
        }
    } else if (!screen_on_now && !screen_wake_pending) {
        bool playing_now_for_idle = audio_is_playing();
        if (screen_was_on) {
            /* Just went to sleep this tick -- the 10-minute countdown starts
             * from here, not from whenever the inactivity that caused it
             * began. */
            screen_off_since_tick = lv_tick_get();
        } else if (screen_off_playback_active && !playing_now_for_idle) {
            /* If playback ended while the screen was already off, restart the idle
             * timer from this moment so radio suspend and idle shutdown wait for
             * the full configured delay after playback stops. */
            screen_off_since_tick = lv_tick_get();
        }
        screen_off_playback_active = playing_now_for_idle;
        /* Cable presence and charger status are separate signals: a USB
         * source can remain connected while the battery reports discharging.
         * Treat UNKNOWN conservatively so background power actions never
         * suspend or power off while external power may still be present. */
        battery_external_power_state_t external_power_state = battery_get_external_power_state();
        bool external_power_blocks_idle =
            external_power_state != BATTERY_EXTERNAL_POWER_DISCONNECTED;
        /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
         * the latter forks a process, and this condition is checked every
         * tick while the screen is off, not throttled like refresh_bt_icon()
         * is. The cached value is frozen at whatever it was when the screen
         * went dark, which is fine: nothing in this app can change BT power
         * state without a UI the user can't reach with the screen off. */
        if (!radios_suspended && !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC &&
            !audio_is_playing() && !external_power_blocks_idle && !battery_is_charging() &&
            !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS) {
            gui_shell_suspend_connections(&wifi_was_on_before_suspend, &bt_was_on_before_suspend);
            radios_suspended = true;
        }

#ifdef TEST_BUILD_TAG
        /* Diagnostic logging for idle suspend/shutdown blockers. When an idle
         * deadline expires but background work is active, log which subsystems
         * are reporting busy (throttled to once every 30s). */
        uint32_t idle_elapsed_ms = lv_tick_elaps(screen_off_since_tick);
        bool radio_suspend_due = !radios_suspended && idle_elapsed_ms >= RADIO_SUSPEND_DELAY_MS;
        bool idle_action_due = !idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
                               idle_elapsed_ms >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000;
        if ((radio_suspend_due || idle_action_due) &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC && !audio_is_playing() &&
            !external_power_blocks_idle && !battery_is_charging()) {
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
#endif /* TEST_BUILD_TAG */

        /* Deliberately independent of radios_suspended -- idle_shutdown_minutes
         * is a separate, normally-longer setting than RADIO_SUSPEND_DELAY_MS,
         * and shutdown should still happen on its own schedule even if radio
         * suspend is impossible right now (e.g. wifi_dac_mode_enabled) --
         * except it explicitly shares the same DAC-mode/playing/external-
         * power/charging gate, since none of those should ever be interrupted
         * by the device powering itself off out from under them. */
        if (!idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC &&
            !audio_is_playing() && !external_power_blocks_idle && !battery_is_charging() &&
            !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000) {
            if (current_settings.idle_suspend_enabled) {
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
                idle_shutdown_attempted = true;
                idle_shutdown_now();
            }
        }
    }

    return (screen_poll_result_t){screen_on_now, screen_just_woke};
}

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

#ifdef UI_PERF_TRACE
    update_perf_mark("controls");
#endif
    if (screen.on) {
        gui_shell_update_topbar(screen.just_woke);
    }
#ifdef UI_PERF_TRACE
    update_perf_mark("topbar");
#endif

    poll_hardware_status();

    if (current_settings.remote_control_enabled) {
        gui_player_sync_remote_queue();
        gui_external_controls_publish_status(favorite_is_set);
    }

    poll_background_jobs();
}



/* Same true-black default screen_builders.c's own style_theme_screen_bg is
 * initialized with. Kept as a separate plain literal (not that shared
 * style) specifically for gui_show_boot_splash() below -- that function
 * runs from main.c before gui_init() (and screen_builders_init_list_row_style(),
 * which initializes style_theme_screen_bg) has ever run, so the shared
 * style would still be all-zero memory at that point. */
#define SCREEN_BG_COLOR lv_color_make(0, 0, 0)

/* The loader owns the boot artwork. Zero disables both the player splash
 * render and its minimum display wait. */
#define BOOT_SPLASH_MIN_DISPLAY_MS 0

#if BOOT_SPLASH_MIN_DISPLAY_MS > 0
/* Real device uptime (lv_tick_get() is CLOCK_MONOTONIC-backed, see main.c's
 * custom_tick_get()) at which gui_show_boot_splash() below was called --
 * read back by gui_init() near the end of its own setup to decide how much
 * longer, if any, the splash needs to stay up. 0 is never a real tick value
 * this far into boot, so it doubles as "splash was never shown". */
static uint32_t boot_splash_start_tick = 0;
#endif

/* With a nonzero duration, displays the boot splash image immediately after
 * framebuffer initialization. At zero this function does not touch LVGL.
 *
 * Uses asset_path("boot_animation/en/0.jpg"), matching the JPEG drawn by the
 * bootloader to provide a seamless visual handoff without decode discrepancies.
 *
 * An override placed at /usr/data/theme_overrides/boot_animation/en/0.jpg is
 * picked up automatically via asset_path(). If the file does not exist,
 * SCREEN_BG_COLOR provides a clean black background.
 *
 * lv_layer_top() is hidden while the splash is displayed so status bar icons
 * and overlays built during gui_init() do not appear until the splash finishes. */
void gui_show_boot_splash(void) {
#if BOOT_SPLASH_MIN_DISPLAY_MS > 0
    boot_splash_start_tick = lv_tick_get();

    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * scr = lv_obj_create(NULL);
    /* Plain literal, not style_theme_screen_bg -- this runs from main.c
     * before gui_init() (and its lv_style_init(&style_theme_screen_bg))
     * has ever run, so that style object would still be all-zero memory
     * here. */
    lv_obj_set_style_bg_color(scr, SCREEN_BG_COLOR, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, asset_path("boot_animation/en/0.jpg"));
    lv_obj_center(img);

    lv_screen_load(scr);
    lv_timer_handler(); /* force an immediate render -- nothing else pumps the loop until main.c's own main loop starts */
#endif
}





const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}


const char * gui_plugin_get_play_mode(void) {
    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ALL: return "repeat_all";
        case PLAY_MODE_REPEAT_ONE: return "repeat_one";
        case PLAY_MODE_SHUFFLE:    return "shuffle";
        case PLAY_MODE_SHUFFLE_ONCE: return "shuffle_once";
        case PLAY_MODE_SEQUENTIAL:
        default:                   return "sequential";
    }
    return "sequential";
}

const char * gui_plugin_get_current_track_path(void) {
    return gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
}

/* A stuck read on a corrupted SD card block (confirmed on a real device via
 * /proc/<pid>/task/<tid>/status+wchan showing the main thread parked in
 * __bread_gfp, a kernel block-device read, after the card came back from an
 * unclean unmount) lands the calling thread in an uninterruptible (D-state)
 * kernel wait. That can't be interrupted by a signal, alarm(), or
 * pthread_cancel() -- the only way to bound it is to run the risky call on
 * its own throwaway thread and simply stop waiting on it if it doesn't
 * finish in time. On timeout, the thread and its heap-allocated work struct
 * below are deliberately never joined or freed: the worker may still be
 * blocked in the kernel indefinitely, joining it would just reintroduce the
 * exact hang this exists to avoid, and freeing memory it might still write
 * to would be a use-after-free. This leaks one thread and one small
 * allocation per genuinely stuck path for the life of the process, which
 * only happens on real filesystem corruption -- a bounded cost, and far
 * better than the whole UI freezing. */
/* Library scan moved to gui_library.c */

/* Search bindings and the All Songs, Recently Added, and grouped-song
 * implementations moved to gui_library.c. */


/* ---- Subsonic-compatible network streaming ----
 *
 * mp3/flac songs (the two decoders audio.c can open against a true network
 * stream -- see decoder_open()'s own comment in audio.c) play directly off
 * the server via subsonic_song_row_click_cb() below, no local file at all.
 * Every other format (aac/ogg/wma/m4a/ape/opus/wav/aiff/dsf -- whatever
 * this server's own transcoding settings hand back) still downloads the
 * whole track first via the mechanism right below, before handing it to the
 * existing local-file-only decoder for that format: retrofitting every
 * remaining decoder here (the vendored AIFF/DSD/AAC/ALAC/APE ones, all
 * built around a plain seekable FILE*) for streaming reads is a much bigger
 * project than this round's scope. The downloaded copy lands on the SD card
 * (SUBSONIC_STREAM_CACHE_DIR below), not /tmp -- see that macro's own
 * comment for why. */


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



static lv_obj_t * build_stream_media_screen(void) {
    static icon_grid_item_t items[1 + PLUGIN_MAX_STREAM_TILES];
    items[0] = (icon_grid_item_t){ "stream_media/subsonic_row.png", "stream_media/subsonic_row_s.png", TR("Subsonic"), subsonic_tile_cb, NULL };

    int count = 1;
    int plugin_count = plugin_manager_get_stream_tile_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_STREAM_TILES; i++) {
        items[count++] = (icon_grid_item_t){
            plugin_manager_get_stream_tile_icon(i), plugin_manager_get_stream_tile_icon_selected(i),
            plugin_manager_get_stream_tile_label(i), plugin_stream_tile_click_cb, (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_category_menu_screen(TR("Stream Media"), generic_back_cb, items, count,
                                                &launcher_layout_config.stream_media);
    finalize_screen_navigation(scr);
    return scr;
}

/* build_stream_media_screen() is static to this file, so gui_reload.c's
 * in-process UI reload (which needs to tear down and rebuild the same
 * screen gui_init() below builds once at boot) goes through these two
 * wrappers instead of calling it directly. */
void gui_stream_media_teardown(void) {
    if (stream_media_screen) {
        lv_obj_delete(stream_media_screen);
        stream_media_screen = NULL;
    }
}

void gui_stream_media_rebuild(void) {
    stream_media_screen = build_stream_media_screen();
}

void gui_stream_media_refresh(void) {
    lv_obj_t * old = stream_media_screen;
    lv_obj_t * fresh = build_stream_media_screen();
    if (!fresh) return;
    stream_media_screen = fresh;
    gui_navigation_replace_static_screen(2, old, fresh);
    if (old) lv_obj_delete(old);
}

void gui_init(uint32_t screen_width, uint32_t screen_height) {
    lv_display_add_event_cb(lv_display_get_default(), screen_wake_frame_ready_cb,
                            LV_EVENT_ALL, NULL);
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("settings_load begin");
#endif
    settings_load(&current_settings);
#ifndef HOST_BUILD
    audio_output_set_usb_dsd_mode(current_settings.usb_dsd_output);
    audio_output_set_dac_preferences(current_settings.dac_filter, current_settings.dac_nos,
                                     current_settings.dac_dre);
#endif
#ifndef HOST_BUILD
    boot_checkpoint("settings_load done; settings application begin");
#endif
    i18n_set_language(current_settings.language);
    gui_display_apply_rotation(current_settings.screen_upside_down);
    bt_control_restore_codec_preference(current_settings.bt_codec);
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    bt_control_set_sample_rate(current_settings.bt_sample_rate);
    db_log_set_enabled(current_settings.db_logging_enabled);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
    hw_buttons_set_volume_lock_screen_off(current_settings.volume_lock_screen_off);
    button_mapping_init(&physical_button_mapping);
    gui_apply_button_bindings();
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
    headphone_status_refresh_earpods_adc();
    app_clock_init(current_settings.clock_automatic, current_settings.clock_manual_epoch,
                   current_settings.clock_system_reference);
#ifndef HOST_BUILD
    boot_checkpoint("settings application done");
#endif
    /* Must run before anything could turn Wi-Fi/Bluetooth on and trigger
     * wifi_on.sh/bt_init's own one-time read of the file this bind-mounts
     * over -- see hostname_apply()'s own comment. */
    gui_theme_init();
    gui_notifications_init();
#ifndef HOST_BUILD
    boot_checkpoint("hostname_apply begin");
#endif
    hostname_apply(current_settings.hostname);
#ifndef HOST_BUILD
    boot_checkpoint("hostname_apply done");
#endif

    /* Must run before any screen below captures a gui_theme_font(GUI_FONT_ROLE_SUBTEXT)/20/22/28
     * pointer into its own style -- see this function's own doc comment. */

    /* Correct the persisted USB mode against live gadget state before
     * anything else reads it (external_dac_block_reason() in particular --
     * a stale usb_mode==USB_MODE_DAC left over from a previous run would
     * otherwise wrongly block local playback from the moment the app
     * starts). Hardware USB gadget state doesn't survive a reboot, but this
     * binary can also be killed and relaunched without a reboot (as it was
     * repeatedly during development), which is exactly when the persisted
     * value and reality can disagree. */
    usb_mode_t detected_usb_mode;
#ifndef HOST_BUILD
    boot_checkpoint("USB mode discovery begin");
#endif
    bool detected = usb_mode_control_detect_current(&detected_usb_mode);
#ifndef HOST_BUILD
    boot_checkpoint("USB mode discovery done");
#endif
    if (detected) current_settings.usb_mode = (int) detected_usb_mode;

    /* A DAC gadget can survive a player restart. Recover its saved Type-C
     * and charging policy before the first charging poll can enable power. */
    if (!usb_mode_control_set_dac_charging_disabled(current_settings.usb_dac_disable_charging))
        fprintf(stderr, "gui: could not apply the USB DAC charging preference\n");
    if (!usb_mode_control_reconcile_dac_session())
        fprintf(stderr, "gui: USB DAC session recovery incomplete; retaining conservative power policy\n");

    /* ADB is the one mode that survives a restart, because enabling it means
     * going into Developer Options and asking for it. Anything else starts as
     * Storage: DAC in particular would otherwise block local playback on a
     * device that may not even be plugged into anything.
     *
     * Applied through the ordinary switch path rather than inline --
     * usb_mode_control_apply() spends seconds in settle and retry loops, which
     * would stall startup if it ran on this thread. poll_usb_mode_switch()
     * completes it in the background. */
    if (!detected) {
        if (current_settings.usb_mode == (int) USB_MODE_ADB) {
            start_usb_mode_switch(USB_MODE_ADB);
        } else if (current_settings.usb_mode != (int) USB_MODE_STORAGE) {
            current_settings.usb_mode = (int) USB_MODE_STORAGE;
            settings_save(&current_settings);
        }
    }

    /* current_settings.volume itself is left untouched here even when the
     * fixed-startup path below is taken -- it keeps tracking "last used"
     * independently (still updated normally by volume_slider_event_cb's
     * RELEASED case during the session), so turning startup_volume_fixed_enabled
     * back off later resumes from wherever the slider was really last left,
     * not from stale fixed-mode state. */
    audio_set_max_volume_percent(current_settings.max_volume_percent);
    if (current_settings.car_mode_enabled) {
        audio_set_volume((float) current_settings.car_mode_volume_percent / 100.0f);
    } else {
        audio_set_volume(current_settings.startup_volume_fixed_enabled
                              ? (float) current_settings.startup_volume_fixed_percent / 100.0f
                              : current_settings.volume); /* picked up below when the volume slider reads audio_get_volume() */
    }
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);

    /* Apply saved brightness level at startup. */
    backlight_set_normal_percent(current_settings.brightness_percent);

#ifndef HOST_BUILD
    boot_checkpoint("LED charging timezone setup begin");
#endif
    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    if (current_settings.timezone[0] != '\0') timezone_apply(current_settings.timezone);
#ifndef HOST_BUILD
    boot_checkpoint("LED charging timezone setup done");
#endif

    /* Receiver/server modes are session-only:
     * Never restore AirPlay, DLNA, or Remote Control automatically after a
     * process start to avoid unintentional listener exposure and resource usage.
     * Remote Control can use either Wi-Fi or Bluetooth after explicit activation. */
    bool network_modes_changed = current_settings.wifi_dac_mode_enabled ||
                                 current_settings.dlna_renderer_enabled ||
                                 current_settings.remote_control_enabled;
    current_settings.wifi_dac_mode_enabled = false;
    current_settings.dlna_renderer_enabled = false;
    current_settings.remote_control_enabled = false;
    if (network_modes_changed) settings_save_async(&current_settings);

    /* The quick drawer's own open/close drag tracking is polled from
     * update_timer_cb instead (poll_quick_drawer_drag()) -- see its comment
     * for why event-based approaches (both indev-wide LV_EVENT_PRESSED/
     * PRESSING and per-object LV_EVENT_GESTURE bubbling) proved unreliable
     * here specifically. */

    /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own comment
     * on this same property for why COVER flattens on.png's handle and
     * track into one indistinguishable solid color. Mirrored here since
     * this is the initial setup at boot, before apply_accent_color() might
     * ever run again. */



    /* Initialize native theme service and legacy migration before plugin_manager_init */
#ifndef HOST_BUILD
    boot_checkpoint("gui_themes_init begin");
#endif
    gui_themes_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_themes_init done");
#endif

    /* Discovers plugin rows/tiles by loading and running every .lua file
     * under <SD card>/.plugins/ -- run early, well before Books, Settings,
     * or Stream Media (the current plugin entry points, see build_books_
     * screen()/build_settings_screen()/build_stream_media_screen()) could
     * plausibly be reached. */
#ifndef HOST_BUILD
    boot_checkpoint("plugin_manager_init begin");
#endif
    plugin_manager_init();
#ifndef HOST_BUILD
    boot_checkpoint("plugin_manager_init done");
#endif

    /* Apply active native theme on top of fresh plugin baseline before screens are built */
#ifndef HOST_BUILD
    boot_checkpoint("gui_themes_apply_active begin");
#endif
    gui_themes_apply_active();
#ifndef HOST_BUILD
    boot_checkpoint("gui_themes_apply_active done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("pre-screen-build setup done");
#endif

#ifndef HOST_BUILD
    boot_checkpoint("gui_player_init begin");
#endif
    gui_player_init(screen_width, screen_height);
#ifndef HOST_BUILD
    boot_checkpoint("build_player_screen done");
#endif
    gui_lyrics_init();
#ifndef HOST_BUILD
    boot_checkpoint("library_load_from_cache_only begin");
#endif
    library_load_from_cache_only();
#ifndef HOST_BUILD
    boot_checkpoint("library_load_from_cache_only done");
#endif
    /* No whole-library load anywhere in this boot path, on purpose --
     * remote_control.c queries metadata_db.c directly (its own METADATA_DB_
     * GUARD) rather than needing a synced copy of the library, and each of
     * the five build_*_screen() calls above already activates its own
     * paged provider against the DB internally (see build_all_songs_
     * screen()'s own comment), so there's nothing left that would need a
     * whole-library array at boot for any reason, Remote Control enabled
     * or not. */
/* A-Z index & search registered in gui_library_init */


/* files_search and artist_albums initialized in gui_library_init */
    gui_text_input_init();
    gui_text_view_init();
    stream_media_screen = build_stream_media_screen();


    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */
    gui_subsonic_init();

    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */

#ifndef HOST_BUILD
    boot_checkpoint("gui_library_init begin");
#endif
    gui_library_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_library_init done");
#endif
    /* Warms the first ALBUM_THUMBNAIL_CACHE_SIZE album thumbnails into RAM
     * (and the rest onto disk) before the user ever opens Albums. */
    gui_library_start_boot_thumbnail_warmup();
#ifndef HOST_BUILD
    boot_checkpoint("gui_network_init begin");
#endif
    gui_network_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_network_init done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("gui_settings_init begin");
#endif
    gui_settings_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_settings_init done");
#endif
    gui_plugin_manage_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_plugin_store_init begin");
#endif
    gui_plugin_store_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_plugin_store_init done");
#endif
    gui_emulator_store_init();
    gui_books_init();
    build_power_off_countdown_popup();
    gui_queue_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_plugins_init begin");
#endif
    gui_plugins_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_plugins_init done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("gui_shell_init begin");
#endif
    gui_shell_init(screen_width, screen_height);
#ifndef HOST_BUILD
    boot_checkpoint("gui_shell_init done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("gui_navigation_init begin");
#endif
    gui_navigation_init();
#ifndef HOST_BUILD
    boot_checkpoint("gui_navigation_init done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("all screens built");
#endif



#ifndef HOST_BUILD
#if BOOT_SPLASH_MIN_DISPLAY_MS > 0
    /* Only a nonzero player splash configuration can delay startup. */
    while (boot_splash_start_tick != 0 &&
           lv_tick_get() - boot_splash_start_tick < BOOT_SPLASH_MIN_DISPLAY_MS) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50) wait_ms = 50;
        usleep(wait_ms * 1000);
    }
#endif
    boot_checkpoint("boot splash settle wait done");

    /* Ensure the status bar/popups/quick drawer are visible, including when
     * the optional splash did not hide them. */
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
#endif
    gui_library_boot_ready(lv_tick_get());

    /* gui_shell_get_home_screen() is the permanent root of the nav stack -- nav_pop() never
     * goes past it. Load it first so there's always something valid on
     * screen even before any auto-resume logic below runs. */


    /* Initialize user inactivity baseline at the splash-to-Home transition,
     * start runtime update timer, and install gesture indev hooks. */
    gui_reset_interactive_timeout_baseline();
    power_input_timer = lv_timer_create(power_button_timer_cb, 25, NULL);
    media_input_timer = lv_timer_create(media_button_timer_cb, 16, NULL);
    if (ui_wake_enabled()) {
        lv_timer_pause(power_input_timer);
        lv_timer_pause(media_input_timer);
    }
    lv_timer_create(update_timer_cb, 500, NULL);
    remote_track_stream_set_request_notify(ui_wake_notify);
    player_ui_timer_period_ms = backlight_screen_is_on() ? 50 : 500;
    player_ui_timer = lv_timer_create(player_ui_timer_cb, player_ui_timer_period_ms, NULL);
    lv_indev_t * gesture_indev = find_pointer_indev();
    gui_shell_install_indev_hooks(gesture_indev);
#ifndef HOST_BUILD
    boot_checkpoint("lv_screen_load(gui_shell_get_home_screen()) done");
#endif

    /* Auto-resume playback on startup:
     * If an SD card is mounted and contains a saved queue checkpoint, restore it
     * first. Otherwise fall back to the saved track in internal storage /
     * metadata database. Both paths use the same Car Mode / general resume policy.
     * Tracks in SUBSONIC_STREAM_CACHE_DIR are skipped to prevent resuming into
     * transient cache files without network connectivity. */
    bool setup_active = gui_setup_show_if_needed();
    bool sd_restored = false;
    if (!setup_active && sd_card_root_is_mounted()) {
        sd_restored = gui_player_restore_sd_queue(true);
    }

    int boot_resume_mode = gui_player_boot_resume_mode();
    if (!setup_active && !sd_restored && boot_resume_mode != 0 && current_settings.last_track[0] != '\0' &&
        strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
        char ** resume_playlist;
        int resume_count, resume_index;
        if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
            if (install_saved_resume_playlist(resume_playlist, resume_count)) {
                if (boot_resume_mode == 2) {
                    /* Do not open ALSA at all until the user presses Play.  On
                     * a headphone-less boot, start-then-pause could lose the
                     * race to an output-open failure and consume this queue. */
                    prepare_deferred_resume(resume_index, current_settings.last_position);
                } else play_track_at_from(resume_index, current_settings.last_position);
            }
        }
    }
#ifndef HOST_BUILD
    boot_checkpoint("gui_init returning");
#endif

#ifdef TEST_BOOT_RC
    /* The production startup path above deliberately resets session-only
     * modes and persists them as disabled. Re-enable only in memory for this
     * test build; never change the user's saved setting. The service startup
     * does blocking Bluetooth work on its own worker thread. */
    current_settings.remote_control_enabled = true;
    gui_network_start_test_boot_services();
#endif
}


void gui_deinit(void) {
#ifndef HOST_BUILD
    if (screen_wake_ram_staged)
        lv_linux_fbdev_end_ram_stage(lv_display_get_default());
#endif
    screen_wake_ram_staged = false;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), screen_wake_frame_ready_cb, NULL);
    screen_wake_pending = screen_wake_frame_pending = screen_wake_power_menu = screen_wake_flush_seen = screen_wake_reveal_pending = false;
    screen_wake_unlock_requested = false;
    screen_wake_scanout_pending = false;
    screen_wake_notified = false;
    if (screen_wake_retained_fast)
        lv_display_enable_invalidation(lv_display_get_default(), true);
    screen_wake_retained_fast = false;
    last_presented_screen = NULL;
    screen_wake_retained_screen = NULL;
    gui_lock_screen_set_wake_unlock_override(false);
    if (player_ui_timer) {
        lv_timer_delete(player_ui_timer);
        player_ui_timer = NULL;
        player_ui_timer_period_ms = 0;
    }
    button_mapping_reset(&physical_button_mapping);
    gui_setup_teardown();
    gui_library_cancel_scan();
    gui_library_cancel_background_work();
    gui_subsonic_cancel_background_work();
    gui_network_cancel_background_work();
    gui_lyrics_cancel_background_work();
    gui_player_cancel_background_work();
    gui_shell_cancel_background_work();
}
