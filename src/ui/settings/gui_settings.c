#include "setup/gui_setup.h"
#include "../application/gui.h"
#include "gui_settings.h"
#include "input/gui_button_mapping.h"
#include "../player/settings/gui_equalizer.h"
#include "../plugins/manage/gui_plugin_order_editor.h"
#include "../home/gui_home.h"
#include "../updates/gui_updates.h"
#include "../appearance/gui_accent.h"
#include "../appearance/fonts/gui_custom_font.h"
#include "display/gui_display_timing_settings.h"
#include "controls/gui_time_settings.h"
#include "controls/gui_shell_settings.h"
#include "controls/gui_volume_settings.h"
#include "../text/i18n/i18n.h"
#include "../../hardware/input/hw_buttons.h"
#include "../library/browse/gui_library.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../shell/gui_shell.h"
#include "../text/rendering/gui_text_input.h"
#include "../../core/system/runtime/app_version.h"
#include <math.h>
#include "../network/subsonic/gui_subsonic.h"
#include "../library/collections/gui_books.h"
#include "../network/gui_network.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include "../player/screen/gui_lyrics.h"
#include "../widgets/builders/screen_builders.h"
#include "../../core/persistence/settings.h"
#include "../../library/tagcache/core/tagcache.h"
#include "../../library/metadata/db/metadata_db.h"
#include "../assets/assets.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../audio/engine/audio.h"
#include "../../audio/output/audio_output.h"
#include "../../hardware/device/device_config.h"
#include "../../hardware/usb/usb_mode_control.h"
#include "../../core/updates/firmware_update.h"
#include "../../plugins/manager/plugin_manager.h"
#include "../plugins/manage/gui_plugin_manage.h"
#include "../plugins/manage/gui_plugin_store.h"
#include "../appearance/gui_themes.h"
#include "../text/rendering/fallback_font.h"
#include "../shell/gui_navigation.h"
#include "../player/screen/gui_player.h"
#include "../shell/gui_reload.h"
#include "../player/layout/player_layouts.h"
#include "../../core/system/logging/db_log.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../hardware/indicators/backlight.h"
#include "../library/browse/gui_file_manager.h"
#include "../plugins/emulators/gui_emulators.h"
#include <stdatomic.h>
#include <pthread.h>

/* Extern references to screen pointers owned by this module (defined here) */
static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_gapless_toggle_img = NULL;
static lv_obj_t * settings_screen;
static lv_obj_t * settings_library_screen;
static lv_obj_t * settings_sorting_screen;
enum { SORTING_FILE_OPTION_COUNT = 2, SORTING_ALBUM_CONTEXT_COUNT = 3,
       SORTING_ALBUM_MODE_COUNT = 3,
       SORTING_GLOBAL_ALBUM_ARTIST_OPTION = SORTING_FILE_OPTION_COUNT +
           SORTING_ALBUM_CONTEXT_COUNT * SORTING_ALBUM_MODE_COUNT,
       SORTING_OPTION_COUNT = SORTING_FILE_OPTION_COUNT +
           SORTING_ALBUM_CONTEXT_COUNT * SORTING_ALBUM_MODE_COUNT + 1 };
static lv_obj_t * settings_sorting_options[SORTING_OPTION_COUNT];
static lv_obj_t * settings_sound_effects_screen;
static lv_obj_t * dac_settings_screen;
static lv_obj_t * dac_settings_list;
static lv_obj_t * usb_dsd_settings_screen;
static lv_obj_t * usb_dsd_settings_list;
static lv_obj_t * settings_eq_profiles_menu_screen;
static lv_obj_t * settings_appearance_screen;
static lv_obj_t * settings_player_layout_screen;
static lv_obj_t * settings_gestures_screen;
static lv_obj_t * settings_charging_screen;
static lv_obj_t * settings_usb_dac_charging_switch;
static lv_obj_t * settings_usb_dac_charging_row;
#if BOARD_R1_HARDWARE
static void usb_dac_charging_switch_event_cb(lv_event_t * e);
#endif
static lv_obj_t * settings_maintenance_screen;
static lv_obj_t * settings_system_maintenance_screen;
static lv_obj_t * settings_system_updates_screen;
static lv_obj_t * settings_tools_screen;
static lv_obj_t * music_playback_screen;
static lv_obj_t * music_audio_screen;
static lv_obj_t * music_controls_screen;
static lv_obj_t * settings_display_screen;
static lv_obj_t * animation_speed_screen;
static lv_obj_t * animation_speed_list;
static lv_obj_t * keyboard_layout_screen;
static lv_obj_t * keyboard_layout_list;
static lv_obj_t * player_layout_choice_screen;
static lv_obj_t * player_layout_choice_list;
static lv_obj_t * language_choice_screen;
static lv_obj_t * language_choice_list;
static lv_obj_t * settings_power_screen;
static lv_obj_t * settings_system_screen;
static lv_obj_t * about_screen;
static lv_obj_t * creator_help_menu_screen;
static lv_obj_t * creator_plugin_help_screen;
static lv_obj_t * creator_theme_help_screen;
static lv_obj_t * buy_me_a_coffee_screen;
static lv_obj_t * dev_options_screen;
static lv_obj_t * sleep_timer_screen;
static lv_obj_t * idle_shutdown_screen;
static lv_obj_t * settings_eq_summary;
static lv_obj_t * settings_car_summary;
static lv_obj_t * settings_sleep_summary;

static void settings_summary_loaded_cb(lv_event_t * e) {
    (void) e;
    if (settings_eq_summary) {
        gui_equalizer_update_summary(settings_eq_summary);
    }
    if (settings_car_summary)
        lv_label_set_text(settings_car_summary, current_settings.car_mode_enabled ? TR("On") : TR("Off"));
    if (settings_sleep_summary) {
        int seconds = quick_drawer_sleep_timer_remaining_seconds();
        if (quick_drawer_sleep_timer_is_active())
            lv_label_set_text_fmt(settings_sleep_summary, TR("%d min remaining"), (seconds + 59) / 60);
        else lv_label_set_text(settings_sleep_summary, TR("Off"));
    }
    gui_button_mapping_refresh_summaries();
}

static lv_obj_t * build_library_category_screen(void);
static lv_obj_t * build_music_controls_screen(void);

/* Externs to gui.c functions and state this module needs */
extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * stream_media_screen;
extern lv_obj_t * gui_network_get_wireless_screen();
extern lv_obj_t * gui_network_get_usb_dac_overlay();
extern lv_obj_t * gui_shell_get_dac_home_screen();
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);
extern void generic_back_cb(lv_event_t * e);
extern void start_library_rescan(void);
extern void show_library_refresh_all_metadata_prompt(void);
extern void show_library_refresh_all_covers_prompt(void);

static lv_obj_t * sleep_timer_switch;
static lv_obj_t * sleep_timer_slider_card;
static lv_obj_t * sleep_timer_remaining_btn;
static lv_obj_t * sleep_timer_value_label;
static lv_obj_t * sleep_timer_slider;
static lv_obj_t * idle_shutdown_switch;
static lv_obj_t * idle_shutdown_slider_card;
static lv_obj_t * idle_shutdown_value_label;
static lv_obj_t * idle_shutdown_slider;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;
static lv_obj_t * idle_action_poweroff_check;
static lv_obj_t * idle_action_suspend_check;


static lv_obj_t * adb_switch = NULL;

/* The Developer Options screen is built once and cached, so its ADB row would
 * otherwise keep showing whatever the state was at build time. Called when the
 * screen opens, and again by poll_usb_mode_switch() once the gadget settles so
 * a switch that failed does not leave the toggle promising ADB. */
void gui_settings_sync_adb_toggle(void) {
    if (!adb_switch) return;
    if (gui_network_adb_active()) lv_obj_add_state(adb_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(adb_switch, LV_STATE_CHECKED);
}

static void adb_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Turning ADB off falls back to Storage: the stock default, and the safer
     * of the other two since DAC takes over the whole screen with its own
     * overlay rather than just quietly changing what the USB port does. */
    bool want_adb = !gui_network_adb_active();
    if (!start_usb_mode_switch(want_adb ? USB_MODE_ADB : USB_MODE_STORAGE)) {
        /* Request dropped because a switch is already in flight. Put the
         * switch back rather than leaving it showing a state nothing is
         * working toward. */
        gui_settings_sync_adb_toggle();
        return;
    }
    /* Accepted: the switch already shows the requested state, and applying it
     * takes seconds. Leave it optimistically flipped; poll_usb_mode_switch()
     * confirms or rolls it back. */
}

static void dev_options_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_sync_adb_toggle();
    nav_push(dev_options_screen);
}

static const char PAYPAL_DONATION_URL[] =
    "https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=josegarita%40protonmail.com&currency_code=USD";

static void buy_me_a_coffee_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(buy_me_a_coffee_screen);
}

static void creator_plugin_help_row_cb(lv_event_t * e);
static void creator_theme_help_row_cb(lv_event_t * e);

static void creator_help_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!creator_help_menu_screen) {
        static pill_list_item_t items[2];
        items[0] = (pill_list_item_t){ TR("Create a plugin"), PILL_ACCESSORY_CHEVRON, false,
                                       creator_plugin_help_row_cb, NULL, NULL };
        items[1] = (pill_list_item_t){ TR("Create a theme"), PILL_ACCESSORY_CHEVRON, false,
                                       creator_theme_help_row_cb, NULL, NULL };
        creator_help_menu_screen = build_pill_list_screen(TR("Creator guides"), generic_back_cb,
                                                          items, 2, gui_theme_accent_style(), GUI_ROW_GAP, 100);
        finalize_screen_navigation(creator_help_menu_screen);
    }
    nav_push(creator_help_menu_screen);
}

static lv_obj_t * build_buy_me_a_coffee_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Buy Me a Coffee"), &title_label, &list);

    lv_obj_t * qrcode = lv_qrcode_create(list);
    lv_qrcode_set_size(qrcode, BOARD_SCALE_PX(320));
    lv_qrcode_set_dark_color(qrcode, lv_color_black());
    lv_qrcode_set_light_color(qrcode, lv_color_white());
    lv_qrcode_set_quiet_zone(qrcode, true);
    lv_obj_set_style_border_width(qrcode, 4, 0);
    lv_obj_set_style_border_color(qrcode, lv_color_white(), 0);
    lv_qrcode_update(qrcode, PAYPAL_DONATION_URL, strlen(PAYPAL_DONATION_URL));

    lv_obj_t * caption = lv_label_create(list);
    lv_label_set_text(caption, TR("Scan with your phone to support Compás Player on PayPal"));
    lv_obj_set_width(caption, lv_pct(90));
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(caption, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_pad_top(caption, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(caption, BOARD_SCALE_PX(20), 0);

    /* The QR and its caption fit within both 800px and 720px panels. Center
     * the pair as a group so the shorter R3 Pro II screen keeps equal space
     * above and below it without requiring a scroll. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return scr;
}

static void creator_help_add_text(lv_obj_t * parent, const char * text, bool muted) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(label, muted ? &style_theme_text_muted : &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
}

static lv_obj_t * build_creator_help_screen(const char * title, const char * url,
                                             const char * instructions, const char * extra) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(title, &title_label, &list);
    lv_obj_set_style_pad_row(list, BOARD_SCALE_PX(12), 0);
    creator_help_add_text(list, instructions, false);
    if (extra) creator_help_add_text(list, extra, false);

    /* A square white quiet zone keeps the QR readable on every theme. The
     * list remains scrollable when the 720px panel cannot fit all guidance. */
    lv_obj_t * qr = lv_qrcode_create(list);
    lv_qrcode_set_size(qr, BOARD_SCALE_PX(240));
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_qrcode_update(qr, url, strlen(url));
    creator_help_add_text(list, TR("Scan for the full guide"), true);
    creator_help_add_text(list, url, true);
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(24), 0);
    enable_gesture_bubble_recursive(scr);
    return scr;
}

