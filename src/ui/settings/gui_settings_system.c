#include "../application/gui.h"
#include "gui_settings.h"
#include "gui_about_info.h"
#include "../updates/gui_updates.h"
#include "controls/gui_time_settings.h"
#include "../text/i18n/i18n.h"
#include "../../hardware/input/hw_buttons.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../text/rendering/gui_text_input.h"
#include "../network/gui_network.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include "../widgets/builders/screen_builders.h"
#include "../../core/updates/firmware_update.h"
#include "../plugins/manage/gui_plugin_manage.h"
#include "../plugins/manage/gui_plugin_store.h"
#include "../shell/gui_reload.h"
#include "../../core/system/logging/db_log.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include <string.h>
#include "gui_settings_internal.h"

static lv_obj_t * settings_system_maintenance_screen;
static lv_obj_t * settings_system_updates_screen;
static lv_obj_t * settings_tools_screen;
static lv_obj_t * language_choice_screen;
static lv_obj_t * language_choice_list;
lv_obj_t * settings_system_screen;
static lv_obj_t * about_screen;
static lv_obj_t * creator_help_menu_screen;
static lv_obj_t * creator_plugin_help_screen;
static lv_obj_t * creator_theme_help_screen;
static lv_obj_t * buy_me_a_coffee_screen;
static lv_obj_t * dev_options_screen;

static lv_obj_t * adb_switch = NULL;

/* The Developer Options screen is built once and cached, so its ADB row would
 * otherwise keep showing whatever the state was at build time. Called when the
 * screen opens, and again by poll_usb_mode_switch() once the gadget settles so
 * a switch that failed does not leave the toggle promising ADB. */
void gui_settings_sync_adb_toggle(void) {
    if (!adb_switch) return;
    lv_obj_set_state(adb_switch, LV_STATE_CHECKED, gui_network_adb_active());
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

static const char DONATION_URL[] =
    "https://ko-fi.com/starnished";

static void creator_plugin_help_row_cb(lv_event_t * e);
static void creator_theme_help_row_cb(lv_event_t * e);

static void creator_help_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!creator_help_menu_screen) {
        pill_list_item_t items[2];
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

/* A square white quiet zone keeps the QR readable on every theme. */
static lv_obj_t * add_white_qr(lv_obj_t * parent, int32_t size, const char * url) {
    lv_obj_t * qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_qrcode_update(qr, url, strlen(url));
    return qr;
}

static lv_obj_t * build_buy_me_a_coffee_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Support Compás"), &title_label, &list);

    add_white_qr(list, BOARD_SCALE_PX(320), DONATION_URL);

    lv_obj_t * caption = lv_label_create(list);
    lv_label_set_text(caption, TR("Scan with your phone to support Compás Player on Ko-fi"));
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

    /* The list remains scrollable when the 720px panel cannot fit all guidance. */
    add_white_qr(list, BOARD_SCALE_PX(240), url);
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
            "https://compasplayer.org/plugins.html#quick-start",
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
            "https://compasplayer.org/themes.html#writing-a-theme",
            TR("Edit a .theme text file on your computer. Copy it to the Themes folder on your SD card, then select it in More > Themes."),
            TR("Scan for a starter theme and instructions. To redesign the player screen, see the XML layout guide linked there."));
    }
    nav_push(creator_theme_help_screen);
}

static lv_obj_t * build_about_screen(void) {
    pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ TR("Support Compás"), PILL_ACCESSORY_CHEVRON, false,
                                    push_screen_slot_cb, NULL, &buy_me_a_coffee_screen };
    items[1] = (pill_list_item_t){ TR("Create plugins & themes"), PILL_ACCESSORY_CHEVRON, false,
                                    creator_help_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("About"), generic_back_cb, items, 2, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    /* The scrolling list is the last child build_pill_list_screen() adds. */
    lv_obj_t * list = lv_obj_get_child(scr, lv_obj_get_child_count(scr) - 1);
    gui_about_info_attach(scr, list);
    finalize_screen_navigation(scr);
    return scr;
}

