#include "gui_setup.h"

#include "gui.h"
#include "gui_navigation.h"
#include "gui_reload.h"
#include "gui_network.h"
#include "gui_plugin_store.h"
#include "gui_settings.h"
#include "gui_setup_plugins.h"
#include "gui_notifications.h"
#include "gui_theme.h"
#include "gui_shell.h"
#include "player_layouts.h"
#include "usb_mode_control.h"
#include "i18n.h"
#include "screen_builders.h"
#include "settings.h"
#include "timezone_location.h"
#include "setup_world_map_data.h"
#include "gui_library.h"
#include "db_log.h"
#include "lvgl/src/draw/snapshot/lv_snapshot.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

extern player_settings_t current_settings;
/* Keep persisted IDs stable; Layout is appended as ID 7. Display order is
 * separate because Layout now sits between Plugins and Scan. */
enum {
    STEP_WELCOME = 0, STEP_LANGUAGE = 1, STEP_TIMEZONE = 2, STEP_WIFI = 3,
    STEP_PLUGINS = 4, STEP_SCAN = 5, STEP_COMPLETE = 6, STEP_LAYOUT = 7
};
static const int setup_step_order[] = {
    STEP_WELCOME, STEP_LANGUAGE, STEP_TIMEZONE, STEP_WIFI,
    STEP_PLUGINS, STEP_LAYOUT, STEP_SCAN, STEP_COMPLETE
};

static int setup_step_position(int value) {
    for (size_t i = 0; i < sizeof(setup_step_order) / sizeof(setup_step_order[0]); ++i)
        if (setup_step_order[i] == value) return (int)i;
    return -1;
}

static int setup_step_at_position(int position) {
    const int count = (int)(sizeof(setup_step_order) / sizeof(setup_step_order[0]));
    if (position < 0) position = 0;
    if (position >= count) position = count - 1;
    return setup_step_order[position];
}
static lv_obj_t * setup_screen;
static lv_obj_t * content;
static lv_obj_t * setup_body;
static lv_obj_t * scan_switch;
static gui_popup_t language_popup;
static lv_obj_t * language_list;
static int step;
static char staged_language[8];
static bool staged_scan;
static bool setup_scan_running;
static bool setup_finalizing;
static bool setup_apply_pending;
static lv_timer_t * setup_apply_timer;
static bool setup_storage_held;
static bool setup_storage_release;
static bool setup_storage_done;
static bool setup_storage_success;
static bool setup_storage_running;
static bool setup_storage_release_pending;
static pthread_t setup_storage_thread;
static pthread_mutex_t setup_storage_mutex = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t * setup_storage_timer;
static void (*setup_storage_complete)(bool);
static bool setup_scan_success;
static char staged_layout_plugin[64];
static char staged_layout_name[65];
static lv_timer_t * layout_catalog_timer;
static bool layout_catalog_ready;
static uint64_t layout_preview_generation;
static char layout_suggestion_preview[512];
static bool setup_started;
static lv_timer_t * plugin_network_timer;
static lv_obj_t * plugin_network_notice;
static lv_obj_t * plugin_catalog_notice;
static lv_obj_t * plugin_download_rows[3];
static bool plugin_download_available[3];
static const char * const suggested_plugin_ids[] = { "example.gain_mode", "compas.autoeq" };
static bool plugin_network_connected;
static bool plugin_catalog_rendered;
static bool plugin_catalog_request_started;
static void style_plugin_selection(lv_obj_t * row, const char * id);

/* Cache the welcome page while the intro paints: the fade only redraws one
 * opaque image, the title and a small number of notes, not the whole widget tree. */
#define INTRO_NOTE_COUNT 18
static lv_obj_t * intro_overlay;
static lv_obj_t * intro_background;
static lv_obj_t * intro_title;
static lv_obj_t * intro_notes[INTRO_NOTE_COUNT];
static lv_draw_buf_t * intro_frame;
static void intro_cleanup(void);
static void intro_exec(void * obj, int32_t progress);

static void render_async(void * unused);
static void finish_async(void * unused);
static lv_color_t style_color(lv_style_t * style, lv_style_prop_t prop, uint32_t fallback);

