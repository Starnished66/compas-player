#include "gui.h"
#include "../../core/system/runtime/app_clock.h"
#include "../text/i18n/i18n.h"
#include "../settings/setup/gui_setup.h"
#include "../appearance/gui_themes.h"
#include "../settings/gui_settings.h"
#include "../network/gui_network.h"
#include "../library/collections/gui_books.h"
#include "../text/rendering/gui_text_input.h"
#include "../text/rendering/gui_text_view.h"
#include "../player/screen/gui_lyrics.h"
#include "../player/transport/player_seek_input.h"
#include "../network/subsonic/gui_subsonic.h"
#include "../assets/assets.h"
#include "../../hardware/indicators/backlight.h"
#include "../../audio/output/audio_output.h"
#include "../../hardware/input/hw_buttons.h"
#include "../widgets/builders/screen_builders.h"
#include "../../core/persistence/bt_ssc_settings.h"
#include "../../core/persistence/bt_rc_trust_settings.h"
#include "../../network/bluetooth/control/bt_link_key_watch.h"
#include "../../core/system/logging/db_log.h"
#include "../effects/transition_compositor.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../../hardware/device/headphone_status.h"
#include "../plugins/manage/gui_plugin_manage.h"
#include "../plugins/manage/gui_plugin_store.h"
#include "../plugins/emulators/gui_emulator_store.h"
#include "../../hardware/indicators/led_control.h"
#include "../../hardware/power/charge_limiter.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include "../../core/time/timezone_apply.h"
#include "../../core/device/hostname_apply.h"
#include <string.h>
#include <unistd.h>
#include "../settings/controls/gui_remote_settings.h"
#include "gui_internal.h"

#ifdef HOST_BUILD
#define GUI_BOOT_CHECKPOINT(step) ((void) 0)
#else
#define GUI_BOOT_CHECKPOINT(step) boot_checkpoint(step)
#endif

lv_obj_t * stream_media_screen;
bool favorite_is_set = false;

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
    gui_screen_power_reset_idle_state();
    hw_button_event_t event;
    while (hw_buttons_consume_event(&event)) {}
    (void) hw_buttons_consume_screenshot();
    /* Rebuilds the snapshots gui_release_memory_for_external_session() freed
     * (Home's frame in a timer pass, the back and Now Playing frames asynchronously), and
     * the drawer's. */
    if (external_session_released_memory) {
        external_session_released_memory = false;
        gui_navigation_invalidate_theme_snapshots();
        quick_drawer_mark_snapshot_dirty();
    }
    full_redraw_async_cb(NULL);
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
}