static void creator_plugin_help_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!creator_plugin_help_screen) {
        creator_plugin_help_screen = build_creator_help_screen(
            TR("Create a plugin"),
            "https://github.com/Starnished66/compas-page/blob/f410df626a18a17da3c0333e562e5afc81569aed/docs/CREATING.md#plugins",
            TR("Write a Lua plugin on your computer. Copy its .lua file to the .plugins folder on your SD card."),
            TR("Open More > Plugins > Refresh Plugins. Scan for a working example and the full API guide."));
    }
    nav_push(creator_plugin_help_screen);
}

static void creator_theme_help_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!creator_theme_help_screen) {
        creator_theme_help_screen = build_creator_help_screen(
            TR("Create a theme"),
            "https://github.com/Starnished66/compas-page/blob/f410df626a18a17da3c0333e562e5afc81569aed/docs/CREATING.md#themes",
            TR("Edit a .theme text file on your computer. Copy it to the Themes folder on your SD card, then select it in More > Themes."),
            TR("Scan for a starter theme and instructions. To redesign the player screen, see the XML layout guide linked there."));
    }
    nav_push(creator_theme_help_screen);
}

static lv_obj_t * build_about_screen(void) {
    static pill_list_item_t items[4];
    lv_obj_t * version_row = NULL;
    items[0] = (pill_list_item_t){ TR("Compás Player"), PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[0].out_row = &version_row;
    items[1] = (pill_list_item_t){ TR("Buy Me a Coffee"), PILL_ACCESSORY_CHEVRON, false,
                                    buy_me_a_coffee_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Create plugins & themes"), PILL_ACCESSORY_CHEVRON, false,
                                    creator_help_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Developer Options"), PILL_ACCESSORY_CHEVRON, false,
                                    dev_options_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("About"), generic_back_cb, items, 4, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    lv_label_set_text(add_pill_summary_label(version_row), app_version_label());
    finalize_screen_navigation(scr);
    return scr;
}

static void db_logging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.db_logging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    db_log_set_enabled(current_settings.db_logging_enabled);
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
}

/* Writes a detailed timestamped log of library database scans and album art
 * cache jobs to .logs/database_artwork.log, and USB DAC bridge diagnostics to
 * .logs/usb_dac_bridge.log on the SD card -- see db_log.h and usb_dac_bridge.h. */
static void screenshot_combo_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screenshot_combo_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
}

static void reboot_to_boot_selector_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char error[128] = {0};
    if (!idle_shutdown_reboot_to_bootloader(error, sizeof(error)))
        show_error_toast(error[0] ? error : TR("Could not restart into the boot selector"));
}

#if defined(BOARD_R3II_2025)
  #define SCREENSHOT_ROW_LABEL TR("Screenshots (Power + Previous)")
#else
  #define SCREENSHOT_ROW_LABEL TR("Screenshots (Power + Vol Down)")
#endif
static lv_obj_t * build_dev_options_screen(void) {
    static pill_list_item_t items[4];
    /* ADB lives here rather than on the USB Mode screen: it overrides
     * Storage/DAC while on and persists across a reboot, so it sits behind
     * Developer Options as the explicit opt-in that makes re-applying it on
     * startup reasonable. The USB Mode screen still dims the other modes
     * while it owns the port. */
    items[0] = (pill_list_item_t){ TR("ADB"), PILL_ACCESSORY_TOGGLE, gui_network_adb_active(),
                                    NULL, adb_switch_event_cb, NULL, &adb_switch };
    items[1] = (pill_list_item_t){ TR("Enable debug logging"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.db_logging_enabled, NULL, db_logging_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ SCREENSHOT_ROW_LABEL, PILL_ACCESSORY_TOGGLE,
                                    current_settings.screenshot_combo_enabled, NULL,
                                    screenshot_combo_switch_event_cb, NULL };
    items[3] = (pill_list_item_t){ TR("Reboot to bootloader"), PILL_ACCESSORY_NONE, false,
                                    reboot_to_boot_selector_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("Developer Options"), generic_back_cb, items, 4, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Called by gui_theme.c after every accent change, including a new cover
 * while "Match album art" is on. */
void gui_settings_accent_changed(void) {
    gui_accent_sync();
    gui_equalizer_accent_changed();
}

void gui_settings_open_sleep_timer(void) {
    if (!sleep_timer_screen) return;
    gui_settings_sync_sleep_timer_toggle();
    nav_push(sleep_timer_screen);
}

void gui_settings_open_playback(void) {
    if (!music_playback_screen) return;
    nav_push(music_playback_screen);
}

void gui_settings_open_sound(void) {
    if (!music_audio_screen) return;
    nav_push(music_audio_screen);
}

/* Index into SLEEP_TIMER_STEPS closest to `minutes' -- same reasoning as
 * screen_timeout_seconds_to_step_index() above. */
static int sleep_timer_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT, minutes);
}

/* Extracted from build_sleep_timer_screen() so it can be shared with its
 * own format_duration(), needed now that SLEEP_TIMER_STEPS reaches 180
 * (settings.c) and a bare "%d min" would read as "180 min" instead of the
 * much more readable "3 hr". */
static void format_sleep_timer_duration(char * buf, size_t buf_size, int minutes) {
    int hours = minutes / 60;
    int remainder = minutes % 60;
    if (hours == 0) snprintf(buf, buf_size, TR("%d min"), remainder);
    else if (remainder == 0) snprintf(buf, buf_size, TR("%d hr"), hours);
    else snprintf(buf, buf_size, TR("%d hr %d min"), hours, remainder);
}

/* Arms or disarms the sleep timer directly, synchronizing state with the
 * quick drawer sleep timer toggle. */
static void sleep_timer_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    quick_drawer_sleep_timer_set_active(active);
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* This only sets the duration the quick-drawer sleep icon/enable toggle
 * above arms next time (or restarts the CURRENT countdown at, if already
 * armed -- same real-time-restart behavior ExtendedSleepTimer.lua's own
 * duration slider has, matching what a user dragging this while counting
 * down would expect: the countdown they're looking at should reflect the
 * duration they just picked, not the one that was armed before). */
static void sleep_timer_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = SLEEP_TIMER_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.sleep_timer_minutes = minutes;
        char duration_buf[32];
        format_sleep_timer_duration(duration_buf, sizeof(duration_buf), minutes);
        lv_label_set_text(sleep_timer_value_label, duration_buf);
        if (quick_drawer_sleep_timer_is_active()) quick_drawer_sleep_timer_set_active(true);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* "Show Time Remaining" button -- same info ExtendedSleepTimer.lua's own
 * "Show Time Remaining" settings row surfaces via a toast, in the same
 * H:MM:SS (or M:SS under an hour) shape as that plugin's format_remaining().
 * Hidden whenever the timer is disarmed (sleep_timer_switch_event_cb /
 * gui_settings_sync_sleep_timer_toggle), so this is only ever reachable
 * while armed -- no "timer is off" fallback message needed here. */
static void sleep_timer_show_remaining_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int total_seconds = quick_drawer_sleep_timer_remaining_seconds();
    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int secs = total_seconds % 60;
    char buf[48];
    if (hours > 0) snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d:%02d"), hours, minutes, secs);
    else snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d"), minutes, secs);
    show_info_toast(buf);
}

/* Mirrors build_screen_timeout_screen()'s layout (enable row + slider
 * card), plus one more row: a "Show Time Remaining" button neither that
 * screen nor this one needed before ExtendedSleepTimer.lua's own version
 * of this same feature demonstrated why one's genuinely useful here (the
 * status bar has nowhere to show a running countdown, unlike the quick
 * drawer's own quick_drawer_sleep_label). */
static lv_obj_t * build_sleep_timer_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Sleep Timer"), generic_back_cb, NULL, NULL);

    /* Same flex-row/SPACE_BETWEEN/flex_grow-label shape as build_screen_
     * timeout_screen()'s own enable_row -- see its comment for the real-
     * device overlap bug this avoids at bigger system text sizes. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Enable Sleep Timer"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    sleep_timer_switch = lv_switch_create(enable_row);
    lv_obj_add_style(sleep_timer_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (quick_drawer_sleep_timer_is_active()) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sleep_timer_switch, sleep_timer_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sleep_timer_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SLEEP_TIMER_STEP_COUNT - 1,
        sleep_timer_minutes_to_step_index(current_settings.sleep_timer_minutes),
        sleep_timer_slider_event_cb, &sleep_timer_slider, &sleep_timer_value_label);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
    char duration_buf[32];
    format_sleep_timer_duration(duration_buf, sizeof(duration_buf), current_settings.sleep_timer_minutes);
    lv_label_set_text(sleep_timer_value_label, duration_buf);

    sleep_timer_remaining_btn = lv_obj_create(scr);
    lv_obj_set_size(sleep_timer_remaining_btn, lv_pct(90), BOARD_SCALE_PX(70));
    lv_obj_align_to(sleep_timer_remaining_btn, sleep_timer_slider_card, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(sleep_timer_remaining_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(sleep_timer_remaining_btn, 0, 0);
    lv_obj_set_style_radius(sleep_timer_remaining_btn, 10, 0);
    lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_timer_remaining_btn, sleep_timer_show_remaining_cb, LV_EVENT_CLICKED, NULL);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * remaining_label = lv_label_create(sleep_timer_remaining_btn);
    lv_label_set_text(remaining_label, TR("Show Time Remaining"));
    lv_obj_add_style(remaining_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remaining_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(remaining_label);

    finalize_screen_navigation(scr);
    return scr;
}

/* All IDLE_SHUTDOWN_STEPS values are whole minutes, so unlike
 * format_screen_timeout() this never needs a seconds remainder -- "10m" /
 * "1h" / "2h", not "1h 30m", since 90 isn't one of the steps. */
static void format_idle_shutdown(char * buf, size_t buf_size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, buf_size, "%dm", minutes);
        return;
    }
    snprintf(buf, buf_size, "%dh", minutes / 60);
}

static int idle_shutdown_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT, minutes);
}