static void no_scrollbar(lv_obj_t * obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

bool gui_setup_is_active(void) {
    return setup_screen && lv_screen_active() == setup_screen;
}

lv_obj_t * gui_setup_get_screen(void) { return setup_screen; }

static lv_obj_t * label(lv_obj_t * parent, const char * text, gui_font_role_t role,
                        lv_color_t color, int32_t width) {
    lv_obj_t * obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, gui_theme_font(role), 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    no_scrollbar(obj);
    return obj;
}

static lv_obj_t * button(lv_obj_t * parent, const char * text, bool primary, lv_event_cb_t cb) {
    lv_obj_t * obj = lv_button_create(parent);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, BOARD_SCALE_PX(primary ? 64 : 56));
    lv_obj_set_style_radius(obj, BOARD_SCALE_PX(18), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, primary ? accent_lv_color() : lv_color_hex(GUI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    if (!primary) {
        lv_obj_set_style_border_width(obj, BOARD_SCALE_PX(1), 0);
        lv_obj_set_style_border_color(obj, lv_color_mix(accent_lv_color(), lv_color_hex(GUI_COLOR_PANEL), 40), 0);
    }
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL);
    lv_color_t primary_text = lv_color_brightness(accent_lv_color()) > 160
        ? lv_color_hex(0x14170B) : lv_color_white();
    lv_obj_t * caption = lv_label_create(obj);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_font(caption, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_text_color(caption, primary ? primary_text :
        style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xDDE1E7), 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_SCROLLABLE);
    if (primary) {
        lv_obj_t * arrow = lv_label_create(obj);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
        lv_obj_set_style_text_color(arrow, primary_text, 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(20), 0);
        lv_obj_remove_flag(arrow, LV_OBJ_FLAG_SCROLLABLE);
    }
    return obj;
}

static void schedule_render(void) {
    lv_async_call_cancel(render_async, NULL);
    lv_async_call(render_async, NULL);
}
static void save_progress_from(const char * source) {
    current_settings.setup_step = step;
    settings_save(&current_settings);
    DB_LOG("SETUP_NAV", "save source=%s step=%d setup_complete=%d setup_started=%d scan_running=%d",
           source ? source : "unknown", step, current_settings.setup_complete,
           setup_started, setup_scan_running);
    (void)db_log_flush();
}
static void loaded_cb(lv_event_t * e) {
    (void)e;
    if (!intro_overlay) schedule_render();
}
static bool installation_blocks_navigation(void) {
    if (setup_scan_running) {
        show_info_toast(TR("Please wait for the library scan to finish"));
        return true;
    }
    if (!gui_setup_plugins_busy() && !setup_finalizing) return false;
    show_info_toast(TR("Please wait for plugin installation to finish"));
    return true;
}
static void advance_step(const char * source) {
    int previous_step = step;
    int position = setup_step_position(step);
    if (position >= 0) step = setup_step_at_position(position + 1);
    if (step == STEP_TIMEZONE || step == STEP_WIFI) gui_network_setup_wifi_prepare();
    DB_LOG("SETUP_NAV", "advance source=%s before=%d after=%d", source ? source : "unknown",
           previous_step, step);
    save_progress_from("advance");
    schedule_render();
}
static void skip_cb(lv_event_t * e) {
    (void)e;
    if (installation_blocks_navigation()) return;
    if (step == STEP_PLUGINS) gui_setup_plugins_clear_selection();
    advance_step("skip");
}
static void next_cb(lv_event_t * e) {
    (void)e;
    if (installation_blocks_navigation()) return;
    if (step == STEP_WIFI && !gui_network_setup_wifi_connected()) {
        show_error_toast(TR("Connect to a Wi-Fi network before continuing."));
        return;
    }
    if (step == STEP_PLUGINS) {
        if (gui_setup_plugins_selected_count() == 0) {
            show_info_toast(TR("Select at least one plugin to continue."));
            return;
        }
    }
    if (step == STEP_LANGUAGE) {
        if (strcmp(staged_language, current_settings.language) != 0) {
            snprintf(current_settings.language, sizeof(current_settings.language), "%s", staged_language);
            i18n_set_language(staged_language);
            step = setup_step_at_position(setup_step_position(step) + 1);
            save_progress_from("language-next");
            gui_reload_request();
            return;
        }
    }
    advance_step("continue");
}
static void back_cb(lv_event_t * e) {
    lv_indev_t * indev = lv_indev_active();
    lv_point_t point = {0, 0};
    if (indev) lv_indev_get_point(indev, &point);
    lv_obj_t * target = e ? lv_event_get_target(e) : NULL;
    DB_LOG("SETUP_NAV", "back event step=%d target=%p target_valid=%d indev=%p point=%d,%d",
           step, (void *)target, target && lv_obj_is_valid(target), (void *)indev, point.x, point.y);
    (void)db_log_flush();
    if (installation_blocks_navigation()) return;
    int position = setup_step_position(step);
    if (position > 0) step = setup_step_at_position(position - 1);
    save_progress_from("back");
    schedule_render();
}
static void * setup_storage_worker(void * unused) {
    (void)unused;
    bool success = true;
    if (setup_storage_release) usb_mode_control_storage_write_end();
    else success = usb_mode_control_storage_write_begin();
    pthread_mutex_lock(&setup_storage_mutex);
    setup_storage_success = success;
    setup_storage_done = true;
    pthread_mutex_unlock(&setup_storage_mutex);
    return NULL;
}
static bool setup_storage_job(bool release, void (*complete)(bool));
static void setup_storage_finished(bool success);
static void setup_storage_poll(lv_timer_t * timer) {
    (void)timer;
    if (!setup_storage_running) {
        if (setup_storage_release_pending && setup_storage_job(true, setup_storage_finished))
            setup_storage_release_pending = false;
        return;
    }
    pthread_mutex_lock(&setup_storage_mutex);
    bool done = setup_storage_done, success = setup_storage_success;
    pthread_mutex_unlock(&setup_storage_mutex);
    if (!done) return;
    pthread_join(setup_storage_thread, NULL);
    setup_storage_running = false;
    setup_storage_held = !setup_storage_release && success;
    void (*complete)(bool) = setup_storage_complete;
    setup_storage_complete = NULL;
    if (complete) complete(success);
    if (!setup_storage_held && !setup_storage_running && !setup_storage_release_pending && setup_storage_timer) {
        lv_timer_delete(setup_storage_timer);
        setup_storage_timer = NULL;
    }
}
static bool setup_storage_job(bool release, void (*complete)(bool)) {
    if (setup_storage_running) return false;
    if (!setup_storage_timer) setup_storage_timer = lv_timer_create(setup_storage_poll, 50, NULL);
    if (!setup_storage_timer) return false;
    setup_storage_release = release;
    setup_storage_done = false;
    setup_storage_complete = complete;
    if (pthread_create(&setup_storage_thread, NULL, setup_storage_worker, NULL) != 0) {
        setup_storage_complete = NULL;
        return false;
    }
    setup_storage_running = true;
    return true;
}
static void setup_storage_finished(bool success) {
    (void)success;
    setup_finalizing = false;
    if (!setup_screen) return;
    if (setup_scan_success) advance_step("configuration-applied");
    else schedule_render();
}
static void setup_release_storage(bool success) {
    setup_scan_success = success;
    if (!setup_storage_held) { setup_storage_finished(true); return; }
    if (!setup_storage_job(true, setup_storage_finished)) {
        /* Retry off the UI thread while retaining exclusive card access. */
        setup_storage_release_pending = true;
        show_error_toast(TR("Could not start the plugin operation"));
    }
}
static void setup_scan_completed(bool success) {
    setup_scan_running = false;
    DB_LOG("SETUP_NAV", "scan callback success=%d step=%d", success, step);
    (void)db_log_flush();
    setup_release_storage(success);
}
static void start_setup_scan_async(void * unused) {
    (void)unused;
    if (!setup_screen || step != STEP_SCAN) return;
    char error[256];
    if (!plugin_store_storage_writable(error, sizeof(error))) {
        setup_scan_running = false;
        show_error_toast(error);
        setup_release_storage(false);
        return;
    }
    if (!gui_library_start_setup_scan(setup_scan_completed)) {
        setup_scan_running = false;
        show_error_toast(TR("Could not start the library scan. Please try again."));
        setup_release_storage(false);
    }
}
static void setup_apply_cb(lv_timer_t * timer) {
    (void)timer;
    if (setup_apply_pending && setup_screen) gui_reload_request();
}
static void setup_batch_finished(void) {
    if (!setup_screen || step != STEP_SCAN) return;
    char layout_id[PLAYER_LAYOUT_ID_MAX] = "";
    if (staged_layout_plugin[0] && gui_setup_plugins_layout_ready()) {
        if (!plugin_store_get_player_layout_id(staged_layout_plugin, layout_id, sizeof(layout_id)))
            show_error_toast(TR("Plugin is unavailable in the catalog"));
    }
    player_layouts_session_clear_selection();
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s", layout_id);
    settings_save(&current_settings);
    /* Load newly installed plugin registrations before the library scan.
     * The setup state survives the UI reload; after_reload resumes the job. */
    setup_apply_pending = true;
    /* Respect the reload cooldown even when an empty batch completes just
     * after a language reload. Retry until teardown acknowledges the reload. */
    setup_apply_timer = lv_timer_create(setup_apply_cb, 300, NULL);
    if (!setup_apply_timer) {
        setup_apply_pending = false;
        show_error_toast(TR("Could not start the library scan. Please try again."));
        setup_release_storage(false);
    }
}
static void setup_storage_acquired(bool success) {
    if (!success) {
        setup_finalizing = false;
        show_error_toast(TR("Disconnect USB storage from the host before changing plugins."));
        return;
    }
    if (!gui_setup_plugins_start(setup_batch_finished)) setup_release_storage(false);
}
static void finish_cb(lv_event_t * e) {
    (void)e;
    if (installation_blocks_navigation()) return;
    if (scan_switch) staged_scan = lv_obj_has_state(scan_switch, LV_STATE_CHECKED);
    if (staged_scan && !sd_card_root_is_mounted()) {
        show_error_toast(TR("Insert an SD card to scan for music, or turn off Scan for music."));
        return;
    }
    if (staged_scan || staged_layout_plugin[0] || gui_setup_plugins_selected_count() > 0) {
        if (!sd_card_root_is_mounted()) {
            show_error_toast(TR("Insert an SD card to change plugins."));
            return;
        }
        char error[256];
        if (!plugin_store_storage_writable(error, sizeof(error))) {
            show_error_toast(error);
            return;
        }
    }
    setup_finalizing = true;
    if (!staged_scan && !staged_layout_plugin[0] && gui_setup_plugins_selected_count() == 0) {
        if (!gui_setup_plugins_start(setup_batch_finished)) setup_finalizing = false;
        return;
    }
    if (!setup_storage_job(false, setup_storage_acquired)) {
        setup_finalizing = false;
        show_error_toast(TR("Could not start the plugin operation"));
    }
}
static void complete_cb(lv_event_t * e) {
    (void)e;
    lv_async_call(finish_async, NULL);
}
static void language_popup_close(lv_event_t * e) {
    (void)e;
    gui_popup_hide(&language_popup);
}
static void language_select_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);
    const char * code = i18n_language_code(index);
    if (!code) return;
    gui_popup_hide(&language_popup);
    if (strcmp(code, current_settings.language) == 0) return;
    snprintf(staged_language, sizeof(staged_language), "%s", code);
    snprintf(current_settings.language, sizeof(current_settings.language), "%s", code);
    current_settings.setup_step = step;
    settings_save(&current_settings);
    i18n_set_language(code);
    DB_LOG("SETUP_NAV", "language changed step=%d persisted_step=%d", step,
           current_settings.setup_step);
    (void)db_log_flush();
    gui_reload_request();
}
static const char * selected_language_name(void) {
    for (size_t i = 0; i < i18n_language_count(); ++i)
        if (strcmp(i18n_language_code(i), staged_language) == 0) return i18n_language_name(i);
    return i18n_language_name(0);
}
static void language_open_cb(lv_event_t * e) {
    (void)e;
    if (!language_popup.popup) {
        language_popup.popup = build_popup_surface(language_popup_close, &language_popup.backdrop);
        lv_obj_t * title = lv_label_create(language_popup.popup);
        lv_label_set_text(title, TR("Choose a language"));
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_add_style(title, &style_theme_text_primary, 0);
        language_list = lv_obj_create(language_popup.popup);
        lv_obj_set_width(language_list, LV_PCT(100));
        int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
        int32_t row_h = BOARD_SCALE_PX(64);
        if (row_h < 44) row_h = 44;
        int32_t list_h = (int32_t)i18n_language_count() * row_h +
                          (int32_t)(i18n_language_count() - 1) * BOARD_SCALE_PX(6);
        int32_t available_h = screen_h - BOARD_SCALE_PX(184) - 2 * STATUS_BAR_CLEARANCE;
        if (list_h > available_h) list_h = available_h;
        if (list_h < BOARD_SCALE_PX(128)) list_h = BOARD_SCALE_PX(128);
        lv_obj_set_height(language_list, list_h);
        lv_obj_set_style_bg_opa(language_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(language_list, 0, 0);
        lv_obj_set_style_pad_all(language_list, 0, 0);
        lv_obj_set_style_pad_row(language_list, BOARD_SCALE_PX(6), 0);
        lv_obj_set_flex_flow(language_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(language_list, LV_DIR_VER);
        lv_obj_add_flag(language_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(language_list, LV_SCROLLBAR_MODE_OFF);
        for (size_t i = 0; i < i18n_language_count(); ++i) {
            lv_obj_t * row = lv_button_create(language_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, row_h);
            lv_obj_set_style_radius(row, BOARD_SCALE_PX(14), 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_bg_color(row, strcmp(i18n_language_code(i), staged_language) == 0
                ? lv_color_mix(accent_lv_color(), lv_color_hex(GUI_COLOR_PANEL), 36)
                : style_color(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_ROW), 0);
            lv_obj_set_style_border_width(row, BOARD_SCALE_PX(1), 0);
            lv_obj_set_style_border_color(row, strcmp(i18n_language_code(i), staged_language) == 0
                ? accent_lv_color() : style_color(&style_theme_card_bg, LV_STYLE_BG_COLOR, GUI_COLOR_BORDER), 0);
            lv_obj_add_event_cb(row, language_select_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t * name = lv_label_create(row);
            lv_label_set_text(name, i18n_language_name(i));
            lv_obj_set_width(name, LV_PCT(100));
            lv_obj_set_style_text_font(name, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
            lv_obj_set_style_text_color(name, style_color(&style_theme_text_primary,
                LV_STYLE_TEXT_COLOR, 0xFFFFFF), 0);
            lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_left(row, BOARD_SCALE_PX(16), 0);
            lv_obj_set_style_pad_right(row, BOARD_SCALE_PX(16), 0);
        }
        lv_obj_t * cancel = lv_button_create(language_popup.popup);
        lv_obj_set_width(cancel, LV_PCT(100));
        int32_t cancel_h = BOARD_SCALE_PX(64);
        if (cancel_h < 44) cancel_h = 44;
        lv_obj_set_height(cancel, cancel_h);
        lv_obj_set_style_radius(cancel, BOARD_SCALE_PX(14), 0);
        lv_obj_set_style_shadow_width(cancel, 0, 0);
        lv_obj_set_style_bg_color(cancel, lv_color_hex(0x8B3030), 0);
        lv_obj_add_event_cb(cancel, language_popup_close, LV_EVENT_CLICKED, NULL);
        lv_obj_t * cancel_text = lv_label_create(cancel);
        lv_label_set_text(cancel_text, TR("Cancel"));
        lv_obj_set_style_text_font(cancel_text, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_style_text_color(cancel_text, lv_color_white(), 0);
        lv_obj_center(cancel_text);
        no_scrollbar(cancel_text);
    }
    gui_popup_show(&language_popup);
}
static void open_timezone_cb(lv_event_t * e) { (void)e; gui_setup_open_timezone(); }
static void wifi_hidden_cb(lv_event_t * e) { (void)e; gui_network_setup_wifi_add_hidden(); }
static void wifi_rescan_cb(lv_event_t * e) { (void)e; gui_network_setup_wifi_rescan(); }
static void plugin_network_refresh(void) {
    if (!plugin_network_notice) return;
    bool connected = gui_network_setup_wifi_connected();
    if (connected == plugin_network_connected) return;
    plugin_network_connected = connected;
    if (connected && !gui_plugin_store_setup_catalog_ready()) plugin_catalog_request_started = false;
    lv_obj_set_flag(plugin_network_notice, LV_OBJ_FLAG_HIDDEN, connected);
    size_t setup_count = sizeof(suggested_plugin_ids) / sizeof(suggested_plugin_ids[0]);
    for (size_t i = 0; i <= setup_count; ++i) {
        if (!plugin_download_rows[i]) continue;
        bool is_more = (i == setup_count);
        bool enabled = connected && (is_more || plugin_download_available[i]);
        lv_obj_set_style_opa(plugin_download_rows[i], enabled ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_flag(plugin_download_rows[i], LV_OBJ_FLAG_CLICKABLE, enabled);
        lv_obj_set_state(plugin_download_rows[i], LV_STATE_DISABLED, !enabled);
        if (!is_more) {
            lv_obj_t * check = lv_obj_get_child(plugin_download_rows[i], lv_obj_get_child_count(plugin_download_rows[i]) - 1);
            lv_obj_set_state(check, LV_STATE_DISABLED, !enabled);
            lv_obj_set_flag(check, LV_OBJ_FLAG_CLICKABLE, enabled);
        }
    }
}
static void connect_plugins_wifi_cb(lv_event_t * e) {
    (void)e;
    step = STEP_WIFI;
    gui_network_setup_wifi_prepare();
    save_progress_from("return-wifi");
    schedule_render();
}
static void plugin_network_poll(lv_timer_t * timer) {
    (void)timer;
    if (step != STEP_PLUGINS) return;
    plugin_network_refresh();
    if (gui_setup_plugins_busy()) return;
    if (plugin_catalog_notice && gui_network_setup_wifi_connected()) {
        plugin_store_status_t status;
        plugin_store_get_status(&status, NULL, 0);
        lv_label_set_text(plugin_catalog_notice, status.state == PLUGIN_STORE_FAILED
            ? TR("Could not load the plugin catalog. Tap More to retry.")
            : TR("Loading plugin catalog..."));
    }
    if (gui_network_setup_wifi_connected() && !plugin_catalog_request_started)
        plugin_catalog_request_started = gui_plugin_store_setup_catalog_prepare();
    if (plugin_catalog_rendered != gui_plugin_store_setup_catalog_ready()) {
        schedule_render();
        return;
    }
    size_t setup_count = sizeof(suggested_plugin_ids) / sizeof(suggested_plugin_ids[0]);
    for (size_t i = 0; i < setup_count; ++i) {
        lv_obj_t * row = plugin_download_rows[i];
        if (!row) continue;
        const char * id = suggested_plugin_ids[i];
        lv_obj_t * check = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
        if (lv_obj_has_state(check, LV_STATE_CHECKED) != gui_setup_plugins_is_selected(id))
            style_plugin_selection(row, id);
    }
}
static void setup_plugin_picker_done(void) {
    if (step == STEP_PLUGINS && !installation_blocks_navigation()) next_cb(NULL);
}
static void open_plugins_cb(lv_event_t * e) {
    (void)e;
    (void)gui_plugin_store_open_picker(gui_setup_plugins_is_selected, gui_setup_plugins_toggle,
                                     setup_plugin_picker_done);
}
static bool setup_layout_is_selected(const char * id) {
    return id && strcmp(id, staged_layout_plugin) == 0;
}
static void setup_layout_selected(const char * id, const char * name) {
    if (installation_blocks_navigation() || !id) return;
    snprintf(staged_layout_plugin, sizeof(staged_layout_plugin), "%s", id);
    snprintf(staged_layout_name, sizeof(staged_layout_name), "%s", name ? name : "");
    gui_setup_plugins_set_layout(staged_layout_plugin, staged_layout_name);
    schedule_render();
}
static void setup_default_layout_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) setup_layout_selected("", TR("Default"));
}
static void setup_layout_catalog_poll(lv_timer_t * timer) {
    (void)timer;
    if (!setup_screen || step != STEP_LAYOUT) return;
    if (!gui_navigation_is_top(setup_screen) || gui_navigation_transition_in_progress()) return;
    bool ready = gui_plugin_store_setup_catalog_ready();
    if (ready) {
        char preview[sizeof(layout_suggestion_preview)];
        if (!plugin_store_get_preview(GUI_SETUP_LAYOUT_PLUGIN_ID, preview, sizeof(preview)))
            (void)gui_plugin_store_setup_populate_layout_suggestions(NULL, NULL, NULL);
    }
    uint64_t previews = plugin_store_preview_generation();
    if (ready != layout_catalog_ready) schedule_render();
    else if (previews != layout_preview_generation) {
        char preview[sizeof(layout_suggestion_preview)] = "";
        (void)plugin_store_get_preview(GUI_SETUP_LAYOUT_PLUGIN_ID, preview, sizeof(preview));
        if (strcmp(preview, layout_suggestion_preview) != 0) schedule_render();
        else layout_preview_generation = previews;
    }
    if (!ready && !plugin_catalog_request_started && gui_network_setup_wifi_connected())
        plugin_catalog_request_started = gui_plugin_store_setup_catalog_prepare();
}
static void open_player_layouts_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    (void)gui_plugin_store_open_layout_picker(setup_layout_is_selected, setup_layout_selected);
}
static void style_plugin_selection(lv_obj_t * row, const char * id) {
    bool selected = gui_setup_plugins_is_selected(id);
    lv_obj_set_style_border_width(row, selected ? BOARD_SCALE_PX(2) : 0, 0);
    lv_obj_set_style_border_color(row, accent_lv_color(), 0);
    lv_obj_t * mark = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    lv_obj_set_state(mark, LV_STATE_CHECKED, selected);
}
static void set_suggested_plugin_available(size_t index, lv_obj_t * row, bool available) {
    if (!row) return;
    if (index < sizeof(plugin_download_available) / sizeof(plugin_download_available[0]))
        plugin_download_available[index] = available;
    bool connected = gui_network_setup_wifi_connected();
    lv_obj_set_style_opa(row, available && connected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_flag(row, LV_OBJ_FLAG_CLICKABLE, available && connected);
    lv_obj_set_state(row, LV_STATE_DISABLED, !available || !connected);
    lv_obj_t * check = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    lv_obj_set_state(check, LV_STATE_DISABLED, !available || !connected);
    lv_obj_set_flag(check, LV_OBJ_FLAG_CLICKABLE, available && connected);
}
static const char * suggested_plugin_unavailable_state(void) {
    if (!gui_network_setup_wifi_connected()) return TR("No network detected");
    plugin_store_status_t status;
    plugin_store_get_status(&status, NULL, 0);
    return status.state == PLUGIN_STORE_REFRESHING
        ? TR("Loading plugin catalog...") : TR("Unavailable");
}
static void toggle_suggested_plugin(lv_obj_t * row, const char * id) {
    plugin_store_result_t plugin;
    plugin_store_details_t details;
    if (!gui_plugin_store_setup_get_plugin(id, &plugin, &details)) {
        show_error_toast(TR("Plugin is unavailable in the catalog"));
        return;
    }
    gui_setup_plugins_toggle(id, plugin.name[0] ? plugin.name : id);
    style_plugin_selection(row, id);
}
static void suggested_plugin_cb(lv_event_t * e) {
    toggle_suggested_plugin(lv_event_get_target(e), lv_event_get_user_data(e));
}
static void suggested_checkbox_cb(lv_event_t * e) {
    toggle_suggested_plugin(lv_obj_get_parent(lv_event_get_target(e)), lv_event_get_user_data(e));
}

static void scan_row_cb(lv_event_t * e) {
    (void)e;
    staged_scan = !staged_scan;
    if (staged_scan) lv_obj_add_state(scan_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(scan_switch, LV_STATE_CHECKED);
    current_settings.setup_scan_music = staged_scan;
    settings_save(&current_settings);
}
static void scan_switch_cb(lv_event_t * e) {
    (void)e;
    staged_scan = lv_obj_has_state(scan_switch, LV_STATE_CHECKED);
    current_settings.setup_scan_music = staged_scan;
    settings_save(&current_settings);
}

static lv_obj_t * new_card(lv_obj_t * parent) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_color(card, lv_color_mix(accent_lv_color(),
                            style_color(&style_theme_card_bg, LV_STYLE_BG_COLOR, GUI_COLOR_PANEL), 20), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_border_width(card, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(card, lv_color_mix(accent_lv_color(), lv_color_hex(GUI_COLOR_SCREEN), 45), 0);
    lv_obj_set_style_pad_all(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(10), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scrollbar(card);
    return card;
}

static void add_page_title(lv_obj_t * parent, const char * title, const char * detail) {
    label(parent, title, GUI_FONT_ROLE_TITLE,
          style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xFFFFFF), LV_PCT(100));
    if (detail) {
        lv_obj_t * sub = label(parent, detail, GUI_FONT_ROLE_BODY,
                               style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
        lv_obj_set_style_text_line_space(sub, BOARD_SCALE_PX(3), 0);
    }
}

static const char * const journey_icons[] = {
    LV_SYMBOL_SETTINGS, LV_SYMBOL_GPS, LV_SYMBOL_WIFI, LV_SYMBOL_LIST,
    LV_SYMBOL_IMAGE, LV_SYMBOL_AUDIO
};
static const char * const journey_names[] = {
    N_("Language"), N_("Time zone"), N_("Wi-Fi"), N_("Plugins"),
    N_("Layout"), N_("Music")
};

static void add_header(void) {
    lv_obj_t * header = lv_obj_create(content);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, BOARD_SCALE_PX(60));
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scrollbar(header);
    if (step > STEP_WELCOME) {
        lv_obj_t * back = lv_button_create(header);
        lv_obj_set_size(back, BOARD_SCALE_PX(64), BOARD_SCALE_PX(56));
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(back, BOARD_SCALE_PX(16), 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_set_style_pad_all(back, 0, 0);
        no_scrollbar(back);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * arrow = lv_label_create(back);
        lv_label_set_text(arrow, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(arrow, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(arrow);
        no_scrollbar(arrow);
    } else {
        lv_obj_t * spacer = lv_obj_create(header);
        lv_obj_set_size(spacer, BOARD_SCALE_PX(64), BOARD_SCALE_PX(56));
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        no_scrollbar(spacer);
    }
    lv_obj_t * kicker = lv_label_create(header);
    lv_label_set_text(kicker, TR("Quick setup"));
    lv_obj_set_style_text_font(kicker, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_text_color(kicker, style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xFFFFFF), 0);

    lv_obj_set_width(kicker, LV_SIZE_CONTENT);
    no_scrollbar(kicker);
    if (step == STEP_WIFI || step == STEP_PLUGINS) {
        lv_obj_set_style_text_font(kicker, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
        lv_obj_set_width(kicker, 0);
        lv_obj_set_flex_grow(kicker, 1);
        lv_label_set_long_mode(kicker, LV_LABEL_LONG_WRAP);
        lv_obj_t * skip = button(header, TR("Skip for now"), false, skip_cb);
        lv_point_t skip_text_size;
        lv_text_get_size(&skip_text_size, TR("Skip for now"), gui_theme_font(GUI_FONT_ROLE_STATUS),
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t skip_width = skip_text_size.x + BOARD_SCALE_PX(28);
        int32_t skip_min = BOARD_SCALE_PX(120);
        if (skip_width < skip_min) skip_width = skip_min;
        int32_t skip_max = BOARD_SCALE_PX(160);
        if (skip_width > skip_max) skip_width = skip_max;
        lv_obj_set_size(skip, skip_width, BOARD_SCALE_PX(56));
        lv_obj_set_style_pad_top(skip, 0, 0);
        lv_obj_set_style_pad_bottom(skip, 0, 0);
        lv_obj_set_style_pad_left(skip, BOARD_SCALE_PX(8), 0);
        lv_obj_set_style_pad_right(skip, BOARD_SCALE_PX(8), 0);
        lv_obj_t * skip_caption = lv_obj_get_child(skip, 0);
        lv_obj_set_width(skip_caption, LV_PCT(100));
        lv_label_set_long_mode(skip_caption, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(skip_caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_ext_click_area(skip, BOARD_SCALE_PX(6));
        lv_obj_set_style_radius(skip, BOARD_SCALE_PX(12), 0);
        lv_obj_set_style_text_font(skip_caption, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
    }
}

static void add_progress(void) {
    lv_obj_t * strip = lv_obj_create(content);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_height(strip, BOARD_SCALE_PX(10));
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_style_pad_column(strip, BOARD_SCALE_PX(5), 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    no_scrollbar(strip);
    for (int i = 0; i < 6; ++i) {
        lv_obj_t * segment = lv_obj_create(strip);
        lv_obj_set_height(segment, BOARD_SCALE_PX(6));
        lv_obj_set_flex_grow(segment, 1);
        lv_obj_set_style_radius(segment, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(segment, 0, 0);
        lv_obj_set_style_bg_color(segment, i + 1 <= setup_step_position(step)
            ? accent_lv_color() : lv_color_hex(GUI_COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
        no_scrollbar(segment);
    }
}

static void journey_chip_size_cb(lv_event_t * event) {
    if (lv_event_get_code(event) != LV_EVENT_SIZE_CHANGED) return;
    lv_obj_t * chip = lv_event_get_current_target(event);
    lv_obj_t * icon = lv_obj_get_child(chip, 0);
    lv_obj_t * name = lv_obj_get_child(chip, 1);
    if (!icon || !name) return;

    int32_t available = lv_obj_get_width(chip) -
        lv_obj_get_style_pad_left(chip, 0) - lv_obj_get_style_pad_right(chip, 0) -
        lv_obj_get_width(icon) - lv_obj_get_style_pad_column(chip, 0);
    if (available < 1) available = 1;
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(name), lv_obj_get_style_text_font(name, 0),
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t name_width = text_size.x < available ? text_size.x : available;
    if (name_width < 1) name_width = 1;
    if (lv_obj_get_width(name) != name_width) lv_obj_set_width(name, name_width);
}

static void add_journey(void) {
    const lv_font_t * icon_font = gui_theme_font(GUI_FONT_ROLE_ROW);
    int32_t icon_column_width = BOARD_SCALE_PX(32);
    for (size_t i = 0; i < sizeof(journey_icons) / sizeof(journey_icons[0]); ++i) {
        lv_point_t icon_size;
        lv_text_get_size(&icon_size, journey_icons[i], icon_font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (icon_size.x > icon_column_width) icon_column_width = icon_size.x;
    }
    lv_obj_t * panel = new_card(setup_body);
    /* The welcome overview absorbs the remaining body height so its entries
     * are spaced through the card and the action remains anchored below. */
    lv_obj_set_height(panel, 0);
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_pad_all(panel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_row(panel, BOARD_SCALE_PX(8), 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scrollbar(panel);
    lv_obj_t * heading = lv_label_create(panel);
    lv_label_set_text(heading, TR("Your setup journey"));
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(heading, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(GUI_COLOR_SECONDARY), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    const int position = setup_step_position(step);
    for (int row_index = 0; row_index < 3; ++row_index) {
        lv_obj_t * row = lv_obj_create(panel);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 0);
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, BOARD_SCALE_PX(6), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        no_scrollbar(row);
        int start = row_index * 2;
        int end = start + 2;
        if (end > 6) end = 6;
        for (int i = start; i < end; ++i) {
            lv_obj_t * chip = lv_obj_create(row);
            lv_obj_set_height(chip, LV_PCT(100));
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_style_bg_color(chip, lv_color_hex(GUI_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(chip, i + 1 == position ? BOARD_SCALE_PX(1) : 0, 0);
            lv_obj_set_style_border_color(chip, accent_lv_color(), 0);
            no_scrollbar(chip);
            lv_obj_set_style_pad_top(chip, 0, 0);
            lv_obj_set_style_pad_bottom(chip, 0, 0);
            lv_obj_set_style_pad_left(chip, BOARD_SCALE_PX(9), 0);
            lv_obj_set_style_pad_right(chip, BOARD_SCALE_PX(9), 0);
            lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(chip, BOARD_SCALE_PX(10), 0);
            lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_event_cb(chip, journey_chip_size_cb, LV_EVENT_SIZE_CHANGED, NULL);
            lv_obj_t * icon = lv_label_create(chip);
            lv_label_set_text(icon, journey_icons[i]);
            lv_obj_set_width(icon, icon_column_width);
            lv_obj_set_style_text_color(icon, i + 1 <= position ? accent_lv_color() : lv_color_hex(GUI_COLOR_SECONDARY), 0);
            lv_obj_set_style_text_font(icon, icon_font, 0);
            lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t * name = lv_label_create(chip);
            lv_label_set_text(name, TR(journey_names[i]));
            lv_obj_set_style_text_color(name,
                                        style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xE5E8ED), 0);
            lv_obj_set_style_text_font(name, gui_theme_font(
                BOARD_SCREEN_WIDTH < BOARD_REFERENCE_WIDTH ? GUI_FONT_ROLE_BODY : GUI_FONT_ROLE_ROW), 0);
            lv_obj_set_width(name, LV_SIZE_CONTENT);
            lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
            no_scrollbar(icon);
            no_scrollbar(name);
        }
    }
}

static void add_vinyl_hero(lv_obj_t * parent) {
    lv_obj_t * hero = new_card(parent);
    lv_obj_set_height(hero, BOARD_SCALE_PX(150));
    lv_obj_set_style_bg_color(hero, lv_color_mix(accent_lv_color(),
                            style_color(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_SCREEN), 34), 0);
    lv_obj_set_style_pad_all(hero, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int32_t disc = BOARD_SCALE_PX(118);
    lv_obj_t * record = lv_obj_create(hero);
    no_scrollbar(record);
    lv_obj_set_size(record, disc, disc);
    lv_obj_set_style_radius(record, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(record, lv_color_hex(0x080A0D), 0);
    lv_obj_set_style_bg_opa(record, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(record, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(record, lv_color_mix(accent_lv_color(), lv_color_hex(0xFFFFFF), 45), 0);
    lv_obj_t * groove1 = lv_obj_create(record);
    no_scrollbar(groove1);
    lv_obj_set_size(groove1, disc - BOARD_SCALE_PX(18), disc - BOARD_SCALE_PX(18));
    lv_obj_center(groove1);
    lv_obj_set_style_radius(groove1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(groove1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(groove1, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(groove1, lv_color_hex(0x282C33), 0);
    lv_obj_t * groove2 = lv_obj_create(record);
    no_scrollbar(groove2);
    lv_obj_set_size(groove2, disc - BOARD_SCALE_PX(38), disc - BOARD_SCALE_PX(38));
    lv_obj_center(groove2);
    lv_obj_set_style_radius(groove2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(groove2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(groove2, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(groove2, lv_color_hex(0x282C33), 0);
    lv_obj_t * label_disc = lv_obj_create(record);
    no_scrollbar(label_disc);
    lv_obj_set_size(label_disc, BOARD_SCALE_PX(42), BOARD_SCALE_PX(42));
    lv_obj_center(label_disc);
    lv_obj_set_style_radius(label_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(label_disc, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(label_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(label_disc, 0, 0);
    lv_obj_t * spindle = lv_obj_create(record);
    no_scrollbar(spindle);
    lv_obj_set_size(spindle, BOARD_SCALE_PX(7), BOARD_SCALE_PX(7));
    lv_obj_center(spindle);
    lv_obj_set_style_radius(spindle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(spindle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(spindle, 0, 0);

    lv_obj_t * bars = lv_obj_create(hero);
    no_scrollbar(bars);
    lv_obj_set_size(bars, BOARD_SCALE_PX(112), BOARD_SCALE_PX(92));
    lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_obj_set_style_pad_all(bars, 0, 0);
    lv_obj_set_style_pad_column(bars, BOARD_SCALE_PX(5), 0);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const int heights[] = { 22, 45, 72, 38, 88, 54, 30, 66, 40 };
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        lv_obj_t * bar = lv_obj_create(bars);
        no_scrollbar(bar);
        lv_obj_set_size(bar, BOARD_SCALE_PX(5), BOARD_SCALE_PX(heights[i]));
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bar, lv_color_mix(accent_lv_color(), lv_color_hex(0xFFFFFF), i == 4 ? 250 : 120), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
    }
}

/* Draw embedded land spans directly: no transient line points or image buffers. */
static void timezone_map_draw(lv_event_t * e) {
    lv_obj_t * map = lv_event_get_target(e);
    lv_area_t area;
    lv_obj_get_content_coords(map, &area);
    const int32_t w = lv_area_get_width(&area), h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_mix(accent_lv_color(),
        style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), 65);
    line.opa = LV_OPA_COVER;
    line.width = (h + 179) / 180;
    for (size_t i = 0; i < sizeof(setup_land_spans) / sizeof(setup_land_spans[0]); ++i) {
        const uint16_t * span = setup_land_spans[i];
        line.p1.x = area.x1 + span[1] * (w - 1) / 359;
        line.p2.x = area.x1 + span[2] * (w - 1) / 359;
        line.p1.y = line.p2.y = area.y1 + span[0] * (h - 1) / 179;
        lv_draw_line(layer, &line);
    }
    int32_t lat, lon;
    if (!timezone_location_get(current_settings.timezone, &lat, &lon)) return;
    int32_t x = area.x1 + (lon + 180000) * (w - 1) / 360000;
    int32_t y = area.y1 + (90000 - lat) * (h - 1) / 180000;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = accent_lv_color();
    dot.bg_opa = LV_OPA_30;
    int32_t r = BOARD_SCALE_PX(12);
    lv_area_t halo = { x - r, y - r, x + r, y + r };
    lv_draw_rect(layer, &dot, &halo);
    r = BOARD_SCALE_PX(5);
    lv_area_t pin = { x - r, y - r, x + r, y + r };
    dot.bg_opa = LV_OPA_COVER;
    dot.border_width = BOARD_SCALE_PX(2);
    dot.border_color = style_color(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_SCREEN);
    dot.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dot, &pin);
}

static void add_step_illustration(void) {
    lv_obj_t * visual = lv_obj_create(setup_body);
    lv_obj_set_size(visual, LV_PCT(100), BOARD_SCALE_PX(112));
    lv_obj_set_style_bg_opa(visual, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(visual, 0, 0);
    lv_obj_set_style_pad_all(visual, 0, 0);
    lv_obj_set_layout(visual, LV_LAYOUT_NONE);
    no_scrollbar(visual);
    if (step == STEP_TIMEZONE) {
        lv_obj_set_height(visual, BOARD_SCALE_PX(210));
        lv_obj_set_style_radius(visual, BOARD_SCALE_PX(20), 0);
        lv_obj_set_style_bg_color(visual, lv_color_mix(accent_lv_color(),
            style_color(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_SCREEN), 12), 0);
        lv_obj_set_style_bg_opa(visual, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(visual, BOARD_SCALE_PX(16), 0);
        lv_obj_add_event_cb(visual, timezone_map_draw, LV_EVENT_DRAW_MAIN, NULL);
        return;
    }
    const bool compact_badge = step == STEP_WIFI || step == STEP_PLUGINS;
    const int32_t size = BOARD_SCALE_PX(compact_badge ? 56 : 92);
    if (step == STEP_WIFI) lv_obj_set_height(visual, BOARD_SCALE_PX(64));
    else if (compact_badge) lv_obj_set_height(visual, BOARD_SCALE_PX(56));
    lv_obj_t * badge = lv_obj_create(visual);
    lv_obj_set_size(badge, size, size);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(2), 0);
    lv_obj_set_style_radius(badge, step == STEP_LANGUAGE || step == STEP_PLUGINS
                                    ? BOARD_SCALE_PX(22) : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_mix(accent_lv_color(),
                            style_color(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_SCREEN), 30), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, BOARD_SCALE_PX(2), 0);
    lv_obj_set_style_border_color(badge, accent_lv_color(), 0);
    no_scrollbar(badge);

    if (step == STEP_LANGUAGE) {
        lv_obj_t * letters = lv_label_create(badge);
        lv_label_set_text(letters, "Aa");
        lv_obj_set_style_text_color(letters, accent_lv_color(), 0);
        lv_obj_set_style_text_font(letters, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_center(letters);
        no_scrollbar(letters);
    } else if (step == STEP_WIFI) {
        lv_obj_t * glyph = lv_label_create(badge);
        lv_label_set_text(glyph, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(glyph, accent_lv_color(), 0);
        lv_obj_set_style_text_font(glyph, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_center(glyph);
        no_scrollbar(glyph);
    } else if (step == STEP_PLUGINS) {
        lv_obj_t * grid = lv_obj_create(badge);
        lv_obj_set_size(grid, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_obj_center(grid);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_set_style_pad_row(grid, BOARD_SCALE_PX(5), 0);
        lv_obj_set_style_pad_column(grid, BOARD_SCALE_PX(5), 0);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        no_scrollbar(grid);
        for (int i = 0; i < 4; ++i) {
            lv_obj_t * tile = lv_obj_create(grid);
            lv_obj_set_size(tile, BOARD_SCALE_PX(18), BOARD_SCALE_PX(18));
            lv_obj_set_style_radius(tile, BOARD_SCALE_PX(6), 0);
            lv_obj_set_style_bg_color(tile, lv_color_mix(accent_lv_color(), lv_color_hex(0xFFFFFF), i == 0 ? 210 : 120), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            no_scrollbar(tile);
        }
    } else {
        lv_obj_t * disc = lv_obj_create(badge);
        lv_obj_set_size(disc, BOARD_SCALE_PX(58), BOARD_SCALE_PX(58));
        lv_obj_center(disc);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(disc, lv_color_hex(0x090B0E), 0);
        lv_obj_set_style_border_width(disc, BOARD_SCALE_PX(1), 0);
        lv_obj_set_style_border_color(disc, lv_color_hex(0x626A75), 0);
        no_scrollbar(disc);
        lv_obj_t * center = lv_obj_create(disc);
        lv_obj_set_size(center, BOARD_SCALE_PX(26), BOARD_SCALE_PX(26));
        lv_obj_center(center);
        lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(center, accent_lv_color(), 0);
        lv_obj_set_style_border_width(center, 0, 0);
        no_scrollbar(center);
    }
}

static lv_color_t style_color(lv_style_t * style, lv_style_prop_t prop, uint32_t fallback) {
    lv_style_value_t value;
    if (lv_style_get_prop(style, prop, &value) == LV_STYLE_RES_FOUND) return value.color;
    return lv_color_hex(fallback);
}

static lv_obj_t * suggested_plugin(lv_obj_t * parent, const char * name, const char * description,
                             const char * id, bool equalizer) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, BOARD_SCALE_PX(88), 0);
    lv_obj_set_style_radius(row, BOARD_SCALE_PX(18), 0);
    lv_obj_add_style(row, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_color(row, lv_color_mix(accent_lv_color(),
        style_color(&style_theme_card_bg, LV_STYLE_BG_COLOR, GUI_COLOR_PANEL), 35), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_column(row, BOARD_SCALE_PX(14), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scrollbar(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, suggested_plugin_cb, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t * badge = lv_obj_create(row);
    lv_obj_set_size(badge, BOARD_SCALE_PX(56), BOARD_SCALE_PX(56));
    lv_obj_set_style_radius(badge, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_bg_color(badge, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_10, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    no_scrollbar(badge);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    if (equalizer) {
        static const int heights[] = { 16, 30, 22 };
        for (int i = 0; i < 3; ++i) {
            lv_obj_t * bar = lv_obj_create(badge);
            lv_obj_set_size(bar, BOARD_SCALE_PX(5), BOARD_SCALE_PX(heights[i]));
            lv_obj_align(bar, LV_ALIGN_CENTER, BOARD_SCALE_PX((i - 1) * 11), 0);
            lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(bar, accent_lv_color(), 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            no_scrollbar(bar);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
    } else {
        lv_obj_t * glyph = label(badge, LV_SYMBOL_VOLUME_MAX, GUI_FONT_ROLE_ROW, accent_lv_color(), LV_SIZE_CONTENT);
        lv_obj_center(glyph);
    }
    lv_obj_t * text = lv_obj_create(row);
    lv_obj_set_size(text, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text, 0, 0);
    lv_obj_set_style_pad_all(text, 0, 0);
    lv_obj_set_style_pad_row(text, BOARD_SCALE_PX(5), 0);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    no_scrollbar(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
    label(text, name, GUI_FONT_ROLE_BODY,
          style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xFFFFFF), LV_PCT(100));
    lv_obj_t * desc = label(text, description, GUI_FONT_ROLE_SUBTEXT,
          style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
    lv_obj_set_height(desc, 2 * lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
    lv_obj_t * check = lv_checkbox_create(row);
    lv_checkbox_set_text(check, "");
    lv_obj_set_style_text_font(check, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_border_color(check, accent_lv_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(check, accent_lv_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_image_src(check, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(check, gui_theme_font(GUI_FONT_ROLE_ROW), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(check, lv_color_brightness(accent_lv_color()) > 160
        ? lv_color_hex(0x14170B) : lv_color_white(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_flag(check, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(check, BOARD_SCALE_PX(8));
    lv_obj_add_event_cb(check, suggested_checkbox_cb, LV_EVENT_VALUE_CHANGED, (void *)id);
    style_plugin_selection(row, id);
    return row;
}

static void render_async(void * unused) {
    (void)unused;
    if (!setup_screen || !content) return;
    plugin_network_notice = NULL;
    plugin_catalog_notice = NULL;
    memset(plugin_download_rows, 0, sizeof(plugin_download_rows));
    memset(plugin_download_available, 0, sizeof(plugin_download_available));
    if (step != STEP_PLUGINS && step != STEP_LAYOUT) plugin_catalog_request_started = false;
    if (step != STEP_LAYOUT && layout_catalog_timer) {
        lv_timer_delete(layout_catalog_timer);
        layout_catalog_timer = NULL;
    }
    lv_obj_clean(content);
    scan_switch = NULL;
    setup_body = NULL;

    add_header();
    add_progress();
    setup_body = lv_obj_create(content);
    lv_obj_set_width(setup_body, LV_PCT(100));
    lv_obj_set_height(setup_body, 0);
    lv_obj_set_flex_grow(setup_body, 1);
    lv_obj_set_style_bg_opa(setup_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(setup_body, 0, 0);
    lv_obj_set_style_pad_all(setup_body, 0, 0);
    lv_obj_set_style_pad_row(setup_body, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(setup_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(setup_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(setup_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(setup_body, LV_DIR_VER);

    lv_obj_t * footer = lv_obj_create(content);
    lv_obj_set_width(footer, LV_PCT(100));
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_set_style_pad_row(footer, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scrollbar(footer);

    if (step == STEP_WELCOME) {
        add_vinyl_hero(setup_body);
        lv_obj_t * title = label(setup_body, TR("Welcome to Compás"), GUI_FONT_ROLE_TITLE,
                                 style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xFFFFFF), LV_PCT(100));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
        add_journey();
        button(footer, TR("Get started"), true, next_cb);
        return;
    }

    if (step == STEP_COMPLETE) {
        add_vinyl_hero(setup_body);
        add_page_title(setup_body, TR("Quick Setup Complete"), "");
        button(footer, TR("Continue"), true, complete_cb);
        return;
    }

    lv_obj_t * step_count = label(setup_body, "", GUI_FONT_ROLE_STATUS,
                                  style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
    int position = setup_step_position(step);
    lv_label_set_text_fmt(step_count, TR("Step %d of %d"), position,
                          setup_step_position(STEP_SCAN));
    if (step == STEP_TIMEZONE) gui_network_setup_wifi_prepare();
    if (step != STEP_LAYOUT) add_step_illustration();

    const char * title = step == STEP_LANGUAGE ? TR("Language") :
                         step == STEP_TIMEZONE ? TR("Time zone") :
                         step == STEP_WIFI ? TR("Wi-Fi") :
                         step == STEP_PLUGINS ? TR("Plugins") :
                         step == STEP_LAYOUT ? TR("Player Layout") : TR("Your music");
    char timezone_detail[192];
    const char * detail = step == STEP_LANGUAGE ? TR("Choose the language for your player.") :
                          step == STEP_TIMEZONE ? TR("Set your local time zone so the clock is right.") :
                          step == STEP_WIFI ? TR("Connect to Wi-Fi for streaming, updates, and online services.") :
                          step == STEP_PLUGINS ? TR("Start with these suggestions, or explore more plugins.") :
                          step == STEP_LAYOUT ? NULL :
                          TR("Insert your SD card with music files. Compas Player can scan it and build your library.");
    if (step == STEP_TIMEZONE) {
        const char * selected_timezone = current_settings.timezone[0] ? current_settings.timezone : TR("Not selected (UTC)");
        snprintf(timezone_detail, sizeof(timezone_detail), "%s\n%s", detail, selected_timezone);
        detail = timezone_detail;
    }
    add_page_title(setup_body, title, detail);
    lv_obj_t * card = new_card(setup_body);
    if (step == STEP_LANGUAGE) {
        button(card, selected_language_name(), false, language_open_cb);
        button(footer, TR("Continue"), true, next_cb);
        return;
    }
    if (step == STEP_TIMEZONE) {
        button(card, TR("Choose time zone"), false, open_timezone_cb);
        button(footer, TR("Continue"), true, next_cb);
        return;
    }
    if (step == STEP_WIFI) {
        gui_network_setup_wifi_prepare();
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_t * actions = lv_obj_create(card);
        lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(actions, 0, 0);
        lv_obj_set_style_pad_all(actions, 0, 0);
        lv_obj_set_style_pad_column(actions, BOARD_SCALE_PX(12), 0);
        lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
        no_scrollbar(actions);
        lv_obj_t * hidden = button(actions, TR("Add hidden network"), false, wifi_hidden_cb);
        lv_obj_set_width(hidden, 0);
        lv_obj_set_flex_grow(hidden, 1);
        lv_obj_t * rescan = button(actions, TR("Rescan"), false, wifi_rescan_cb);
        lv_obj_set_width(rescan, 0);
        lv_obj_set_flex_grow(rescan, 1);
        /* Equal action widths leave room for translated refresh captions.
         * Wrap the longer hidden-network caption inside the button. */
        lv_obj_t * wifi_actions[] = { hidden, rescan };
        for (unsigned i = 0; i < sizeof(wifi_actions) / sizeof(wifi_actions[0]); ++i) {
            lv_obj_t * action = wifi_actions[i];
            lv_obj_set_height(action, ui_list_row_height());
            lv_obj_set_style_pad_all(action, BOARD_SCALE_PX(8), 0);
            lv_obj_t * caption = lv_obj_get_child(action, 0);
            lv_obj_set_width(caption, LV_PCT(100));
            lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_t * networks = lv_obj_create(card);
        lv_obj_set_size(networks, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(networks, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(networks, 0, 0);
        lv_obj_set_style_pad_all(networks, 0, 0);
        lv_obj_set_style_pad_row(networks, BOARD_SCALE_PX(10), 0);
        lv_obj_set_flex_flow(networks, LV_FLEX_FLOW_COLUMN);
        no_scrollbar(networks);
        gui_network_setup_wifi_mount(networks);
        button(footer, TR("Continue"), true, next_cb);
        return;
    }
    if (step == STEP_PLUGINS) {
        memset(plugin_download_rows, 0, sizeof(plugin_download_rows));
        memset(plugin_download_available, 0, sizeof(plugin_download_available));
        gui_network_setup_wifi_prepare();
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        plugin_network_notice = new_card(card);
        lv_obj_add_flag(plugin_network_notice, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(plugin_network_notice, connect_plugins_wifi_cb, LV_EVENT_CLICKED, NULL);
        label(plugin_network_notice, TR("No network detected"), GUI_FONT_ROLE_BODY, accent_lv_color(), LV_PCT(100));
        label(plugin_network_notice, TR("Connect to a network to download plugins."), GUI_FONT_ROLE_SUBTEXT,
              style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
        label(plugin_network_notice, TR("Connect to Wi-Fi"), GUI_FONT_ROLE_SUBTEXT, accent_lv_color(), LV_PCT(100));
        if (!plugin_catalog_request_started && gui_network_setup_wifi_connected())
            plugin_catalog_request_started = gui_plugin_store_setup_catalog_prepare();
        plugin_catalog_rendered = gui_plugin_store_setup_catalog_ready();
        if (!plugin_catalog_rendered && gui_network_setup_wifi_connected())
            plugin_catalog_notice = label(card, TR("Loading plugin catalog..."), GUI_FONT_ROLE_SUBTEXT,
                  style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
        for (size_t i = 0; i < 2; ++i) {
            const char * id = suggested_plugin_ids[i];
            bool is_eq = (i == 1);
            const char * fallback_name = is_eq ? TR("AutoEQ") : TR("Gain Mode");
            plugin_store_result_t plugin;
            plugin_store_details_t details;
            bool found = gui_plugin_store_setup_get_plugin(id, &plugin, &details);
            if (!found) {
                plugin_download_rows[i] = suggested_plugin(card, fallback_name,
                    suggested_plugin_unavailable_state(), id, is_eq);
                set_suggested_plugin_available(i, plugin_download_rows[i], false);
                continue;
            }
            if (plugin.incompatible || plugin.state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE) {
                plugin_download_rows[i] = suggested_plugin(card,
                    plugin.name[0] ? plugin.name : fallback_name,
                    TR("Needs newer firmware"),
                    id, is_eq);
                set_suggested_plugin_available(i, plugin_download_rows[i], false);
                continue;
            }
            plugin_download_rows[i] = suggested_plugin(card,
                plugin.name[0] ? plugin.name : fallback_name,
                details.description, id, is_eq);
            plugin_download_available[i] = true;
        }
        plugin_download_rows[2] = button(card, TR("More"), false, open_plugins_cb);
        plugin_network_connected = !gui_network_setup_wifi_connected();
        plugin_network_refresh();
        if (!plugin_network_timer) plugin_network_timer = lv_timer_create(plugin_network_poll, 500, NULL);
        button(footer, TR("Done"), true, next_cb);
        return;
    }
    if (step == STEP_LAYOUT) {
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_t * grid = lv_obj_create(card);
        lv_obj_remove_style_all(grid);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        configure_cover_card_grid(grid, 2);
        lv_obj_set_style_pad_top(grid, BOARD_SCALE_PX(6), 0);
        lv_obj_set_style_pad_bottom(grid, BOARD_SCALE_PX(6), 0);
        int default_index = player_layouts_find(PLAYER_LAYOUT_ID_DEFAULT);
        char preview[512], resolved[520];
        const char * preview_src = NULL;
        if (player_layouts_get_preview(default_index, preview, sizeof(preview))) {
            snprintf(resolved, sizeof(resolved), "S:%s", preview);
            preview_src = resolved;
        }
        lv_obj_t * default_card = add_cover_card(grid, TR("Default"), preview_src, 2,
                                                 setup_default_layout_cb, NULL);
        if (default_card) {
            lv_obj_set_style_outline_width(default_card, staged_layout_plugin[0] ? 0 : BOARD_SCALE_PX(2), 0);
            lv_obj_set_style_outline_color(default_card, accent_lv_color(), 0);
            lv_obj_set_style_outline_pad(default_card, BOARD_SCALE_PX(2), 0);
        }
        layout_catalog_ready = gui_plugin_store_setup_catalog_ready();
        layout_preview_generation = plugin_store_preview_generation();
        layout_suggestion_preview[0] = '\0';
        (void)plugin_store_get_preview(GUI_SETUP_LAYOUT_PLUGIN_ID, layout_suggestion_preview,
                                     sizeof(layout_suggestion_preview));
        (void)gui_plugin_store_setup_populate_layout_suggestions(grid, setup_layout_is_selected, setup_layout_selected);
        if (!layout_catalog_ready && gui_network_setup_wifi_connected()) {
            label(card, TR("Loading layouts"), GUI_FONT_ROLE_SUBTEXT,
                  style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
            if (!plugin_catalog_request_started)
                plugin_catalog_request_started = gui_plugin_store_setup_catalog_prepare();
        }
        if (!layout_catalog_timer) layout_catalog_timer = lv_timer_create(setup_layout_catalog_poll, 500, NULL);
        button(card, TR("More"), false, open_player_layouts_cb);
        button(footer, TR("Continue"), true, next_cb);
        return;
    }
    lv_obj_t * row = lv_obj_create(card);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, BOARD_SCALE_PX(72));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    no_scrollbar(row);
    lv_obj_add_event_cb(row, scan_row_cb, LV_EVENT_CLICKED, NULL);
    label(row, TR("Scan for music"), GUI_FONT_ROLE_ROW, lv_color_hex(0xFFFFFF), LV_PCT(72));
    scan_switch = lv_switch_create(row);
    lv_obj_set_size(scan_switch, BOARD_SCALE_PX(64), BOARD_SCALE_PX(40));
    no_scrollbar(scan_switch);
    lv_obj_add_style(scan_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_flag(scan_switch, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(scan_switch, scan_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (staged_scan) lv_obj_add_state(scan_switch, LV_STATE_CHECKED);
    if (!sd_card_root_is_mounted())
        label(card, TR("No SD card detected. You can scan later from Library settings."),
              GUI_FONT_ROLE_SUBTEXT, style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), LV_PCT(100));
    button(footer, TR("Continue"), true, finish_cb);
}

static void finish_async(void * unused) {
    (void)unused;
    if (!setup_screen) return;
    bool language_changed = strcmp(staged_language, current_settings.language) != 0;
    snprintf(current_settings.language, sizeof(current_settings.language), "%s", staged_language);
    current_settings.setup_complete = true;
    memset(current_settings.setup_plugin_ids, 0, sizeof(current_settings.setup_plugin_ids));
    current_settings.setup_layout_plugin_id[0] = '\0';
    current_settings.setup_scan_music = true;
    settings_save(&current_settings);
    if (language_changed) i18n_set_language(staged_language);
    gui_library_invalidate_boot_prompt();
    nav_reset_to_home();
    if (language_changed) gui_reload_request();
    gui_setup_teardown();
}


static void intro_done_cb(lv_anim_t * anim) {
    (void)anim;
    intro_cleanup();
}

static void intro_cleanup(void) {
    if (intro_overlay) {
        lv_anim_delete(intro_overlay, intro_exec);
        lv_obj_delete(intro_overlay);
        intro_overlay = NULL;
    }
    intro_title = NULL;
    memset(intro_notes, 0, sizeof(intro_notes));
    if (intro_background) {
        lv_image_cache_drop(intro_frame);
        lv_obj_delete(intro_background);
        intro_background = NULL;
    }
    if (intro_frame) {
        lv_draw_buf_destroy(intro_frame);
        intro_frame = NULL;
    }
    if (content) lv_obj_remove_flag(content, LV_OBJ_FLAG_HIDDEN);
}

static void intro_exec(void * obj, int32_t progress) {
    (void)obj;
    if (!intro_overlay) return;
    int32_t width = lv_obj_get_width(setup_screen);
    int32_t height = lv_obj_get_height(setup_screen);
    /* Leave the title readable while the notes fall, then reveal the page. */
    int32_t reveal = progress > 720 ? (progress - 720) * 255 / 280 : 0;
    if (reveal > 255) reveal = 255;
    lv_obj_set_style_bg_opa(intro_overlay, 255 - reveal, 0);
    int32_t title_in = progress < 100 ? progress * 255 / 100 : 255;
    lv_obj_set_style_opa(intro_title, title_in * (255 - reveal) / 255, 0);
    for (int i = 0; i < INTRO_NOTE_COUNT; ++i) {
        int32_t delay = (i * 43) % 300;
        int32_t local = progress - delay;
        if (local < 0) local = 0;
        int32_t x = (int32_t)((uint32_t)(i * 137 + 19) % (uint32_t)(width - BOARD_SCALE_PX(32)));
        int32_t start_y = -BOARD_SCALE_PX(36) - (i % 4) * BOARD_SCALE_PX(36);
        int32_t y = start_y + local * (height + BOARD_SCALE_PX(180)) / 1050;
        lv_obj_set_pos(intro_notes[i], x, y);
        int32_t fade_in = local < 130 ? local * 180 / 130 : 180;
        int32_t fade_out = local > 280 ? (local - 280) * 180 / 570 : 0;
        int32_t opacity = fade_in - fade_out;
        if (opacity < 0) opacity = 0;
        opacity = opacity * (255 - reveal) / 255;
        lv_obj_set_style_opa(intro_notes[i], opacity, 0);
    }
}

static void intro_start(void) {
    if (current_settings.setup_intro_played || step != STEP_WELCOME) return;
    lv_obj_update_layout(setup_screen);
    intro_frame = lv_snapshot_take(setup_screen, LV_COLOR_FORMAT_RGB565);
    if (!intro_frame) return; /* Setup remains usable under memory pressure. */
    current_settings.setup_intro_played = true;
    settings_save(&current_settings);
    intro_background = lv_image_create(setup_screen);
    lv_image_set_src(intro_background, intro_frame);
    lv_obj_set_pos(intro_background, 0, 0);
    no_scrollbar(intro_background);
    lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);

    intro_overlay = lv_obj_create(setup_screen);
    lv_obj_remove_style_all(intro_overlay);
    lv_obj_set_size(intro_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(intro_overlay, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(intro_overlay, LV_OPA_COVER, 0);
    no_scrollbar(intro_overlay);
    lv_obj_add_flag(intro_overlay, LV_OBJ_FLAG_CLICKABLE);
    /* Consume taps without advancing setup until the intro completes. */
    for (int i = 0; i < INTRO_NOTE_COUNT; ++i) {
        intro_notes[i] = lv_label_create(intro_overlay);
        lv_label_set_text(intro_notes[i], LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(intro_notes[i],
            gui_theme_font(i % 3 == 0 ? GUI_FONT_ROLE_TITLE : GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_style_text_color(intro_notes[i], i % 3 == 0 ? accent_lv_color() :
            style_color(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY), 0);
        no_scrollbar(intro_notes[i]);
        lv_obj_remove_flag(intro_notes[i], LV_OBJ_FLAG_CLICKABLE);
    }
    intro_title = label(intro_overlay, TR("Welcome to Compás"), GUI_FONT_ROLE_TITLE,
        style_color(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, 0xFFFFFF), LV_PCT(90));
    lv_obj_set_style_text_align(intro_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(intro_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(intro_title, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, intro_overlay);
    lv_anim_set_exec_cb(&anim, intro_exec);
    lv_anim_set_values(&anim, 0, 1000);
    lv_anim_set_duration(&anim, 2000);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_completed_cb(&anim, intro_done_cb);
    if (!lv_anim_start(&anim)) intro_cleanup();
}

bool gui_setup_show_if_needed(void) {
    if (current_settings.setup_complete) return false;
    if (setup_screen) return true;
    if (!setup_started) {
        setup_started = true;
        step = current_settings.setup_step;
        if (setup_step_position(step) < 0) step = STEP_WELCOME;
        gui_setup_plugins_restore_selection();
        staged_scan = current_settings.setup_scan_music;
        snprintf(staged_layout_plugin, sizeof(staged_layout_plugin), "%s", current_settings.setup_layout_plugin_id);
        snprintf(staged_layout_name, sizeof(staged_layout_name), "%s", staged_layout_plugin);
        snprintf(staged_language, sizeof(staged_language), "%s",
                 current_settings.language[0] ? current_settings.language : I18N_DEFAULT_LANGUAGE);
        DB_LOG("SETUP_NAV", "startup restored step=%d persisted_step=%d setup_complete=%d",
               step, current_settings.setup_step, current_settings.setup_complete);
        (void)db_log_flush();
    } else {
        DB_LOG("SETUP_NAV", "show existing step=%d persisted_step=%d setup_complete=%d",
               step, current_settings.setup_step, current_settings.setup_complete);
        (void)db_log_flush();
    }
    setup_screen = lv_obj_create(NULL);
    lv_obj_add_event_cb(setup_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(setup_screen, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(setup_screen, LV_OPA_COVER, 0);
    content = lv_obj_create(setup_screen);
    lv_obj_set_size(content, LV_PCT(100),
                    lv_display_get_vertical_resolution(NULL) - STATUS_BAR_CLEARANCE - BOARD_SCALE_PX(8));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_left(content, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_right(content, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_top(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(content, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_row(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    render_async(NULL);
    intro_start();
    nav_push(setup_screen);
    gui_setup_plugins_resume();
    return true;
}

void gui_setup_after_reload(void) {
    if (!setup_apply_pending || !setup_screen || step != STEP_SCAN) return;
    setup_apply_pending = false;
    char canonical[PLAYER_LAYOUT_ID_MAX];
    if (current_settings.player_layout[0] &&
        player_layouts_canonical_id(current_settings.player_layout, canonical, sizeof(canonical)) &&
        strcmp(canonical, current_settings.player_layout) != 0) {
        snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s", canonical);
        settings_save(&current_settings);
        player_layouts_rescan();
    }
    if (staged_scan) {
        setup_scan_running = true;
        lv_async_call(start_setup_scan_async, NULL);
    } else {
        setup_release_storage(true);
    }
}

void gui_setup_teardown(void) {
    if (setup_apply_timer) { lv_timer_delete(setup_apply_timer); setup_apply_timer = NULL; }
    lv_async_call_cancel(start_setup_scan_async, NULL);
    setup_scan_running = false;
    gui_setup_plugins_teardown();
    gui_popup_teardown(&language_popup);
    language_list = NULL;
    plugin_network_notice = NULL;
    plugin_catalog_notice = NULL;
    memset(plugin_download_rows, 0, sizeof(plugin_download_rows));
    if (plugin_network_timer) { lv_timer_delete(plugin_network_timer); plugin_network_timer = NULL; }
    if (layout_catalog_timer) { lv_timer_delete(layout_catalog_timer); layout_catalog_timer = NULL; }
    intro_cleanup();
    if (!setup_screen) return;
    lv_async_call_cancel(render_async, NULL);
    lv_async_call_cancel(finish_async, NULL);
    if (lv_screen_active() == setup_screen) {
        lv_obj_t * home = gui_shell_get_home_screen();
        if (home) lv_screen_load(home);
    }
    lv_obj_delete(setup_screen);
    setup_screen = NULL;
    content = NULL;
    scan_switch = NULL;
    if (current_settings.setup_complete) {
        if (setup_storage_timer) { lv_timer_delete(setup_storage_timer); setup_storage_timer = NULL; }
        setup_started = false;
        setup_finalizing = setup_apply_pending = false;
        staged_layout_plugin[0] = staged_layout_name[0] = '\0';
    }
}
