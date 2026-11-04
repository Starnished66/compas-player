#include "../application/gui.h"
#include "gui_settings.h"
#include "input/gui_button_mapping.h"
#include "../player/settings/gui_equalizer.h"
#include "../home/gui_home.h"
#include "../updates/gui_updates.h"
#include "../appearance/gui_accent.h"
#include "../appearance/fonts/gui_custom_font.h"
#include "display/gui_display_timing_settings.h"
#include "controls/gui_time_settings.h"
#include "controls/gui_shell_settings.h"
#include "controls/gui_volume_settings.h"
#include "../text/i18n/i18n.h"
#include "../network/gui_network.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../plugins/manage/gui_plugin_manage.h"
#include <string.h>
#include <unistd.h>
#include "../library/browse/gui_file_manager.h"
#include "../plugins/emulators/gui_emulators.h"
#include "../plugins/apps/gui_apps.h"
#include "gui_settings_internal.h"

static lv_obj_t * settings_screen;
lv_obj_t * settings_eq_summary;
lv_obj_t * settings_car_summary;
lv_obj_t * settings_sleep_summary;

void settings_summary_loaded_cb(lv_event_t * e) {
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

/* Row click handler that opens the screen stored in the lv_obj_t * slot its
 * user_data points at. The slot is read at click time because the screens
 * are rebuilt on every UI reload. */
void push_screen_slot_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t * screen = *(lv_obj_t **) lv_event_get_user_data(e);
    if (screen) nav_push(screen);
}

/* Not declared in any header: defined in gui.c. */
extern lv_obj_t * stream_media_screen;

/* Called by gui_theme.c after every accent change, including a new cover
 * while "Match album art" is on. */
void gui_settings_accent_changed(void) {
    gui_accent_sync();
    gui_equalizer_accent_changed();
    gui_library_accent_changed();
}

/* Filter plugin rows into the new settings groups while preserving the
 * original plugin index used by each click handler. Unknown groups stay in
 * the direct list so older/future plugins remain reachable. */
int append_grouped_plugin_rows(pill_list_item_t * items, int count, int max_items,
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

int count_plugin_rows_in_group(const char * list_id, int plugin_count, const char * group) {
    int count = 0;
    for (int i = 0; i < plugin_count; ++i) {
        const char * candidate = plugin_manager_get_list_item_group(list_id, i);
        if (candidate && strcmp(candidate, group) == 0) ++count;
    }
    return count;
}

static void settings_category_music_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_sound();
}

static void settings_category_playback_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_playback();
}

static void settings_category_library_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_library();
}


static lv_obj_t * build_settings_screen(void) {
    pill_list_item_t items[7];
    lv_obj_t * category_rows[7] = { NULL };
    items[0] = (pill_list_item_t){ TR("Sound"), PILL_ACCESSORY_CHEVRON, false, settings_category_music_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Playback & Controls"), PILL_ACCESSORY_CHEVRON, false, settings_category_playback_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Display"), PILL_ACCESSORY_CHEVRON, false, push_screen_slot_cb, NULL, &settings_display_screen };
    items[3] = (pill_list_item_t){ TR("Power"), PILL_ACCESSORY_CHEVRON, false, push_screen_slot_cb, NULL, &settings_power_screen };
    items[4] = (pill_list_item_t){ TR("Library"), PILL_ACCESSORY_CHEVRON, false, settings_category_library_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("System"), PILL_ACCESSORY_CHEVRON, false, push_screen_slot_cb, NULL, &settings_system_screen };
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
        if (category_rows[i]) decorate_category_row(category_rows[i], icons[i]);
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

PLUGIN_ROW_CLICK_CB(plugin_more_list_item_click_cb, plugin_manager_more_list_item_clicked)

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
    { "breeze_rows/submenu_external_apps.png", N_("External Apps"), gui_apps_row_cb },
};
enum { NATIVE_COUNT = (int) (sizeof(native) / sizeof(native[0])) };

/* Breeze art may be missing on older firmware assets; External Apps then
 * keeps the settings gear it used before. */
static const char * more_native_icon(int index) {
    const char * icon = native[index].icon;
    const char * breeze = strncmp(icon, "breeze_rows/", 12) == 0 ? asset_path_plain(icon) : NULL;
    if (breeze && access(breeze, R_OK) != 0) icon = "settings/system.png";
    return icon;
}

static void decorate_more_rows(lv_obj_t ** rows, int count, const char ** plugin_icons) {
    for (int i = 0; i < count; ++i) {
        if (rows[i]) {
            if (i < NATIVE_COUNT) decorate_category_row(rows[i], more_native_icon(i));
            else decorate_category_row_plugin_icon(rows[i], plugin_icons[i - NATIVE_COUNT]);
        }
    }
}

/* Native entries use the shared category look (decorate_category_row());
 * plugin rows registered for "more" follow them with the same look, their
 * icon resolved like every other plugin row icon. The screen is rebuilt on
 * every plugin reload (gui_shell_build_screens()), so rows track the
 * currently loaded plugins. */
lv_obj_t * build_dac_home_screen(void) {
    pill_list_item_t items[NATIVE_COUNT + PLUGIN_MAX_MORE_LIST_ITEMS];
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
    decorate_more_rows(rows, count, plugin_icons);
    finalize_screen_navigation(scr);
    return scr;
}

lv_obj_t * build_home_screen(void) {
    return gui_home_build_screen(settings_tile_cb, stream_media_tile_cb);
}

void gui_settings_init(void) {
    gui_accent_build_screens();
    gui_custom_font_build_screen();
    gui_display_timing_settings_build_screens();
    gui_volume_settings_build_startup_max_screens();
    gui_time_settings_build_screens();
    settings_system_build_screens();
    settings_power_build_screens();
    settings_music_build_screens();
    settings_display_build_screens();
    gui_volume_settings_build_car_mode_screen();
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
    settings_system_teardown_popups();

    gui_accent_teardown();
    gui_custom_font_teardown();
    gui_display_timing_settings_teardown();
    gui_volume_settings_teardown_startup_max_screens();
    gui_time_settings_teardown();
    gui_shell_settings_teardown_order();
    gui_plugin_order_editor_teardown();
    gui_shell_settings_teardown_after_plugin_order();
    /* The summary labels live on screens deleted below; drop them first. The
     * Buttons & Remote label is dropped by gui_button_mapping_teardown_screens(),
     * which must run before that screen is deleted. */
    settings_eq_summary = settings_car_summary = settings_sleep_summary = NULL;
    gui_button_mapping_teardown_screens();
    gui_volume_settings_teardown_car_mode_screen();
    settings_music_teardown_screens();
    settings_display_teardown_screens();
    settings_power_teardown_screens();
    settings_system_teardown_screens();
    delete_screen(&settings_screen);
    gui_equalizer_teardown_screens();
}

lv_obj_t * gui_settings_get_screen(void) { return settings_screen; }