/* Writes a detailed timestamped log of library database scans and album art
 * cache jobs to .logs/database_artwork.log, and USB DAC bridge diagnostics to
 * .logs/usb_dac_bridge.log on the SD card -- see db_log.h and usb_dac_bridge.h.
 * Bluetooth messages go to database_artwork.log (area BT) and an HCI capture
 * to .logs/bluetooth.snoop -- see bluetooth_control.h. */
static void db_logging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.db_logging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    db_log_set_enabled(current_settings.db_logging_enabled);
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
    bt_control_set_debug_log_enabled(current_settings.db_logging_enabled);
}

static void screenshot_combo_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screenshot_combo_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
}

static void reboot_to_boot_selector_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char error[128] = {0};
    if (!idle_shutdown_reboot_to_bootloader(error, sizeof(error)))
        show_error_toast(error[0] ? error : TR("Could not restart into the boot selector"));
}

static const char * screenshot_row_label(void) {
#if defined(BOARD_R3II_2025)
    return TR("Screenshots (Power + Previous)");
#else
    return TR("Screenshots (Power + Vol Down)");
#endif
}
static lv_obj_t * build_dev_options_screen(void) {
    pill_list_item_t items[4];
    /* ADB lives here rather than on the USB Mode screen: it overrides
     * Storage/DAC while on and persists across a reboot, so it sits behind
     * Developer Options as the explicit opt-in that makes re-applying it on
     * startup reasonable. The USB Mode screen still dims the other modes
     * while it owns the port. */
    items[0] = (pill_list_item_t){ TR("ADB"), PILL_ACCESSORY_TOGGLE, gui_network_adb_active(),
                                    NULL, adb_switch_event_cb, NULL, &adb_switch };
    items[1] = (pill_list_item_t){ TR("Enable debug logging"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.db_logging_enabled, NULL, db_logging_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ screenshot_row_label(), PILL_ACCESSORY_TOGGLE,
                                    current_settings.screenshot_combo_enabled, NULL,
                                    screenshot_combo_switch_event_cb, NULL };
    items[3] = (pill_list_item_t){ TR("Reboot to bootloader"), PILL_ACCESSORY_NONE, false,
                                    reboot_to_boot_selector_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("Developer Options"), generic_back_cb, items, 4, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Defined later, alongside its confirmation popup (build_factory_reset_popup(),
 * a hand-built top-layer overlay). */
static void factory_reset_btn_cb(lv_event_t * e);

/* Shared click handler for every plugin-registered System list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
PLUGIN_ROW_CLICK_CB(plugin_system_list_item_click_cb, plugin_manager_system_list_item_clicked)

/* Defined later, alongside the reboot-confirm popup this opens into (see
 * hostname_entry_done_cb()) -- needed here first, for build_settings_
 * system_screen()'s own row list below. */
static void hostname_row_cb(lv_event_t * e);
/* Shared click handler for every plugin-registered Settings list row below
 * -- same index-not-object shape plugin_books_list_item_click_cb() already
 * uses for the Books screen. */
PLUGIN_ROW_CLICK_CB(plugin_settings_list_item_click_cb, plugin_manager_settings_list_item_clicked)

static lv_obj_t * build_settings_tools_screen(void) {
    pill_list_item_t items[PLUGIN_MAX_SETTINGS_LIST_ITEMS];
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
    push_cached_screen(&settings_system_maintenance_screen, build_settings_maintenance_screen);
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

void settings_updates_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_system_updates_screen, build_settings_updates_screen);
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
    pill_list_item_t items[8 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("USB Mode"), PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Clock"), PILL_ACCESSORY_CHEVRON, false, gui_time_settings_clock_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Language"), PILL_ACCESSORY_CHEVRON, false, language_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Plugin Manager"), PILL_ACCESSORY_CHEVRON, false, gui_plugin_manage_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Maintenance"), PILL_ACCESSORY_CHEVRON, false, settings_maintenance_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("Developer Options"), PILL_ACCESSORY_CHEVRON, false,
                                    dev_options_row_cb, NULL, NULL };
    items[6] = (pill_list_item_t){ TR("About"), PILL_ACCESSORY_CHEVRON, false, push_screen_slot_cb, NULL, &about_screen };
    int count = 7;
    if (plugin_manager_get_settings_list_item_count() > 0) {
        items[count++] = (pill_list_item_t){ TR("Additional Tools"), PILL_ACCESSORY_CHEVRON, false,
                                             push_screen_slot_cb, NULL, &settings_tools_screen };
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

/* ---- Factory Reset, with a confirmation popup -- same hand-built
 * top-layer overlay shape as the other confirmation popups (this codebase
 * doesn't use LVGL's lv_msgbox anywhere). Wiping every app setting is
 * exactly the kind of thing a stray tap must never be able to trigger.
 * Confirmation deletes configuration and reboots; the device path does not
 * return to the old UI after a destructive reset. ---- */
static gui_popup_t factory_reset_popup;

/* True (after a toast) while a firmware update runs: the update owns the
 * device and a reboot would interrupt it. */
static bool reboot_blocked_by_update(void) {
    if (!firmware_update_busy()) return false;
    show_error_toast(TR("An update is already in progress"));
    return true;
}

static void factory_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&factory_reset_popup);
}