static void idle_shutdown_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.idle_shutdown_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.idle_shutdown_enabled) {
        lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Mutually-exclusive idle-action choice (Power Off vs Suspend to RAM) --
 * replaces the old single "Suspend instead of power off" switch, which
 * modeled this same 2-way choice as a toggle acting on an implicit
 * opposite (plain poweroff being "off"). Same accent-border-highlight
 * pill-row selection language as populate_usb_mode_screen()'s own Storage/
 * USB DAC rows (add_pill_row_base()), but inline on this same screen
 * rather than a separate list screen, since it stays right above the
 * slider it's a sibling setting of. */
static void idle_action_choice_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool suspend = (bool) (intptr_t) lv_event_get_user_data(e);
    current_settings.idle_suspend_enabled = suspend;
    settings_save(&current_settings);
    lv_obj_set_style_border_width(idle_action_poweroff_row, suspend ? 0 : 3, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, suspend ? 3 : 0, 0);
    if (suspend) {
        lv_obj_add_flag(idle_action_poweroff_check, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_action_suspend_check, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(idle_action_poweroff_check, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_action_suspend_check, LV_OBJ_FLAG_HIDDEN);
    }
}

static void idle_shutdown_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = IDLE_SHUTDOWN_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.idle_shutdown_minutes = minutes;
        char buf[32];
        format_idle_shutdown(buf, sizeof(buf), minutes);
        lv_label_set_text(idle_shutdown_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_idle_shutdown_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Idle Shutdown"), generic_back_cb, NULL, NULL);

    lv_obj_t * content = scr;
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20;
#if defined(BOARD_TEMPOTEC_V1)
    /* Keep the fixed header and home-indicator areas outside the scrolling
     * region while allowing both idle-action rows and the timeout slider to
     * remain reachable on the short V1 display. */
    content = lv_obj_create(scr);
    int32_t content_y = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100),
                    BOARD_SCREEN_HEIGHT - content_y - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_pos(content, 0, content_y);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    content_top = 20;
#endif

    /* Uses content-based sizing so wrapped label text does not overlap. */
    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Automatically go idle"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    idle_shutdown_switch = lv_switch_create(enable_row);
    lv_obj_add_style(idle_shutdown_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.idle_shutdown_enabled) lv_obj_add_state(idle_shutdown_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(idle_shutdown_switch, idle_shutdown_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Mutually-exclusive idle-action choice (Power Off or Suspend to RAM).
     * Shown/hidden together with the slider card in
     * idle_shutdown_switch_event_cb(). */
    /* Section height and the two pill rows' Y offsets below were all
     * hardcoded (292 / 34 / 34+124+10) at some medium-tier row height --
     * add_pill_row_base()'s own row height already scales with
     * GUI_FONT_ROLE_BODY's line height, but these hand-rolled offsets
     * didn't, so at the largest ("BlindMF") font tier the two rows grew
     * taller than the fixed gaps between them and overflowed the fixed
     * section height into the slider card below (GitHub issue #91).
     * Derived from the same font metrics add_pill_row_base() itself uses,
     * so this stays correct at every tier without needing another manual
     * retune. */
    int32_t idle_action_explain_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    int32_t idle_action_row_h = ui_list_row_height();
    int32_t idle_action_row1_y = idle_action_explain_h + 14;
    int32_t idle_action_row_gap = 10;
    int32_t idle_action_row2_y = idle_action_row1_y + idle_action_row_h + idle_action_row_gap;
    int32_t idle_action_section_h = idle_action_row2_y + idle_action_row_h;

    /* 100% width accommodates display-width pill rows without clipping.
     * pad_top and pad_bottom are zeroed so the child rows and label fit within
     * the section height (now font-tier-derived above) without vertical overflow. */
    idle_action_section = lv_obj_create(content);
    lv_obj_set_size(idle_action_section, lv_pct(100), idle_action_section_h);
    /* Positioned relative to enable_row's bottom edge so wrapped label text
     * does not cause overlapping. */
    lv_obj_align_to(idle_action_section, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_top(idle_action_section, 0, 0);
    lv_obj_set_style_pad_bottom(idle_action_section, 0, 0);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_action_explain_label = lv_label_create(idle_action_section);
    lv_label_set_text(idle_action_explain_label, TR("Choose what happens when idle:"));
    lv_obj_add_style(idle_action_explain_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_action_explain_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_explain_label, LV_ALIGN_TOP_LEFT, 0, 0);

    idle_action_poweroff_row = add_pill_row_base(idle_action_section, TR("Power Off"));
    lv_obj_align(idle_action_poweroff_row, LV_ALIGN_TOP_MID, 0, idle_action_row1_y);
    lv_obj_add_style(idle_action_poweroff_row, &style_theme_card_bg, 0);
    /* Keep the accent outline last: style_theme_card_bg also defines a
     * border color and otherwise hides the selection accent. The shared
     * outline style is transparent, so restore the card fill explicitly. */
    lv_obj_add_style(idle_action_poweroff_row, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_bg_opa(idle_action_poweroff_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(idle_action_poweroff_row, current_settings.idle_suspend_enabled ? 0 : 3, 0);
    lv_obj_add_flag(idle_action_poweroff_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_poweroff_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) false);
    idle_action_poweroff_check = lv_label_create(idle_action_poweroff_row);
    lv_label_set_text(idle_action_poweroff_check, LV_SYMBOL_OK);
    lv_obj_add_style(idle_action_poweroff_check, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(idle_action_poweroff_check, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_poweroff_check, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
    lv_obj_remove_flag(idle_action_poweroff_check, LV_OBJ_FLAG_CLICKABLE);
    if (current_settings.idle_suspend_enabled) lv_obj_add_flag(idle_action_poweroff_check, LV_OBJ_FLAG_HIDDEN);

    idle_action_suspend_row = add_pill_row_base(idle_action_section, TR("Suspend to RAM"));
    lv_obj_align(idle_action_suspend_row, LV_ALIGN_TOP_MID, 0, idle_action_row2_y);
    lv_obj_add_style(idle_action_suspend_row, &style_theme_card_bg, 0);
    lv_obj_add_style(idle_action_suspend_row, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_bg_opa(idle_action_suspend_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, current_settings.idle_suspend_enabled ? 3 : 0, 0);
    lv_obj_add_flag(idle_action_suspend_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_suspend_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) true);
    idle_action_suspend_check = lv_label_create(idle_action_suspend_row);
    lv_label_set_text(idle_action_suspend_check, LV_SYMBOL_OK);
    lv_obj_add_style(idle_action_suspend_check, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(idle_action_suspend_check, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_suspend_check, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
    lv_obj_remove_flag(idle_action_suspend_check, LV_OBJ_FLAG_CLICKABLE);
    if (!current_settings.idle_suspend_enabled) lv_obj_add_flag(idle_action_suspend_check, LV_OBJ_FLAG_HIDDEN);

    /* Slider card positioned below idle_action_section. Sized at 200px height
     * to accommodate the explanatory caption above the slider. */
    idle_shutdown_slider_card = build_setting_slider_card(content, idle_action_section, BOARD_SCALE_PX(200), BOARD_SCALE_PX(48),
        0, IDLE_SHUTDOWN_STEP_COUNT - 1,
        idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes),
        idle_shutdown_slider_event_cb, &idle_shutdown_slider, &idle_shutdown_value_label);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_shutdown_slider_caption = lv_label_create(idle_shutdown_slider_card);
    lv_label_set_text(idle_shutdown_slider_caption, TR("Idle timeout:"));
    lv_obj_add_style(idle_shutdown_slider_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_shutdown_slider_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_shutdown_slider_caption, LV_ALIGN_TOP_LEFT, 24, 14);
    char initial_buf[32];
    format_idle_shutdown(initial_buf, sizeof(initial_buf), current_settings.idle_shutdown_minutes);
    lv_label_set_text(idle_shutdown_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void idle_shutdown_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(idle_shutdown_screen);
}

/* Defined later, alongside its confirmation popup (build_factory_reset_popup(),
 * a hand-built top-layer overlay). */
static void factory_reset_btn_cb(lv_event_t * e);

/* ---- Settings category sub-screens -- grouped into category screens
 * (Display, Power & Sleep, Device & Storage, Date & Time, About). ---- */

/* ---- Music Settings sub-screens -- grouped into category screens
 * (Playback, Audio, Controls & Interface, Timers, Library, Plugins). ---- */


/* Shared click handler for every plugin-registered Playback list row below
 * -- same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_playback_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_playback_list_item_clicked(index);
}

static void plugin_music_audio_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_audio_list_item_clicked(index);
}

static void plugin_music_controls_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_controls_list_item_clicked(index);
}

static void music_controls_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!music_controls_screen) music_controls_screen = build_music_controls_screen();
    if (music_controls_screen) nav_push(music_controls_screen);
}

static void plugin_music_library_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_library_list_item_clicked(index);
}

/* Filter plugin rows into the new settings groups while preserving the
 * original plugin index used by each click handler. Unknown groups stay in
 * the direct list so older/future plugins remain reachable. */
static int append_grouped_plugin_rows(pill_list_item_t * items, int count, int max_items,
                                      const char * list_id, const char * group,
                                      plugin_list_item_count_cb_t get_count_fn,
                                      plugin_list_item_label_cb_t get_label_fn,
                                      plugin_list_item_options_cb_t get_options_fn,
                                      lv_event_cb_t click_cb) {
    int plugin_count = get_count_fn();
    int added = 0;
    for (int i = 0; i < plugin_count && added < max_items; ++i) {
        const char * row_group = plugin_manager_get_list_item_group(list_id, i);
        bool include = group ? (row_group && strcmp(row_group, group) == 0)
                             : (!row_group || ((strcmp(list_id, "music_audio") == 0) &&
                                                strcmp(row_group, "effects") != 0 &&
                                                strcmp(row_group, "profiles") != 0) ||
                               ((strcmp(list_id, "display") == 0) &&
                                strcmp(row_group, "appearance") != 0 &&
                                strcmp(row_group, "player_layout") != 0));
        if (!include) continue;
        pill_list_item_t item = { get_label_fn(i), PILL_ACCESSORY_CHEVRON, false,
                                  click_cb, NULL, (void *) (intptr_t) i };
        const char * text_size = NULL;
        get_options_fn(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count + added++] = item;
    }
    return count + added;
}

static int count_plugin_rows_in_group(const char * list_id, int plugin_count, const char * group) {
    int count = 0;
    for (int i = 0; i < plugin_count; ++i) {
        const char * candidate = plugin_manager_get_list_item_group(list_id, i);
        if (candidate && strcmp(candidate, group) == 0) ++count;
    }
    return count;
}

static void music_effects_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sound_effects_screen) {
        static pill_list_item_t rows[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
        int count = append_grouped_plugin_rows(rows, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
            "music_audio", "effects", plugin_manager_get_music_audio_list_item_count,
            plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
            plugin_music_audio_list_item_click_cb);
        settings_sound_effects_screen = build_pill_list_screen(TR("Sound Effects"), generic_back_cb, rows,
            count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
        finalize_screen_navigation(settings_sound_effects_screen);
    }
    if (settings_sound_effects_screen) nav_push(settings_sound_effects_screen);
}

static void music_profiles_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_eq_profiles_menu_screen) {
        static pill_list_item_t rows[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
        int count = append_grouped_plugin_rows(rows, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
            "music_audio", "profiles", plugin_manager_get_music_audio_list_item_count,
            plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
            plugin_music_audio_list_item_click_cb);
        settings_eq_profiles_menu_screen = build_pill_list_screen(TR("Download Profiles"), generic_back_cb,
            rows, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
        finalize_screen_navigation(settings_eq_profiles_menu_screen);
    }
    if (settings_eq_profiles_menu_screen) nav_push(settings_eq_profiles_menu_screen);
}

static lv_obj_t * build_music_playback_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    lv_obj_t * car_row = NULL;
    items[0] = (pill_list_item_t){ TR("Resume Last Track"), PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Crossfade"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    /* Directly below Crossfade: the two are coupled (crossfade needs an
     * armed next track, which only gapless provides), so seeing them
     * together is what makes that relationship legible when one flips the
     * other -- see gui_player_set_gapless_enabled(). */
    items[2] = (pill_list_item_t){ TR("Gapless"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.gapless_enabled, NULL, gapless_switch_event_cb, NULL,
                                    &settings_gapless_toggle_img };
    items[3] = (pill_list_item_t){ TR("Car Mode"), PILL_ACCESSORY_CHEVRON,
                                    false, gui_volume_settings_car_mode_row_cb, NULL, NULL };
    items[3].out_row = &car_row;
    items[4] = (pill_list_item_t){ TR("Buttons & Remote"), PILL_ACCESSORY_CHEVRON,
                                    false, music_controls_menu_cb, NULL, NULL };

    int count = 5;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_PLAYBACK_LIST_ITEMS,
                                    plugin_manager_get_playback_list_item_count,
                                    plugin_manager_get_playback_list_item_label,
                                    plugin_manager_get_playback_list_item_options,
                                    plugin_playback_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Playback & Controls"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_car_summary = add_pill_summary_label(car_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static bool native_dac_controls_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dac_controls_supported();
#endif
}

static bool native_dac_dre_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dac_dre_supported();
#endif
}

static void apply_dac_preferences(void) {
#ifndef HOST_BUILD
    audio_output_set_dac_preferences(current_settings.dac_filter, current_settings.dac_nos,
                                     current_settings.dac_dre);
#endif
}

static void populate_dac_settings_screen(void);

static void dac_filter_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int filter = (int) (intptr_t) lv_event_get_user_data(e);
    if (filter < 0 || filter > 3) return;
    current_settings.dac_filter = filter;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) {
        populate_dac_settings_screen();
    }
}

static void dac_nos_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.dac_nos = !current_settings.dac_nos;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) populate_dac_settings_screen();
}

static void dac_dre_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.dac_dre = !current_settings.dac_dre;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) populate_dac_settings_screen();
}

static const char * const dac_filter_labels[4] = {
    N_("Fast roll-off, low latency"),
    N_("Fast roll-off, phase compensated"),
    N_("Slow roll-off, low latency"),
    N_("Slow roll-off, phase compensated")
};

static void populate_dac_settings_screen(void) {
    if (!dac_settings_list) return;
    lv_obj_clean(dac_settings_list);
    for (int i = 0; i < 4; ++i) {
        lv_obj_t * row = add_pill_option_row(dac_settings_list, TR(dac_filter_labels[i]),
            current_settings.dac_filter == i, dac_filter_event_cb, (void *) (intptr_t) i);
        lv_obj_t * label = lv_obj_get_child(row, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, lv_pct(78));
    }
    lv_obj_t * nos_row = add_pill_toggle_row(dac_settings_list, TR("NOS Mode"),
        current_settings.dac_nos, dac_nos_event_cb);
    lv_obj_t * nos_label = lv_obj_get_child(nos_row, 0);
    lv_label_set_long_mode(nos_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nos_label, lv_pct(78));
    if (native_dac_dre_supported()) {
        lv_obj_t * dre_row = add_pill_toggle_row(dac_settings_list, TR("DRE"),
            current_settings.dac_dre, dac_dre_event_cb);
        lv_obj_t * dre_label = lv_obj_get_child(dre_row, 0);
        lv_label_set_long_mode(dre_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(dre_label, lv_pct(78));
    }
    lv_obj_t * detail = add_section_header(dac_settings_list,
        TR("Filters and NOS affect local PCM playback. NOS bypasses interpolation and ignores the selected filter. Changes apply when playback restarts."));
    lv_obj_set_width(detail, lv_pct(100));
    lv_obj_set_style_pad_right(detail, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    if (native_dac_dre_supported()) {
        lv_obj_t * dre_detail = add_section_header(dac_settings_list,
            TR("DRE (Dynamic Range Enhancement) affects local PCM and DSD playback."));
        lv_obj_set_width(dre_detail, lv_pct(100));
        lv_obj_set_style_pad_right(dre_detail, BOARD_SCALE_PX(24), 0);
        lv_label_set_long_mode(dre_detail, LV_LABEL_LONG_WRAP);
    }
}

static lv_obj_t * build_dac_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Local DAC Settings"), &title, &dac_settings_list);
    (void) title;
    populate_dac_settings_screen();
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !dac_settings_screen) return;
    populate_dac_settings_screen();
    nav_push(dac_settings_screen);
}

static const char * const usb_dsd_output_labels[] = { N_("PCM"), N_("Native DSD"), N_("DoP") };
static void usb_dsd_output_event_cb(lv_event_t * e);

static void populate_usb_dsd_settings_screen(void) {
    if (!usb_dsd_settings_list) return;
    lv_obj_clean(usb_dsd_settings_list);
    for (int i = USB_DSD_OUTPUT_PCM; i <= USB_DSD_OUTPUT_DOP; ++i) {
        lv_obj_t * row = add_pill_option_row(usb_dsd_settings_list, TR(usb_dsd_output_labels[i]),
            current_settings.usb_dsd_output == i, usb_dsd_output_event_cb, (void *) (intptr_t) i);
        lv_obj_t * label = lv_obj_get_child(row, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, lv_pct(78));
    }
    lv_obj_t * detail = add_section_header(usb_dsd_settings_list,
        TR("Native DSD and DoP bypass software volume and DSP. Use DoP only with a compatible DAC. Unavailable formats or rates use PCM."));
    lv_obj_set_width(detail, lv_pct(100));
    lv_obj_set_style_pad_right(detail, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
}

static void usb_dsd_output_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int output = (int) (intptr_t) lv_event_get_user_data(e);
    if (output < USB_DSD_OUTPUT_PCM || output > USB_DSD_OUTPUT_DOP) return;
    current_settings.usb_dsd_output = output;
#ifndef HOST_BUILD
    audio_output_set_usb_dsd_mode(output);
#endif
    settings_save_async(&current_settings);
    populate_usb_dsd_settings_screen();
}

static lv_obj_t * build_usb_dsd_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("USB DSD Output"), &title, &usb_dsd_settings_list);
    (void) title;
    populate_usb_dsd_settings_screen();
    finalize_screen_navigation(scr);
    return scr;
}