const char * gui_plugin_get_current_track_path(void) {
    return gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
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

/* Loads the saved settings and applies them to the subsystems that read them at startup. */
static void apply_boot_settings(void) {
    GUI_BOOT_CHECKPOINT("gui_init entered");
    GUI_BOOT_CHECKPOINT("settings_load begin");
    settings_load(&current_settings);
#ifndef HOST_BUILD
    audio_output_set_usb_dsd_mode(current_settings.usb_dsd_output);
    audio_output_set_dsd_gain_db(current_settings.dsd_gain_db);
    audio_output_set_dac_preferences(current_settings.dac_filter, current_settings.dac_nos,
                                     current_settings.dac_dre);
#endif
    GUI_BOOT_CHECKPOINT("settings_load done; settings application begin");
    i18n_set_language(current_settings.language);
    gui_display_apply_rotation(current_settings.screen_upside_down);
    bt_control_restore_codec_preference(current_settings.bt_codec);
    bt_ssc_settings_apply(&current_settings);
    bt_rc_trust_settings_apply(&current_settings);
    bt_link_key_watch_start();
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    bt_control_set_sample_rate(current_settings.bt_sample_rate);
    db_log_set_enabled(current_settings.db_logging_enabled);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
    hw_buttons_set_volume_lock_screen_off(current_settings.volume_lock_screen_off);
    player_seek_input_set_hw_step_seconds(current_settings.hw_seek_step_seconds);
    gui_buttons_init();
    gui_apply_button_bindings();
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
    bt_control_set_debug_log_enabled(current_settings.db_logging_enabled);
    headphone_status_refresh_earpods_adc();
    app_clock_init(current_settings.clock_automatic, current_settings.clock_manual_epoch,
                   current_settings.clock_system_reference);
    GUI_BOOT_CHECKPOINT("settings application done");
}

/* Starts the theme and notification services and applies the saved hostname. */
static void init_theme_and_hostname(void) {
    gui_theme_init();
    ui_marquee_init();
    gui_notifications_init();
    GUI_BOOT_CHECKPOINT("hostname_apply begin");
    /* Must run before anything could turn Wi-Fi/Bluetooth on and trigger
     * wifi_on.sh/bt_init's own one-time read of the file this bind-mounts
     * over -- see hostname_apply()'s own comment. */
    hostname_apply(current_settings.hostname);
    GUI_BOOT_CHECKPOINT("hostname_apply done");
}

/* Corrects the persisted USB mode against the live gadget state. */
static void reconcile_usb_mode_at_boot(void) {
    /* Correct the persisted USB mode against live gadget state before
     * anything else reads it (external_dac_block_reason() in particular --
     * a stale usb_mode==USB_MODE_DAC left over from a previous run would
     * otherwise wrongly block local playback from the moment the app
     * starts). Hardware USB gadget state doesn't survive a reboot, but this
     * binary can also be killed and relaunched without a reboot (as it was
     * repeatedly during development), which is exactly when the persisted
     * value and reality can disagree. */
    usb_mode_t detected_usb_mode;
    GUI_BOOT_CHECKPOINT("USB mode discovery begin");
    bool detected = usb_mode_control_detect_current(&detected_usb_mode);
    GUI_BOOT_CHECKPOINT("USB mode discovery done");
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
}

/* Applies the saved volume, brightness, LED, charging and timezone settings. */
static void apply_boot_audio_and_power(void) {
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

    GUI_BOOT_CHECKPOINT("LED charging timezone setup begin");
    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    if (current_settings.timezone[0] != '\0') timezone_apply(current_settings.timezone);
    GUI_BOOT_CHECKPOINT("LED charging timezone setup done");
}

/* Clears the receiver and server modes that never survive a restart. */
static void reset_session_only_network_modes(void) {
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
    gui_remote_settings_init(); /* after the session-only fields above are final */
}

/* Loads the native themes and plugins, then applies the active theme. */
static void init_plugins_and_themes(void) {
    /* Initialize native theme service and legacy migration before plugin_manager_init */
    GUI_BOOT_CHECKPOINT("gui_themes_init begin");
    gui_themes_init();
    GUI_BOOT_CHECKPOINT("gui_themes_init done");

    /* Discovers plugin rows/tiles by loading and running every .lua file
     * under <SD card>/.plugins/ -- run early, well before Books, Settings,
     * or Stream Media (the current plugin entry points, see build_books_
     * screen()/build_settings_screen()/build_stream_media_screen()) could
     * plausibly be reached. */
    GUI_BOOT_CHECKPOINT("plugin_manager_init begin");
    plugin_manager_init();
    GUI_BOOT_CHECKPOINT("plugin_manager_init done");

    /* Apply active native theme on top of fresh plugin baseline before screens are built */
    GUI_BOOT_CHECKPOINT("gui_themes_apply_active begin");
    gui_themes_apply_active();
    GUI_BOOT_CHECKPOINT("gui_themes_apply_active done");
    GUI_BOOT_CHECKPOINT("pre-screen-build setup done");
}

/* Builds every screen and the shell before the first frame. */
static void build_startup_core_screens(uint32_t screen_width, uint32_t screen_height) {
    GUI_BOOT_CHECKPOINT("gui_player_init begin");
    gui_player_init(screen_width, screen_height);
    GUI_BOOT_CHECKPOINT("build_player_screen done");
    gui_lyrics_init();
    GUI_BOOT_CHECKPOINT("library_load_from_cache_only begin");
    library_load_from_cache_only();
    GUI_BOOT_CHECKPOINT("library_load_from_cache_only done");
    gui_text_input_init();
    gui_text_view_init();
    stream_media_screen = build_stream_media_screen();

    /* Artists/Albums are built empty and populated once a fetch completes. */
    gui_subsonic_init();

    GUI_BOOT_CHECKPOINT("gui_library_init begin");
    gui_library_init();
    GUI_BOOT_CHECKPOINT("gui_library_init done");
    /* Warms the first ALBUM_THUMBNAIL_CACHE_SIZE album thumbnails into RAM
     * (and the rest onto disk) before the user ever opens Albums. */
    gui_library_start_boot_thumbnail_warmup();
}

static void build_startup_network_settings_screens(void) {
    GUI_BOOT_CHECKPOINT("gui_network_init begin");
    gui_network_init();
    GUI_BOOT_CHECKPOINT("gui_network_init done");
    GUI_BOOT_CHECKPOINT("gui_settings_init begin");
    gui_settings_init();
    GUI_BOOT_CHECKPOINT("gui_settings_init done");
    gui_plugin_manage_init();
    GUI_BOOT_CHECKPOINT("gui_plugin_store_init begin");
    gui_plugin_store_init();
    GUI_BOOT_CHECKPOINT("gui_plugin_store_init done");
    gui_emulator_store_init();
    gui_books_init();
    build_power_off_countdown_popup();
    gui_queue_init();
}

static void build_startup_shell(uint32_t screen_width, uint32_t screen_height) {
    GUI_BOOT_CHECKPOINT("gui_plugins_init begin");
    gui_plugins_init();
    GUI_BOOT_CHECKPOINT("gui_plugins_init done");
    GUI_BOOT_CHECKPOINT("gui_shell_init begin");
    gui_shell_init(screen_width, screen_height);
    GUI_BOOT_CHECKPOINT("gui_shell_init done");
    GUI_BOOT_CHECKPOINT("gui_navigation_init begin");
    gui_navigation_init();
    GUI_BOOT_CHECKPOINT("gui_navigation_init done");
}

static void build_startup_screens(uint32_t screen_width, uint32_t screen_height) {
    build_startup_core_screens(screen_width, screen_height);
    build_startup_network_settings_screens();
    build_startup_shell(screen_width, screen_height);
    GUI_BOOT_CHECKPOINT("all screens built");
}

/* Starts the input and update timers and installs the gesture indev hooks. */
static void start_runtime_timers(void) {
    /* Initialize user inactivity baseline at the splash-to-Home transition,
     * start runtime update timer, and install gesture indev hooks. */
    gui_reset_interactive_timeout_baseline();
    gui_screen_power_start_timers();
    gui_control_poll_start_timers();
    lv_indev_t * gesture_indev = find_pointer_indev();
    gui_shell_install_indev_hooks(gesture_indev);
    GUI_BOOT_CHECKPOINT("lv_screen_load(gui_shell_get_home_screen()) done");
}

/* Restores the saved queue or last track, as the resume mode asks. */
static void auto_resume_playback_at_boot(void) {
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
}

void gui_init(uint32_t screen_width, uint32_t screen_height) {
    gui_screen_power_init();
    apply_boot_settings();
    init_theme_and_hostname();
    reconcile_usb_mode_at_boot();
    apply_boot_audio_and_power();
    reset_session_only_network_modes();
    init_plugins_and_themes();
    build_startup_screens(screen_width, screen_height);

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

    start_runtime_timers();
    auto_resume_playback_at_boot();
    GUI_BOOT_CHECKPOINT("gui_init returning");
}

void gui_deinit(void) {
    settings_save_drain();
    gui_screen_power_deinit();
    gui_control_poll_deinit();
    gui_setup_teardown();
    gui_library_cancel_scan();
    gui_library_cancel_background_work();
    gui_subsonic_cancel_background_work();
    gui_network_cancel_background_work();
    gui_lyrics_cancel_background_work();
    gui_player_cancel_background_work();
    gui_shell_cancel_background_work();
}