static void factory_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&factory_reset_popup);
    if (reboot_blocked_by_update()) return;
    settings_factory_reset(); /* deletes the settings file and reboots -- see its own comment in settings.h/.c */
}

static void factory_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&factory_reset_popup);
}

void build_factory_reset_popup(void) {
    factory_reset_popup.popup = build_confirm_popup(
        TR("Reset all settings and reboot?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Reset"), lv_color_hex(GUI_BREEZE_NEGATIVE),
        factory_reset_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), factory_reset_cancel_cb, NULL,
        factory_reset_cancel_cb, &factory_reset_popup.backdrop);
}

/* Settings -> System -> Hostname uses the shared confirmation-popup helper;
 * see hostname_apply()'s own comment for why a reboot is genuinely required
 * here (wifi_on.sh/bt_init each only read their file once, on demand). */
static gui_popup_t hostname_reboot_popup;

static void hostname_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&hostname_reboot_popup);
}

static void hostname_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&hostname_reboot_popup);
    if (reboot_blocked_by_update()) return;
    idle_shutdown_reboot_now();
}

void build_hostname_reboot_popup(void) {
    hostname_reboot_popup.popup = build_confirm_popup(
        TR("Restart now to apply the new hostname?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Restart Now"), accent_lv_color(),
        hostname_reboot_now_cb, NULL, TR("Later"), lv_color_make(160, 160, 160), hostname_reboot_later_cb, NULL,
        hostname_reboot_later_cb, &hostname_reboot_popup.backdrop);
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
    gui_popup_show(&hostname_reboot_popup);
}

static void hostname_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Hostname"), current_settings.hostname, false, false, hostname_entry_done_cb, NULL);
}

void settings_system_teardown_popups(void) {
    gui_popup_teardown(&factory_reset_popup);
    gui_popup_teardown(&hostname_reboot_popup);
}

void settings_system_teardown_screens(void) {
    delete_screen(&about_screen);
    delete_screen(&creator_help_menu_screen);
    delete_screen(&creator_plugin_help_screen);
    delete_screen(&creator_theme_help_screen);
    delete_screen(&buy_me_a_coffee_screen);
    delete_screen(&dev_options_screen);
    adb_switch = NULL; /* owned by the screen just deleted; sync runs off the USB poll, not this screen's lifetime */
    delete_screen(&settings_system_maintenance_screen);
    delete_screen(&settings_system_updates_screen);
    delete_screen(&settings_tools_screen);
    delete_screen(&language_choice_screen);
    language_choice_list = NULL;
    delete_screen(&settings_system_screen);
}

lv_obj_t * gui_settings_get_system_screen(void) { return settings_system_screen; }
lv_obj_t * gui_settings_get_about_screen(void) { return about_screen; }

/* Per-group screen construction, called from gui_settings_init(). Slots are
 * private to each group, so the group that owns a screen also builds it. */
void settings_system_build_screens(void) {
    buy_me_a_coffee_screen = build_buy_me_a_coffee_screen();
    about_screen = build_about_screen();
    dev_options_screen = build_dev_options_screen();
    language_choice_screen = build_language_choice_screen();
    settings_system_screen = build_settings_system_screen();
    if (plugin_manager_get_settings_list_item_count() > 0)
        settings_tools_screen = build_settings_tools_screen();
}