static void usb_dsd_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !usb_dsd_settings_screen) return;
    populate_usb_dsd_settings_screen();
    nav_push(usb_dsd_settings_screen);
}

static lv_obj_t * build_music_audio_screen(void) {
    static pill_list_item_t items[8 + PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    lv_obj_t * eq_row = NULL;
    items[0] = (pill_list_item_t){ TR("Equalizer"), PILL_ACCESSORY_CHEVRON, false, gui_equalizer_eq_row_event_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Crossfeed"), PILL_ACCESSORY_CHEVRON, false, gui_equalizer_crossfeed_row_event_cb, NULL, NULL };
    items[0].out_row = &eq_row;
    int count = append_grouped_plugin_rows(items, 2, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
                                       "music_audio", NULL,
                                       plugin_manager_get_music_audio_list_item_count,
                                       plugin_manager_get_music_audio_list_item_label,
                                       plugin_manager_get_music_audio_list_item_options,
                                       plugin_music_audio_list_item_click_cb);
    items[count++] = (pill_list_item_t){ TR("Startup Volume"), PILL_ACCESSORY_CHEVRON, false, gui_volume_settings_startup_volume_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Max Volume"), PILL_ACCESSORY_CHEVRON, false, gui_volume_settings_max_volume_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("ReplayGain"), PILL_ACCESSORY_CHEVRON, false, replaygain_mode_settings_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("USB DSD Output"), PILL_ACCESSORY_CHEVRON, false, usb_dsd_settings_row_cb, NULL, NULL };
    if (native_dac_controls_supported())
        items[count++] = (pill_list_item_t){ TR("Local DAC Settings"), PILL_ACCESSORY_CHEVRON, false, dac_settings_row_cb, NULL, NULL };
    if (count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "effects") > 0)
        items[count++] = (pill_list_item_t){ TR("Sound Effects"), PILL_ACCESSORY_CHEVRON, false, music_effects_menu_cb, NULL, NULL };

    lv_obj_t * scr = build_pill_list_screen(TR("Sound"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_eq_summary = add_pill_summary_label(eq_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void volume_lock_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.volume_lock_screen_off =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    hw_buttons_set_volume_lock_screen_off(current_settings.volume_lock_screen_off);
}

static lv_obj_t * build_music_controls_screen(void) {
    static pill_list_item_t items[3 + PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS];
    lv_obj_t *button_mapping_row = NULL;
    items[0] = (pill_list_item_t){ TR("Button Mapping"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_button_mapping_row_cb, NULL, NULL };
    items[0].out_row = &button_mapping_row;
    items[1] = (pill_list_item_t){ TR("Volume Lock (Screen Off)"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.volume_lock_screen_off, NULL, volume_lock_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ TR("In-line Remote"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.inline_remote_enabled, NULL, inline_remote_switch_event_cb, NULL };

    int count = 3;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS,
                                    plugin_manager_get_music_controls_list_item_count,
                                    plugin_manager_get_music_controls_list_item_label,
                                    plugin_manager_get_music_controls_list_item_options,
                                    plugin_music_controls_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Buttons & Remote"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    items[0].out_row = NULL;
    if (button_mapping_row) gui_button_mapping_set_category_summary(add_pill_summary_label(button_mapping_row));
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

void refresh_all_metadata_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_metadata_prompt();
}

static void refresh_all_covers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_covers_prompt();
}

static void refresh_sorting_options(void) {
    int album_sort = current_settings.album_sort_mode;
    if (!metadata_db_album_sort_available((metadata_db_album_sort_t) album_sort)) album_sort = 0;
    int selected_sorts[] = { album_sort, current_settings.artist_albums_sort_mode,
                             current_settings.album_artist_albums_sort_mode };
    for (int i = 0; i < SORTING_OPTION_COUNT; i++) {
        int selected_sort;
        bool selected;
        if (i < SORTING_FILE_OPTION_COUNT) selected = current_settings.file_sort_mode == i;
        else {
            int context, sort;
            if (i == SORTING_GLOBAL_ALBUM_ARTIST_OPTION) {
                context = 0;
                sort = METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR;
            } else {
                int relative = i - SORTING_FILE_OPTION_COUNT;
                context = relative / SORTING_ALBUM_MODE_COUNT;
                sort = relative % SORTING_ALBUM_MODE_COUNT;
            }
            selected_sort = selected_sorts[context];
            if (context != 0 && selected_sort > METADATA_DB_ALBUM_SORT_YEAR) selected_sort = 0;
            if (!metadata_db_album_sort_available((metadata_db_album_sort_t) selected_sort)) selected_sort = 0;
            selected = selected_sort == sort;
        }
        if (settings_sorting_options[i])
            lv_obj_set_style_border_width(settings_sorting_options[i], selected ? BOARD_SCALE_PX(3) : 0, 0);
    }
}

static void sorting_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int option = (int) (intptr_t) lv_event_get_user_data(e);
    if (option < 0 || option >= SORTING_OPTION_COUNT) return;
    gui_library_sort_scope_t scope;
    if (option < SORTING_FILE_OPTION_COUNT) {
        if (current_settings.file_sort_mode == option) return;
        current_settings.file_sort_mode = option;
        scope = GUI_LIBRARY_SORT_FILES;
    } else {
        int context, sort_value;
        if (option == SORTING_GLOBAL_ALBUM_ARTIST_OPTION) {
            context = 0;
            sort_value = METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR;
        } else {
            int relative = option - SORTING_FILE_OPTION_COUNT;
            context = relative / SORTING_ALBUM_MODE_COUNT;
            sort_value = relative % SORTING_ALBUM_MODE_COUNT;
        }
        metadata_db_album_sort_t sort = (metadata_db_album_sort_t) sort_value;
        if (sort != METADATA_DB_ALBUM_SORT_NAME && !metadata_db_album_sort_available(sort)) {
            show_error_toast(TR("Update Music Database to enable this album order"));
            return;
        }
        if (context == 0) {
            if (current_settings.album_sort_mode == sort_value) return;
            current_settings.album_sort_mode = sort_value;
            scope = GUI_LIBRARY_SORT_ALBUMS;
        } else if (context == 1) {
            if (current_settings.artist_albums_sort_mode == sort_value) return;
            current_settings.artist_albums_sort_mode = sort_value;
            scope = GUI_LIBRARY_SORT_ARTIST_ALBUMS;
        } else {
            if (current_settings.album_artist_albums_sort_mode == sort_value) return;
            current_settings.album_artist_albums_sort_mode = sort_value;
            scope = GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS;
        }
    }
    settings_save_async(&current_settings);
    gui_library_apply_sorting(scope);
    refresh_sorting_options();
}

static lv_obj_t * build_sorting_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Sorting"), &title, &list);
    (void) title;
    add_section_header(list, TR("Files (folders stay first)"));
    static const char * labels[] = { N_("Name (A–Z)"), N_("Newest Modified") };
    static const char * group_titles[] = { N_("Albums"), N_("Artists"), N_("Album Artist") };
    static const char * album_labels[] = { N_("Name (A–Z)"), N_("Recently Added"), N_("Release Year (oldest first)") };
    static const char * global_album_artist_label = N_("Album Artist (A–Z)");
    for (int i = 0; i < SORTING_FILE_OPTION_COUNT; i++) {
        settings_sorting_options[i] = add_pill_option_row(list, TR(labels[i]), false,
                                            sorting_option_cb, (void *) (intptr_t) i);
        lv_obj_add_style(settings_sorting_options[i], gui_theme_accent_outline_style(), 0);
    }
    for (int context = 0; context < SORTING_ALBUM_CONTEXT_COUNT; context++) {
        add_section_header(list, TR(group_titles[context]));
        for (int sort = 0; sort < SORTING_ALBUM_MODE_COUNT; sort++) {
            int option = SORTING_FILE_OPTION_COUNT + context * SORTING_ALBUM_MODE_COUNT + sort;
            settings_sorting_options[option] = add_pill_option_row(list, TR(album_labels[sort]), false,
                                                sorting_option_cb, (void *) (intptr_t) option);
            lv_obj_add_style(settings_sorting_options[option], gui_theme_accent_outline_style(), 0);
        }
        if (context == 0) {
            settings_sorting_options[SORTING_GLOBAL_ALBUM_ARTIST_OPTION] =
                add_pill_option_row(list, TR(global_album_artist_label), false,
                                    sorting_option_cb,
                                    (void *) (intptr_t) SORTING_GLOBAL_ALBUM_ARTIST_OPTION);
            lv_obj_add_style(settings_sorting_options[SORTING_GLOBAL_ALBUM_ARTIST_OPTION],
                             gui_theme_accent_outline_style(), 0);
        }
    }
    lv_obj_t * help = add_section_header(list,
        TR("Files use modification dates. Albums use the newest track added; missing release years sort last. Update Music Database once to read years from existing files."));
    lv_obj_set_width(help, lv_pct(100));
    lv_obj_set_style_pad_right(help, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    refresh_sorting_options();
    return screen;
}

static void sorting_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sorting_screen) settings_sorting_screen = build_sorting_screen();
    if (!settings_sorting_screen) return;
    refresh_sorting_options();
    nav_push(settings_sorting_screen);
}

/* Update checks for added, removed and changed files; Refresh re-reads the
 * tags of every song even when a file looks unchanged. Distinct icons keep
 * the two from reading as the same action. */
static lv_obj_t * build_library_category_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS];
    lv_obj_t * native_rows[4] = { NULL };
    items[0] = (pill_list_item_t){ .label = TR("Update Music Database"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = update_music_database_row_cb,
        .out_row = &native_rows[0] };
    items[1] = (pill_list_item_t){ .label = TR("Refresh All Metadata"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_metadata_row_cb,
        .out_row = &native_rows[1] };
    items[2] = (pill_list_item_t){ .label = TR("Refresh All Covers"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_covers_row_cb,
        .out_row = &native_rows[2] };
    items[3] = (pill_list_item_t){ .label = TR("Sorting"), .accessory = PILL_ACCESSORY_CHEVRON,
        .on_click = sorting_row_cb, .out_row = &native_rows[3] };
    int count = append_plugin_list_rows(items, 4, PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS,
        plugin_manager_get_music_library_list_item_count,
        plugin_manager_get_music_library_list_item_label,
        plugin_manager_get_music_library_list_item_options,
        plugin_music_library_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Library"), generic_back_cb, items, count,
        gui_theme_accent_style(), GUI_ROW_GAP, 100);
    decorate_category_row(native_rows[0], "submenu/update_database.png", NULL);
    decorate_category_row(native_rows[1], "submenu/refresh_metadata.png", NULL);
    decorate_category_row(native_rows[2], "submenu/refresh_covers.png", NULL);
    /* Sorting has its own Breeze art; without it, it shares Library's icon. */
    const char * sorting_breeze = asset_path_plain("breeze_rows/library_sorting.png");
    decorate_category_row(native_rows[3], sorting_breeze && access(sorting_breeze, R_OK) == 0
                                              ? "breeze_rows/library_sorting.png" : "settings/library.png", NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Display list row below --
 * same index-not-object shape plugin_settings_list_item_click_cb() (defined
 * further down, alongside build_settings_screen()) already uses. Forward-
 * declared static since build_settings_display_screen() is defined earlier
 * in the file than that sibling. */
static void plugin_display_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_display_list_item_clicked(index);
}

static void lyrics_keep_screen_on_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.lyrics_keep_screen_on =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
}

typedef struct {
    int scale;
    const char * label;
} animation_speed_option_t;

static const animation_speed_option_t animation_speed_options[] = {
    { 0, N_("Off") }, { 25, "0.25x" }, { 50, "0.5x" }, { 75, "0.75x" }, { 100, "1x" },
};
#define ANIMATION_SPEED_OPTION_COUNT (sizeof(animation_speed_options) / sizeof(animation_speed_options[0]))

static void animation_speed_option_row_cb(lv_event_t * e);

static void populate_animation_speed_screen(void) {
    if (!animation_speed_list) return;
    lv_obj_clean(animation_speed_list);
    for (size_t i = 0; i < ANIMATION_SPEED_OPTION_COUNT; i++) {
        bool selected = current_settings.animation_scale == animation_speed_options[i].scale;
        add_pill_option_row(animation_speed_list, TR(animation_speed_options[i].label),
                            selected, animation_speed_option_row_cb, (void *) (intptr_t) i);
    }
}

static void animation_speed_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.animation_scale = animation_speed_options[index].scale;
    settings_save(&current_settings);
    populate_animation_speed_screen();
}

static lv_obj_t * build_animation_speed_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Animation Speed"), &title_label, &animation_speed_list);
}

static void animation_speed_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_animation_speed_screen();
    nav_push(animation_speed_screen);
}

typedef struct {
    int layout;
    const char * label;
} keyboard_layout_option_t;

static const keyboard_layout_option_t keyboard_layout_options[] = {
    { KEYBOARD_LAYOUT_T9, N_("T9") },
    { KEYBOARD_LAYOUT_QWERTY, N_("QWERTY") },
    { KEYBOARD_LAYOUT_AZERTY, "AZERTY (Français)" },
    { KEYBOARD_LAYOUT_QWERTZ, "QWERTZ (Deutsch)" },
    { KEYBOARD_LAYOUT_RUSSIAN, "Русский (ЙЦУКЕН)" },
    { KEYBOARD_LAYOUT_UKRAINIAN, "Українська (ЙЦУКЕН)" },
};
#define KEYBOARD_LAYOUT_OPTION_COUNT (sizeof(keyboard_layout_options) / sizeof(keyboard_layout_options[0]))

size_t gui_settings_keyboard_layout_count(void) { return KEYBOARD_LAYOUT_OPTION_COUNT; }

int gui_settings_keyboard_layout_at(size_t index, const char ** label) {
    if (index >= KEYBOARD_LAYOUT_OPTION_COUNT) return -1;
    if (label) *label = keyboard_layout_options[index].label;
    return keyboard_layout_options[index].layout;
}

static void keyboard_layout_option_row_cb(lv_event_t * e);

static void populate_keyboard_layout_screen(void) {
    if (!keyboard_layout_list) return;
    lv_obj_clean(keyboard_layout_list);
    for (size_t i = 0; i < KEYBOARD_LAYOUT_OPTION_COUNT; i++) {
        bool selected = current_settings.keyboard_layout == keyboard_layout_options[i].layout;
        add_pill_option_row(keyboard_layout_list, TR(keyboard_layout_options[i].label),
                            selected, keyboard_layout_option_row_cb, (void *) (intptr_t) i);
    }
}

/* Takes effect the next time a text field or inline search opens -- the
 * shared keyboard picks its layout at session start (gui_text_input.c). */
static void keyboard_layout_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.keyboard_layout = keyboard_layout_options[index].layout;
    settings_save(&current_settings);
    populate_keyboard_layout_screen();
}

static lv_obj_t * build_keyboard_layout_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Keyboard"), &title_label, &keyboard_layout_list);
}

static void keyboard_layout_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_keyboard_layout_screen();
    nav_push(keyboard_layout_screen);
}

static void upside_down_screen_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_upside_down = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_display_apply_rotation(current_settings.screen_upside_down);
}

static void artist_images_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_artist_images = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_library_set_artist_images_enabled(current_settings.show_artist_images);
}

void gui_display_apply_rotation(bool upside_down) {
    lv_display_t * disp = lv_display_get_default();
    if (!disp) return;
    lv_display_rotation_t target_rot = upside_down ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0;
    if (lv_display_get_rotation(disp) == target_rot) return;
    lv_display_set_rotation(disp, target_rot);
    gui_navigation_invalidate_theme_snapshots();
    lv_obj_t * act = lv_screen_active();
    if (act) lv_obj_invalidate(act);
}

static void themes_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_themes_open_picker();
}

static lv_obj_t * build_settings_appearance_screen(void) {
    static pill_list_item_t items[9 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Accent Color"), PILL_ACCESSORY_CHEVRON, false, gui_accent_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Font"), PILL_ACCESSORY_CHEVRON, false, gui_custom_font_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Font Size"), PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Keyboard"), PILL_ACCESSORY_CHEVRON, false, keyboard_layout_settings_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Quick Drawer"), PILL_ACCESSORY_CHEVRON, false, gui_shell_settings_quick_drawer_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("Music menu"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_shell_settings_music_order_row_cb, NULL, (void *)(intptr_t)UI_ORDER_MUSIC };
    items[6] = (pill_list_item_t){ TR("Top Bar"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_shell_settings_topbar_row_cb, NULL, NULL };
    items[7] = (pill_list_item_t){ TR("Artist Images"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_artist_images, NULL,
                                    artist_images_switch_event_cb, NULL };
    items[8] = (pill_list_item_t){ TR("Themes"), PILL_ACCESSORY_CHEVRON, false, themes_row_cb, NULL, NULL };
    int count = append_grouped_plugin_rows(items, 9, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "appearance", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Appearance"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Player layout choice: the built-in layout plus every registered one (XML
 * files, exported C layouts, a plugin's session layout). Applying a choice
 * replaces the Player in the background without rebuilding this picker. */
static void player_layout_choice_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const player_layout_info_t * info = player_layouts_get((int) (intptr_t) lv_event_get_user_data(e));
    if (!info) return;
    if (strcmp(info->id, player_layouts_effective_id()) == 0) return;
    /* Picking a layout here replaces a plugin's session layout too. */
    player_layouts_session_clear_selection();
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s",
             strcmp(info->id, PLAYER_LAYOUT_ID_DEFAULT) == 0 ? "" : info->id);
    settings_save(&current_settings);
    /* The picker stays alive now. Update its selection ring in place so
     * the current scroll position and event targets remain unchanged. */
    lv_obj_t * selected_card = lv_event_get_current_target(e);
    lv_obj_t * grid = lv_obj_get_parent(selected_card);
    for (uint32_t i = 0; grid && i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_t * card = lv_obj_get_child(grid, (int32_t) i);
        lv_obj_set_style_outline_width(card, card == selected_card ? BOARD_SCALE_PX(2) : 0, 0);
    }
    gui_player_layout_reload_request();
}

static void player_layout_download_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    (void) gui_plugin_store_open_player_layouts();
}

void gui_settings_populate_player_layout_picker(lv_obj_t * list, bool suggested_only) {
    if (!list) return;
    lv_obj_clean(list);
    player_layouts_rescan();
    if (!suggested_only) {
        lv_obj_t * download = add_pill_chevron_row(list, TR("Download"), player_layout_download_cb);
        lv_obj_set_width(download, lv_pct(100));
    }
    const char * active = player_layouts_effective_id();
    lv_obj_t * grid = lv_obj_create(list);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    configure_cover_card_grid(grid, 2);
    /* The outline extends outside each card. Reserve space at the grid's
     * edges so its first and last rows retain the complete selection ring. */
    lv_obj_set_style_pad_top(grid, BOARD_SCALE_PX(6), 0);
    lv_obj_set_style_pad_bottom(grid, BOARD_SCALE_PX(6), 0);
    int preferred_vinyl_index = player_layouts_find("vinyl");
    if (preferred_vinyl_index < 0)
        preferred_vinyl_index = player_layouts_find("plugin.vinyl_player");
    int default_index = player_layouts_find(PLAYER_LAYOUT_ID_DEFAULT);
    int layout_count = suggested_only ? 2 : player_layouts_count();
    for (int selection = 0; selection < layout_count; selection++) {
        int i = suggested_only
            ? (selection == 0 ? default_index : preferred_vinyl_index)
            : selection;
        if (i < 0 || (suggested_only && selection == 1 && i == default_index)) continue;
        const player_layout_info_t * info = player_layouts_get(i);
        bool selected = strcmp(info->id, active) == 0;
        char preview[512], resolved[520];
        const char * preview_src = NULL;
        if (player_layouts_get_preview(i, preview, sizeof(preview))) {
            snprintf(resolved, sizeof(resolved), "S:%s", preview);
            preview_src = resolved;
        }
        lv_obj_t * card = add_cover_card(grid, info->name, preview_src, 2,
                                          player_layout_choice_row_cb, (void *) (intptr_t) i);
        if (!card) continue;
        lv_obj_set_style_radius(card, BOARD_SCALE_PX(10), 0);
        lv_obj_set_style_outline_width(card, selected ? BOARD_SCALE_PX(2) : 0, 0);
        lv_obj_set_style_outline_color(card, accent_lv_color(), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_pad(card, BOARD_SCALE_PX(2), 0);

    }
}

static void player_layout_choice_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!player_layout_choice_screen) return;
    gui_settings_populate_player_layout_picker(player_layout_choice_list, false);
    nav_push(player_layout_choice_screen);
}

static lv_obj_t * build_player_layout_choice_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Layout"), &title_label, &player_layout_choice_list);
}

static lv_obj_t * build_settings_player_layout_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Layout"), PILL_ACCESSORY_CHEVRON, false,
                                    player_layout_choice_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Lyrics"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_enabled, NULL, lyrics_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ TR("Lyrics Text Size"), PILL_ACCESSORY_CHEVRON, false,
                                    lyrics_font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Hide Player/Lyrics Top Bar"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.hide_player_topbar, NULL,
                                    hide_player_topbar_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ TR("Keep Lyrics Screen On"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_keep_screen_on, NULL,
                                    lyrics_keep_screen_on_switch_event_cb, NULL };
    int count = append_grouped_plugin_rows(items, 5, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "player_layout", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Player Layout"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_gestures_screen(void) {
    pill_list_item_t items[] = {
        { TR("Swipe Up for Home"), PILL_ACCESSORY_TOGGLE,
          current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL },
        { TR("Animation Speed"), PILL_ACCESSORY_CHEVRON, false,
          animation_speed_settings_row_cb, NULL, NULL },
        { TR("Upside Down Screen"), PILL_ACCESSORY_TOGGLE,
          current_settings.screen_upside_down, NULL, upside_down_screen_switch_event_cb, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Gestures & Orientation"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_appearance_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_appearance_screen) settings_appearance_screen = build_settings_appearance_screen();
    if (settings_appearance_screen) nav_push(settings_appearance_screen);
}

static void settings_player_layout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_player_layout_screen) settings_player_layout_screen = build_settings_player_layout_screen();
    if (settings_player_layout_screen) nav_push(settings_player_layout_screen);
}

static void settings_gestures_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_gestures_screen) settings_gestures_screen = build_settings_gestures_screen();
    if (settings_gestures_screen) nav_push(settings_gestures_screen);
}

static lv_obj_t * build_settings_display_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Screen Timeout"), PILL_ACCESSORY_CHEVRON, false, gui_display_timing_settings_timeout_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Screen Dimming"), PILL_ACCESSORY_CHEVRON, false, gui_display_timing_settings_dimming_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Appearance"), PILL_ACCESSORY_CHEVRON, false, settings_appearance_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Player Layout"), PILL_ACCESSORY_CHEVRON, false, settings_player_layout_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Gestures & Orientation"), PILL_ACCESSORY_CHEVRON, false, settings_gestures_row_cb, NULL, NULL };
    int count = append_grouped_plugin_rows(items, 5, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", NULL, plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Display"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Power list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_power_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_power_list_item_clicked(index);
}

static lv_obj_t * build_settings_charging_screen(void) {
    pill_list_item_t items[] = {
        { TR("Charge Limit (85%)"), PILL_ACCESSORY_TOGGLE,
          current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL },
        { TR("Safe Charging (500mA)"), PILL_ACCESSORY_TOGGLE,
          current_settings.safe_charging_enabled, NULL, safe_charging_switch_event_cb, NULL },
        { TR("LED charge indicator"), PILL_ACCESSORY_TOGGLE,
          current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL },
#if BOARD_R1_HARDWARE
        {
            .label = TR("Disable charging\nin USB DAC mode"),
            .accessory = PILL_ACCESSORY_TOGGLE,
            .toggle_initial_state = current_settings.usb_dac_disable_charging,
            .on_toggle_change = usb_dac_charging_switch_event_cb,
            .out_toggle_img = &settings_usb_dac_charging_switch,
            .row_height = BOARD_SCALE_PY(128),
            .out_row = &settings_usb_dac_charging_row,
        },
#endif
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Charging"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
#if BOARD_R1_HARDWARE
    if (settings_usb_dac_charging_row && settings_usb_dac_charging_switch) {
        lv_obj_t * label = lv_obj_get_child(settings_usb_dac_charging_row, 0);
        /* This two-line setting must stay wrapped during font refreshes. */
        lv_obj_remove_flag(label, LV_OBJ_FLAG_USER_3);
        lv_obj_set_style_pad_top(label, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), 0);
    }
#endif
    finalize_screen_navigation(scr);
    return scr;
}

#if BOARD_R1_HARDWARE
static void usb_dac_charging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_network_set_usb_dac_charging_disabled(
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

void gui_settings_sync_usb_dac_charging_toggle(void) {
    if (!settings_usb_dac_charging_switch) return;
    if (current_settings.usb_dac_disable_charging)
        lv_obj_add_state(settings_usb_dac_charging_switch, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(settings_usb_dac_charging_switch, LV_STATE_CHECKED);
}

static void settings_charging_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_charging_screen) settings_charging_screen = build_settings_charging_screen();
    if (settings_charging_screen) nav_push(settings_charging_screen);
}

static void sleep_timer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_settings_open_sleep_timer();
}

static void plugin_music_timers_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        plugin_manager_music_timers_list_item_clicked((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * build_settings_power_screen(void) {
    static pill_list_item_t items[3 + PLUGIN_MAX_POWER_LIST_ITEMS + PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS];
    lv_obj_t * sleep_row = NULL;
    items[0] = (pill_list_item_t){ TR("Sleep Timer"), PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };
    items[0].out_row = &sleep_row;
    items[1] = (pill_list_item_t){ TR("Idle Shutdown"), PILL_ACCESSORY_CHEVRON, false, idle_shutdown_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Charging"), PILL_ACCESSORY_CHEVRON, false, settings_charging_row_cb, NULL, NULL };
    int count = 3;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS,
                                    plugin_manager_get_music_timers_list_item_count,
                                    plugin_manager_get_music_timers_list_item_label,
                                    plugin_manager_get_music_timers_list_item_options,
                                    plugin_music_timers_list_item_click_cb);
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_POWER_LIST_ITEMS,
                                    plugin_manager_get_power_list_item_count,
                                    plugin_manager_get_power_list_item_label,
                                    plugin_manager_get_power_list_item_options,
                                    plugin_power_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Power"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_sleep_summary = add_pill_summary_label(sleep_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered System list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_system_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_system_list_item_clicked(index);
}

/* Defined later, alongside the reboot-confirm popup this opens into (see
 * hostname_entry_done_cb()) -- needed here first, for build_settings_
 * system_screen()'s own row list below. */
static void hostname_row_cb(lv_event_t * e);
static void plugin_settings_list_item_click_cb(lv_event_t * e);
static void settings_about_row_cb(lv_event_t * e);
static void settings_tools_row_cb(lv_event_t * e);

static lv_obj_t * build_settings_tools_screen(void) {
    static pill_list_item_t items[PLUGIN_MAX_SETTINGS_LIST_ITEMS];
    int count = append_plugin_list_rows(items, 0, PLUGIN_MAX_SETTINGS_LIST_ITEMS,
                                        plugin_manager_get_settings_list_item_count,
                                        plugin_manager_get_settings_list_item_label,
                                        plugin_manager_get_settings_list_item_options,
                                        plugin_settings_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Additional Tools"), generic_back_cb, items,
                                            count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_maintenance_screen(void) {
    pill_list_item_t items[] = {
        { TR("Hostname"), PILL_ACCESSORY_CHEVRON, false, hostname_row_cb, NULL, NULL },
        { TR("Factory Reset"), PILL_ACCESSORY_NONE, false, factory_reset_btn_cb, NULL, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Maintenance"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_maintenance_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_system_maintenance_screen) settings_system_maintenance_screen = build_settings_maintenance_screen();
    if (settings_system_maintenance_screen) nav_push(settings_system_maintenance_screen);
}

static void settings_plugins_layouts_updates_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    (void) gui_plugin_store_open_updates();
}

static lv_obj_t * build_settings_updates_screen(void) {
    pill_list_item_t items[] = {
        { TR("Firmware Update"), PILL_ACCESSORY_CHEVRON, false, firmware_update_row_cb, NULL, NULL },
        { TR("Player Update"), PILL_ACCESSORY_CHEVRON, false, gui_updates_player_row_cb, NULL, NULL },
        { TR("Plugins & Layouts"), PILL_ACCESSORY_CHEVRON, false, settings_plugins_layouts_updates_row_cb, NULL, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Updates"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_updates_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_system_updates_screen) settings_system_updates_screen = build_settings_updates_screen();
    if (settings_system_updates_screen) nav_push(settings_system_updates_screen);
}

/* Language choice: every language i18n knows, each under its own name.
 * Applying one rebuilds the UI so every screen is built in the new language. */
static void language_choice_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * code = i18n_language_code((size_t) (intptr_t) lv_event_get_user_data(e));
    if (!code || strcmp(code, i18n_get_language()) == 0) return;
    if (!i18n_set_language(code)) return;
    snprintf(current_settings.language, sizeof(current_settings.language), "%s", code);
    settings_save(&current_settings);
    show_info_toast(TR("Applying language, this may take a while"));
    gui_reload_request();
}

static void populate_language_choice_screen(void) {
    if (!language_choice_list) return;
    lv_obj_clean(language_choice_list);
    const char * active = i18n_get_language();
    for (size_t i = 0; i < i18n_language_count(); i++) {
        add_pill_option_row(language_choice_list, i18n_language_name(i), strcmp(i18n_language_code(i), active) == 0,
                            language_choice_row_cb, (void *) (intptr_t) i);
    }
}

static void language_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_language_choice_screen();
    nav_push(language_choice_screen);
}

static lv_obj_t * build_language_choice_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Language"), &title_label, &language_choice_list);
}

static lv_obj_t * build_settings_system_screen(void) {
    static pill_list_item_t items[7 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("USB Mode"), PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Clock"), PILL_ACCESSORY_CHEVRON, false, gui_time_settings_clock_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Language"), PILL_ACCESSORY_CHEVRON, false, language_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Plugin Manager"), PILL_ACCESSORY_CHEVRON, false, gui_plugin_manage_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Maintenance"), PILL_ACCESSORY_CHEVRON, false, settings_maintenance_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("About"), PILL_ACCESSORY_CHEVRON, false, settings_about_row_cb, NULL, NULL };
    int count = 6;
    if (plugin_manager_get_settings_list_item_count() > 0) {
        items[count++] = (pill_list_item_t){ TR("Additional Tools"), PILL_ACCESSORY_CHEVRON, false,
                                             settings_tools_row_cb, NULL, NULL };
    }
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_SYSTEM_LIST_ITEMS,
                                    plugin_manager_get_system_list_item_count,
                                    plugin_manager_get_system_list_item_label,
                                    plugin_manager_get_system_list_item_options,
                                    plugin_system_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("System"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_category_music_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_audio_screen);
}

static void settings_category_playback_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_playback();
}

static void settings_category_library_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_library();
}

void gui_settings_open_library(void) {
    if (!settings_library_screen) settings_library_screen = build_library_category_screen();
    if (!settings_library_screen) return;
    nav_push(settings_library_screen);
}

static void settings_category_display_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_display_screen);
}

static void settings_category_power_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_power_screen);
}

static void settings_category_system_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_system_screen);
}

static void settings_about_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(about_screen);
}

static void settings_tools_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !settings_tools_screen) return;
    nav_push(settings_tools_screen);
}

/* Auto-resume on launch has no settings row while AUTO_RESUME_ON_LAUNCH_ENABLED
 * is disabled at compile time. current_settings.auto_resume_enabled and
 * auto_resume_switch_event_cb remain available for when re-enabled. */
/* Shared click handler for every plugin-registered Settings list row below
 * -- same index-not-object shape plugin_books_list_item_click_cb() already
 * uses for the Books screen. */
static void plugin_settings_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_settings_list_item_clicked(index);
}

static lv_obj_t * build_settings_screen(void) {
    static pill_list_item_t items[7];
    lv_obj_t * category_rows[7] = { NULL };
    items[0] = (pill_list_item_t){ TR("Sound"), PILL_ACCESSORY_CHEVRON, false, settings_category_music_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Playback & Controls"), PILL_ACCESSORY_CHEVRON, false, settings_category_playback_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Display"), PILL_ACCESSORY_CHEVRON, false, settings_category_display_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Power"), PILL_ACCESSORY_CHEVRON, false, settings_category_power_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Library"), PILL_ACCESSORY_CHEVRON, false, settings_category_library_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("System"), PILL_ACCESSORY_CHEVRON, false, settings_category_system_cb, NULL, NULL };
    items[6] = (pill_list_item_t){ TR("Updates"), PILL_ACCESSORY_CHEVRON, false, settings_updates_row_cb, NULL, NULL };
    for (unsigned i = 0; i < 7; ++i) {
        items[i].out_row = &category_rows[i];
    }

    lv_obj_t * scr = build_pill_list_screen(TR("Settings"), generic_back_cb, items, 7, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    static const char * icons[] = {
        "settings/sound.png", "settings/playback.png", "settings/display.png",
        "settings/power.png", "settings/library.png", "settings/system.png",
        "submenu/update_database.png",
    };
    for (unsigned i = 0; i < 7; ++i) {
        if (category_rows[i]) decorate_category_row(category_rows[i], icons[i], NULL);
        items[i].out_row = NULL;
    }
    finalize_screen_navigation(scr);
    return scr;
}

static void stream_media_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(stream_media_screen);
}

static void settings_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_screen);
}

/* ---- DAC home screen -- direct shortcut to enabling USB DAC or Bluetooth
 * DAC without going through Settings > System / Wireless > Bluetooth first.
 * Reuses the exact same underlying entry points those screens already use
 * (start_usb_mode_switch()/open_bt_dac_screen()) rather than duplicating
 * any of that state machine. Replaced the home screen's old "About" tile --
 * About is still reachable, just moved into Settings (see
 * settings_about_row_cb()). ---- */

static void dac_home_usb_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* If USB DAC is already active in hardware, show its overlay directly. */
    usb_mode_t detected;
    bool have_detected = usb_mode_control_detect_current(&detected);
    if (have_detected && detected == USB_MODE_DAC) {
        current_settings.usb_mode = (int) USB_MODE_DAC;
        nav_push(gui_network_get_usb_dac_overlay());
        return;
    }
    /* Require explicit disable of ADB mode before switching to USB DAC. */
    if (have_detected && detected == USB_MODE_ADB) {
        show_info_toast(TR("Turn off ADB first (Settings > System > USB Mode), then enable USB DAC from here."));
        return;
    }
    start_usb_mode_switch(USB_MODE_DAC);
}

static void plugin_more_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_more_list_item_clicked(index);
}

/* Native entries use the shared category look (decorate_category_row());
 * plugin rows registered for "more" follow them with the same look, their
 * icon resolved like every other plugin row icon. The screen is rebuilt on
 * every plugin reload (gui_shell_build_screens()), so rows track the
 * currently loaded plugins. */
lv_obj_t * build_dac_home_screen(void) {
    static const struct { const char * icon; const char * label; lv_event_cb_t cb; } native[] = {
        { "submenu/plugins.png", N_("Plugins"), gui_plugin_manage_row_cb },
        { "submenu/files.png", N_("File Manager"), gui_file_manager_tile_cb },
        { "submenu/themes.png", N_("Themes"), themes_row_cb },
        { "submenu/layouts.png", N_("Now Playing layouts"), player_layout_choice_settings_row_cb },
        { "submenu/usb.png", N_("USB DAC"), dac_home_usb_row_cb },
        { "submenu/bluetooth.png", N_("Bluetooth DAC"), bt_dac_settings_row_cb },
#if BOARD_SUPPORTS_EMULATORS
        { "submenu/emulators.png", N_("Emulators"), gui_emulators_row_cb },
#endif
    };
    enum { NATIVE_COUNT = (int) (sizeof(native) / sizeof(native[0])) };
    static pill_list_item_t items[NATIVE_COUNT + PLUGIN_MAX_MORE_LIST_ITEMS];
    lv_obj_t * rows[NATIVE_COUNT + PLUGIN_MAX_MORE_LIST_ITEMS] = { NULL };
    const char * plugin_icons[PLUGIN_MAX_MORE_LIST_ITEMS] = { NULL };

    for (int i = 0; i < NATIVE_COUNT; ++i) {
        items[i] = (pill_list_item_t){ TR(native[i].label), PILL_ACCESSORY_CHEVRON, false, native[i].cb, NULL, NULL };
    }
    int count = append_plugin_list_rows(items, NATIVE_COUNT, PLUGIN_MAX_MORE_LIST_ITEMS,
                                        plugin_manager_get_more_list_item_count,
                                        plugin_manager_get_more_list_item_label,
                                        plugin_manager_get_more_list_item_options,
                                        plugin_more_list_item_click_cb);
    for (int i = 0; i < count; ++i) {
        if (i >= NATIVE_COUNT) {
            /* Drawn below at the native 44 px size, not by the pill row. */
            plugin_icons[i - NATIVE_COUNT] = items[i].icon_asset;
            items[i].icon_asset = NULL;
        }
        items[i].out_row = &rows[i];
    }

    lv_obj_t * scr = build_pill_list_screen(TR("More"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    for (int i = 0; i < count; ++i) {
        if (rows[i]) {
            if (i < NATIVE_COUNT) decorate_category_row(rows[i], native[i].icon, NULL);
            else decorate_category_row_plugin_icon(rows[i], plugin_icons[i - NATIVE_COUNT]);
        }
        items[i].out_row = NULL;
    }
    finalize_screen_navigation(scr);
    return scr;
}

lv_obj_t * build_home_screen(void) {
    return gui_home_build_screen(settings_tile_cb, stream_media_tile_cb);
}

/* ---- Reset PEQ to defaults, with a confirmation popup -- same hand-built
 * top-layer overlay shape as bt_dac_leave_popup (this codebase doesn't use
 * LVGL's lv_msgbox anywhere), since a factory reset of every band's
 * freq/gain/Q/type plus the preamp is not something a stray tap should be
 * able to trigger by accident. ---- */
/* ---- Factory Reset, with a confirmation popup -- same hand-built
 * top-layer overlay shape as the other confirmation popups (this codebase
 * doesn't use LVGL's lv_msgbox anywhere). Wiping every app setting is
 * exactly the kind of thing a stray tap must never be able to trigger.
 * Confirmation deletes configuration and reboots; the device path does not
 * return to the old UI after a destructive reset. ---- */
static gui_popup_t factory_reset_popup;

static void hide_factory_reset_popup(void) {
    gui_popup_hide(&factory_reset_popup);
}

static void factory_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
    if (firmware_update_busy()) {
        show_error_toast(TR("An update is already in progress"));
        return;
    }
    settings_factory_reset(); /* deletes the settings file and reboots -- see its own comment in settings.h/.c */
}

static void factory_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&factory_reset_popup);
}

static void build_factory_reset_popup(void) {
    factory_reset_popup.popup = build_confirm_popup(
        TR("Reset all settings and reboot?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Reset"), lv_color_hex(GUI_BREEZE_NEGATIVE),
        factory_reset_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), factory_reset_cancel_cb, NULL,
        factory_reset_popup_backdrop_cb, &factory_reset_popup.backdrop);
}

/* Settings -> System -> Hostname uses the shared confirmation-popup helper;
 * see hostname_apply()'s own comment for why a reboot is genuinely required
 * here (wifi_on.sh/bt_init each only read their file once, on demand). */
static gui_popup_t hostname_reboot_popup;

static void hide_hostname_reboot_popup(void) {
    gui_popup_hide(&hostname_reboot_popup);
}

static void hostname_reboot_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
    if (firmware_update_busy()) {
        show_error_toast(TR("An update is already in progress"));
        return;
    }
    idle_shutdown_reboot_now();
}

static void show_hostname_reboot_popup(void) {
    gui_popup_show(&hostname_reboot_popup);
}

static void build_hostname_reboot_popup(void) {
    hostname_reboot_popup.popup = build_confirm_popup(
        TR("Restart now to apply the new hostname?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Restart Now"), accent_lv_color(),
        hostname_reboot_now_cb, NULL, TR("Later"), lv_color_make(160, 160, 160), hostname_reboot_later_cb, NULL,
        hostname_reboot_popup_backdrop_cb, &hostname_reboot_popup.backdrop);
}

/* RFC 952/1123 hostname-label charset -- letters/digits/hyphen only, no
 * leading/trailing hyphen. Real bug caught in review: nothing validated
 * this before it was written straight to /usr/data/hostname_override.txt
 * and handed to sethostname() (see hostname_apply.c) -- a space or
 * punctuation typed on the on-screen keyboard would produce an invalid
 * WiFi/BT broadcast name, or corrupt whatever naive parsing a downstream
 * consumer of those two bind-mounted files does. Empty is exempted --
 * that's hostname_entry_done_cb()'s own "reset to stock" sentinel below,
 * not a real hostname. */
static bool hostname_is_valid(const char * text) {
    size_t len = strlen(text);
    if (len == 0) return true;
    if (len > 63) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static void hostname_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!hostname_is_valid(text)) {
        show_error_toast(TR("Hostname can only use letters, numbers, and hyphens"));
        return;
    }
    /* Empty submission means "reset to the stock name" -- hostname_apply()
     * itself treats an empty string as a no-op (leaves whatever's already
     * in effect from a previous boot alone), so resetting to stock also
     * needs a reboot back to the un-overridden squashfs file, same as
     * setting a new one does. */
    snprintf(current_settings.hostname, sizeof(current_settings.hostname), "%s", text);
    settings_save(&current_settings);
    show_hostname_reboot_popup();
}

static void hostname_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Hostname"), current_settings.hostname, false, false, hostname_entry_done_cb, NULL);
}

static bool gui_settings_profiles_download_available(void) {
    return count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "profiles") > 0;
}

void gui_settings_init(void) {
    buy_me_a_coffee_screen = build_buy_me_a_coffee_screen();
    about_screen = build_about_screen();
    dev_options_screen = build_dev_options_screen();
    gui_accent_build_screens();
    gui_custom_font_build_screen();
    gui_display_timing_settings_build_screens();
    gui_volume_settings_build_startup_max_screens();
    sleep_timer_screen = build_sleep_timer_screen();
    idle_shutdown_screen = build_idle_shutdown_screen();
    gui_time_settings_build_screens();
    music_playback_screen = build_music_playback_screen();
    if (native_dac_controls_supported()) dac_settings_screen = build_dac_settings_screen();
    usb_dsd_settings_screen = build_usb_dsd_settings_screen();
    music_audio_screen = build_music_audio_screen();
    music_controls_screen = NULL; /* built on first open of Playback & Controls > Buttons & Remote */
    gui_volume_settings_build_car_mode_screen();
    animation_speed_screen = build_animation_speed_screen();
    keyboard_layout_screen = build_keyboard_layout_screen();
    player_layout_choice_screen = build_player_layout_choice_screen();
    language_choice_screen = build_language_choice_screen();
    settings_display_screen = build_settings_display_screen();
    settings_power_screen = build_settings_power_screen();
    settings_system_screen = build_settings_system_screen();
    settings_tools_screen = build_settings_tools_screen();
    settings_screen = build_settings_screen();
    gui_equalizer_build_screens(music_profiles_menu_cb, gui_settings_profiles_download_available);
    gui_updates_build_popups();
    gui_button_mapping_build_reset_popup();
    gui_equalizer_build_popups();
    build_factory_reset_popup();
    build_hostname_reboot_popup();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_settings_init() can rebuild them from a clean slate
 * without leaking the old objects. Does NOT touch build_home_screen()'s
 * result -- that's gui_shell.c's own static (home_screen), not this
 * module's, while Home's compatibility entry point lives here. The four
 * popup-and-backdrop pairs below are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so each needs its own explicit deletion. */
void gui_settings_teardown(void) {
    gui_updates_teardown();
    gui_button_mapping_teardown_reset_popup();
    gui_equalizer_teardown_popups();
    gui_popup_teardown(&factory_reset_popup);
    gui_popup_teardown(&hostname_reboot_popup);

    if (about_screen) { lv_obj_delete(about_screen); about_screen = NULL; }
    if (creator_help_menu_screen) { lv_obj_delete(creator_help_menu_screen); creator_help_menu_screen = NULL; }
    if (creator_plugin_help_screen) { lv_obj_delete(creator_plugin_help_screen); creator_plugin_help_screen = NULL; }
    if (creator_theme_help_screen) { lv_obj_delete(creator_theme_help_screen); creator_theme_help_screen = NULL; }
    if (buy_me_a_coffee_screen) { lv_obj_delete(buy_me_a_coffee_screen); buy_me_a_coffee_screen = NULL; }
    if (dev_options_screen) { lv_obj_delete(dev_options_screen); dev_options_screen = NULL; }
    adb_switch = NULL; /* owned by the screen just deleted; sync runs off the USB poll, not this screen's lifetime */
    gui_accent_teardown();
    gui_custom_font_teardown();
    gui_display_timing_settings_teardown();
    gui_volume_settings_teardown_startup_max_screens();
    if (sleep_timer_screen) { lv_obj_delete(sleep_timer_screen); sleep_timer_screen = NULL; }
    if (idle_shutdown_screen) { lv_obj_delete(idle_shutdown_screen); idle_shutdown_screen = NULL; }
    gui_time_settings_teardown();
    if (settings_library_screen) { lv_obj_delete(settings_library_screen); settings_library_screen = NULL; }
    if (settings_sorting_screen) { lv_obj_delete(settings_sorting_screen); settings_sorting_screen = NULL; }
    memset(settings_sorting_options, 0, sizeof(settings_sorting_options));
    if (settings_sound_effects_screen) { lv_obj_delete(settings_sound_effects_screen); settings_sound_effects_screen = NULL; }
    if (settings_eq_profiles_menu_screen) { lv_obj_delete(settings_eq_profiles_menu_screen); settings_eq_profiles_menu_screen = NULL; }
    if (settings_appearance_screen) { lv_obj_delete(settings_appearance_screen); settings_appearance_screen = NULL; }
    gui_shell_settings_teardown_order();
    gui_plugin_order_editor_teardown();
    gui_shell_settings_teardown_after_plugin_order();
    if (settings_player_layout_screen) { lv_obj_delete(settings_player_layout_screen); settings_player_layout_screen = NULL; }
    if (settings_gestures_screen) { lv_obj_delete(settings_gestures_screen); settings_gestures_screen = NULL; }
    if (settings_charging_screen) { lv_obj_delete(settings_charging_screen); settings_charging_screen = NULL; }
    settings_usb_dac_charging_switch = settings_usb_dac_charging_row = NULL;
    if (settings_maintenance_screen) { lv_obj_delete(settings_maintenance_screen); settings_maintenance_screen = NULL; }
    if (settings_system_maintenance_screen) { lv_obj_delete(settings_system_maintenance_screen); settings_system_maintenance_screen = NULL; }
    if (settings_system_updates_screen) { lv_obj_delete(settings_system_updates_screen); settings_system_updates_screen = NULL; }
    if (settings_tools_screen) { lv_obj_delete(settings_tools_screen); settings_tools_screen = NULL; }
    if (music_playback_screen) { lv_obj_delete(music_playback_screen); music_playback_screen = NULL; }
    if (music_audio_screen) { lv_obj_delete(music_audio_screen); music_audio_screen = NULL; }
    if (dac_settings_screen) { lv_obj_delete(dac_settings_screen); dac_settings_screen = NULL; }
    if (usb_dsd_settings_screen) { lv_obj_delete(usb_dsd_settings_screen); usb_dsd_settings_screen = NULL; }
    dac_settings_list = NULL;
    usb_dsd_settings_list = NULL;
    settings_eq_summary = settings_car_summary = settings_sleep_summary = NULL;
    gui_button_mapping_teardown_screens();
    if (music_controls_screen) { lv_obj_delete(music_controls_screen); music_controls_screen = NULL; }
    gui_volume_settings_teardown_car_mode_screen();
    if (animation_speed_screen) { lv_obj_delete(animation_speed_screen); animation_speed_screen = NULL; }
    animation_speed_list = NULL;
    if (keyboard_layout_screen) { lv_obj_delete(keyboard_layout_screen); keyboard_layout_screen = NULL; }
    keyboard_layout_list = NULL;
    if (player_layout_choice_screen) { lv_obj_delete(player_layout_choice_screen); player_layout_choice_screen = NULL; }
    player_layout_choice_list = NULL;
    if (language_choice_screen) { lv_obj_delete(language_choice_screen); language_choice_screen = NULL; }
    language_choice_list = NULL;
    if (settings_display_screen) { lv_obj_delete(settings_display_screen); settings_display_screen = NULL; }
    if (settings_power_screen) { lv_obj_delete(settings_power_screen); settings_power_screen = NULL; }
    if (settings_system_screen) { lv_obj_delete(settings_system_screen); settings_system_screen = NULL; }
    if (settings_screen) { lv_obj_delete(settings_screen); settings_screen = NULL; }
    gui_equalizer_teardown_screens();
}


/* Externs for callbacks defined in gui.c */

/* Extern widget var in gui.c */




lv_obj_t * gui_settings_get_screen(void) { return settings_screen; }
lv_obj_t * gui_settings_get_music_screen(void) { return music_playback_screen; }
lv_obj_t * gui_settings_get_display_screen(void) { return settings_display_screen; }
lv_obj_t * gui_settings_get_power_screen(void) { return settings_power_screen; }
lv_obj_t * gui_settings_get_system_screen(void) { return settings_system_screen; }
lv_obj_t * gui_settings_get_about_screen(void) { return about_screen; }
void gui_settings_sync_gapless_toggle(void) {
    if (!settings_gapless_toggle_img) return;
    if (current_settings.gapless_enabled) lv_obj_add_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
}

void gui_settings_sync_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    /* Real lv_switch now (see PILL_ACCESSORY_TOGGLE in screen_builders.c) --
     * CHECKED state alone drives its visual, no sprite swap needed. */
    if (current_settings.crossfade_enabled) lv_obj_add_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
}

/* Same bidirectional-sync role as gui_settings_sync_crossfade_toggle()
 * above, for the Sleep Timer screen's own enable switch -- called from
 * gui_shell.c whenever the drawer icon arms/disarms the timer, or the
 * countdown expires on its own, so this screen never shows a stale state
 * the next time it's opened. Also shows/hides the duration slider card and
 * the "Show Time Remaining" button, matching sleep_timer_switch_event_cb's
 * own behavior when the switch is toggled from this screen directly. */
void gui_settings_sync_sleep_timer_toggle(void) {
    if (!sleep_timer_switch) return;
    bool active = quick_drawer_sleep_timer_is_active();
    if (active) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(sleep_timer_switch, LV_STATE_CHECKED);
    if (!sleep_timer_slider_card) return;
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
