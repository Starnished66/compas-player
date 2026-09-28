#include "gui_plugin_store.h"

#include "gui_navigation.h"
#include "gui_notifications.h"
#include "gui_reload.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "plugin_store.h"
#include "screen_builders.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ACTION_INSTALL, ACTION_UPDATE, ACTION_REPLACE, ACTION_REMOVE,
               ACTION_CANCEL, ACTION_CLOSE, ACTION_PRIMARY, ACTION_DISMISS } store_action_t;

static lv_obj_t * store_screen;
static lv_obj_t * store_list;
static plugin_store_result_t * store_rows;
static size_t store_row_count;
static bool store_changed_dirty;
static bool store_ui_active;
static bool store_open_when_ready;
static bool store_is_refresh;
static bool store_pending_update_all;
static bool store_push_pending; /* open the store once the busy screen's pop has finished */
static bool store_confirm_pending; /* likewise for the replace-files question */
static int store_selected_index = -1;
static store_action_t detail_primary_action;
static char store_selected_id[64];
static gui_busy_handle_t store_busy;
static gui_popup_t detail_popup, remove_popup, confirm_popup;
static lv_obj_t * detail_title, *detail_body, *remove_title, *confirm_title;

static void populate_store_screen(void);

static bool result_is_installed(const plugin_store_result_t * row) {
    return row->state == PLUGIN_STORE_PLUGIN_INSTALLED ||
           row->state == PLUGIN_STORE_PLUGIN_UPDATE ||
           row->state == PLUGIN_STORE_PLUGIN_REMOVED;
}

/* The store was left (Back, Home), not just covered by the busy screen. */
static bool store_left(void) {
    return !store_screen || !gui_navigation_contains(store_screen);
}

static void apply_changes(void) {
    store_changed_dirty = false;
    show_info_toast("Refreshing plugins...");
    gui_reload_request();
}

/* Plugins changed on the card: reload now if the user already left the
 * store, otherwise once they leave it. */
static void note_changed(bool changed) {
    if (!changed) return;
    store_changed_dirty = true;
    if (store_left()) apply_changes();
}

static void store_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    if (store_changed_dirty && store_left()) apply_changes();
}

static void store_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    populate_store_screen();
}

static void store_action_cb(lv_event_t * e);

static lv_obj_t * add_action_row(lv_obj_t * popup, const char * text, lv_color_t color,
                                 store_action_t action) {
    lv_obj_t * row = lv_obj_create(popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, store_action_cb, LV_EVENT_CLICKED, (void *) (intptr_t) action);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(label);
    return row;
}

static void popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&detail_popup);
    gui_popup_hide(&remove_popup);
    gui_popup_hide(&confirm_popup);
}

static void create_popup(gui_popup_t * popup, lv_obj_t ** title_out, lv_obj_t ** body_out,
                         bool with_body) {
    lv_obj_t * top = lv_layer_top();
    popup->backdrop = lv_obj_create(top);
    lv_obj_set_size(popup->backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(popup->backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup->backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(popup->backdrop, 0, 0);
    lv_obj_remove_flag(popup->backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(popup->backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(popup->backdrop, popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    popup->popup = lv_obj_create(top);
    lv_obj_set_width(popup->popup, lv_pct(84));
    lv_obj_set_height(popup->popup, LV_SIZE_CONTENT);
    lv_obj_align(popup->popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(popup->popup, 16, 0);
    lv_obj_add_style(popup->popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(popup->popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup->popup, 0, 0);
    lv_obj_set_style_pad_all(popup->popup, 20, 0);
    lv_obj_set_style_pad_row(popup->popup, 10, 0);
    /* A long description scrolls inside the popup instead of pushing the
     * buttons off a small screen. */
    lv_obj_set_style_max_height(popup->popup, lv_pct(90), 0);
    lv_obj_add_flag(popup->popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(popup->popup, LV_DIR_VER);
    lv_obj_add_flag(popup->popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup->popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup->popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title = lv_label_create(popup->popup);
    lv_label_set_text(title, "");
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (title_out) *title_out = title;

    if (with_body) {
        lv_obj_t * body = lv_label_create(popup->popup);
        lv_label_set_text(body, "");
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_style(body, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        if (body_out) *body_out = body;
    }
}

static void build_popups(void) {
    create_popup(&detail_popup, &detail_title, &detail_body, true);
    add_action_row(detail_popup.popup, "Install", accent_lv_color(), ACTION_PRIMARY);
    add_action_row(detail_popup.popup, "Update", accent_lv_color(), ACTION_UPDATE);
    add_action_row(detail_popup.popup, "Replace", accent_lv_color(), ACTION_REPLACE);
    add_action_row(detail_popup.popup, "Remove", lv_color_make(255, 120, 120), ACTION_REMOVE);
    add_action_row(detail_popup.popup, "Cancel", accent_lv_color(), ACTION_CANCEL);

    create_popup(&remove_popup, &remove_title, NULL, false);
    lv_obj_t * body = lv_label_create(remove_popup.popup);
    lv_label_set_text(body, "Its settings stay on the card.");
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    add_action_row(remove_popup.popup, "Remove", lv_color_make(255, 120, 120), ACTION_REMOVE);
    add_action_row(remove_popup.popup, "Cancel", accent_lv_color(), ACTION_CANCEL);

    create_popup(&confirm_popup, &confirm_title, NULL, false);
    add_action_row(confirm_popup.popup, "Replace", accent_lv_color(), ACTION_REPLACE);
    add_action_row(confirm_popup.popup, "Update individually", accent_lv_color(), ACTION_CLOSE);
    add_action_row(confirm_popup.popup, "Cancel", accent_lv_color(), ACTION_CANCEL);
}

static const char * row_status(const plugin_store_result_t * row, char * out, size_t cap) {
    if (row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE)
        return "Needs newer firmware";
    switch (row->state) {
        case PLUGIN_STORE_PLUGIN_UPDATE:
            snprintf(out, cap, "Update to %s", row->version);
            return out;
        case PLUGIN_STORE_PLUGIN_INSTALLED:
        case PLUGIN_STORE_PLUGIN_REMOVED:
            snprintf(out, cap, "%s", row->version[0] ? row->version : "Installed");
            return out;
        case PLUGIN_STORE_PLUGIN_MANUAL: return "Installed manually";
        default: return "";
    }
}

static void show_details(void) {
    if (store_selected_index < 0 || (size_t) store_selected_index >= store_row_count) return;
    const plugin_store_result_t * row = &store_rows[store_selected_index];
    snprintf(store_selected_id, sizeof(store_selected_id), "%s", row->id);
    plugin_store_details_t details = {0};
    bool has_details = plugin_store_get_details(row->id, &details);
    char title[128], body[512];
    snprintf(title, sizeof(title), "%s\n%s", row->name[0] ? row->name : row->id,
             row->version[0] ? row->version : "");
    snprintf(body, sizeof(body), "%s%s%s", has_details ? details.description : "",
             has_details && details.author[0] ? "\n\nBy " : "",
             has_details && details.author[0] ? details.author : "");
    lv_label_set_text(detail_title, title);
    lv_label_set_text(detail_body, body);

    bool installed = result_is_installed(row);
    bool manual = row->state == PLUGIN_STORE_PLUGIN_MANUAL;
    bool update = row->state == PLUGIN_STORE_PLUGIN_UPDATE;
    bool removed = row->state == PLUGIN_STORE_PLUGIN_REMOVED;
    bool incompatible = row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE;
    lv_obj_t * primary = lv_obj_get_child(detail_popup.popup, 2);
    lv_obj_t * update_row = lv_obj_get_child(detail_popup.popup, 3);
    lv_obj_t * replace_row = lv_obj_get_child(detail_popup.popup, 4);
    lv_obj_t * remove_row = lv_obj_get_child(detail_popup.popup, 5);
    if (primary) {
        lv_label_set_text(lv_obj_get_child(primary, 0), incompatible ? "Close" :
                          manual ? "Replace" : update ? "Update" : "Install");
        detail_primary_action = incompatible ? ACTION_DISMISS : update ? ACTION_UPDATE :
                                manual ? ACTION_REPLACE : ACTION_INSTALL;
        if (removed || (installed && !update && !incompatible)) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(primary, LV_OBJ_FLAG_HIDDEN);
    }
    if (update_row) lv_obj_add_flag(update_row, LV_OBJ_FLAG_HIDDEN);
    if (replace_row) lv_obj_add_flag(replace_row, LV_OBJ_FLAG_HIDDEN);
    if (remove_row && installed) lv_obj_clear_flag(remove_row, LV_OBJ_FLAG_HIDDEN);
    else if (remove_row) lv_obj_add_flag(remove_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(detail_popup.popup, 0, LV_ANIM_OFF); /* reopened for another plugin */
    gui_popup_show(&detail_popup);
}

static void store_row_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_selected_index = (int) (intptr_t) lv_event_get_user_data(e);
    show_details();
}

static void add_plugin_row(int index) {
    plugin_store_result_t * row = &store_rows[index];
    char status[64];
    const char * secondary = row_status(row, status, sizeof(status));
    lv_obj_t * pill = add_pill_row_base(store_list, row->name[0] ? row->name : row->id);
    lv_obj_set_height(pill, GUI_SETTINGS_ROW_HEIGHT + 8);
    lv_obj_t * primary = lv_obj_get_child(pill, 0);
    lv_obj_align(primary, LV_ALIGN_TOP_LEFT, GUI_TEXT_INSET, 10);
    lv_obj_set_width(primary, pill_row_default_width() - 120);
    lv_label_set_long_mode(primary, LV_LABEL_LONG_DOT);
    if (secondary[0]) {
        lv_obj_t * label = lv_label_create(pill);
        lv_label_set_text(label, secondary);
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, GUI_TEXT_INSET, -9);
    }
    lv_obj_t * chevron = lv_label_create(pill);
    lv_label_set_text(chevron, ">");
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, store_row_clicked_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static void update_all_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!plugin_store_update_all()) {
        show_error_toast("A plugin operation is already in progress");
        return;
    }
    store_ui_active = true;
    store_is_refresh = false;
    store_pending_update_all = true;
    store_busy = gui_busy_show("Updating plugins", "This may take a while");
    gui_busy_set_progress(store_busy, 0);
}

static void populate_store_screen(void) {
    if (!store_screen || !store_list || !store_rows) return;
    plugin_store_status_t status;
    plugin_store_get_status(&status, store_rows, PLUGIN_STORE_MAX_RESULTS);
    store_row_count = status.result_count;
    lv_obj_clean(store_list);

    bool updates = false;
    for (size_t i = 0; i < store_row_count; i++)
        if (store_rows[i].state == PLUGIN_STORE_PLUGIN_UPDATE && !store_rows[i].incompatible) updates = true;
    if (updates) {
        add_section_header(store_list, "Updates");
        add_pill_chevron_row(store_list, "Update All", update_all_cb);
        for (size_t i = 0; i < store_row_count; i++)
            if (store_rows[i].state == PLUGIN_STORE_PLUGIN_UPDATE && !store_rows[i].incompatible)
                add_plugin_row((int) i);
    }

    bool installed = false;
    for (size_t i = 0; i < store_row_count; i++) if (result_is_installed(&store_rows[i])) installed = true;
    if (installed) {
        add_section_header(store_list, "Installed");
        for (size_t i = 0; i < store_row_count; i++)
            if (result_is_installed(&store_rows[i])) add_plugin_row((int) i);
    }

    bool available = false;
    for (size_t i = 0; i < store_row_count; i++) if (!result_is_installed(&store_rows[i])) available = true;
    if (available) {
        add_section_header(store_list, "Available");
        for (size_t i = 0; i < store_row_count; i++)
            if (!result_is_installed(&store_rows[i])) add_plugin_row((int) i);
    }
}

static bool start_operation(plugin_store_state_t state, bool force) {
    bool started = state == PLUGIN_STORE_INSTALLING ? plugin_store_install(store_selected_id, force) :
                   state == PLUGIN_STORE_UPDATING ? plugin_store_update(store_selected_id, force) :
                   state == PLUGIN_STORE_UNINSTALLING ? plugin_store_uninstall(store_selected_id) : false;
    if (!started) {
        show_error_toast("Could not start the plugin operation");
        return false;
    }
    store_ui_active = true;
    store_is_refresh = false;
    store_pending_update_all = false;
    const char * title = state == PLUGIN_STORE_INSTALLING ? "Installing plugin" :
                         state == PLUGIN_STORE_UPDATING ? "Updating plugins" : "Removing plugin";
    store_busy = gui_busy_show(title, "This may take a while");
    gui_busy_set_progress(store_busy, 0);
    return true;
}

static void store_action_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_action_t action = (store_action_t) (intptr_t) lv_event_get_user_data(e);
    if (action == ACTION_CANCEL) {
        gui_popup_hide(&detail_popup);
        gui_popup_hide(&remove_popup);
        gui_popup_hide(&confirm_popup);
        return;
    }
    if (action == ACTION_REMOVE && gui_popup_is_visible(&detail_popup)) {
        const plugin_store_result_t * row = &store_rows[store_selected_index];
        char title[128];
        snprintf(title, sizeof(title), "Remove %s?", row->name[0] ? row->name : row->id);
        lv_label_set_text(remove_title, title);
        gui_popup_hide(&detail_popup);
        gui_popup_show(&remove_popup);
        return;
    }
    if (action == ACTION_REMOVE && gui_popup_is_visible(&remove_popup)) {
        gui_popup_hide(&remove_popup);
        (void) start_operation(PLUGIN_STORE_UNINSTALLING, false);
        return;
    }
    if (action == ACTION_REPLACE && gui_popup_is_visible(&confirm_popup)) {
        gui_popup_hide(&confirm_popup);
        if (store_pending_update_all) {
            show_info_toast("Update these plugins individually");
            store_pending_update_all = false;
        } else if (store_selected_index >= 0 && (size_t) store_selected_index < store_row_count) {
            (void) start_operation(store_rows[store_selected_index].state == PLUGIN_STORE_PLUGIN_UPDATE ?
                                   PLUGIN_STORE_UPDATING : PLUGIN_STORE_INSTALLING, true);
        }
        return;
    }
    if (action == ACTION_CLOSE) {
        gui_popup_hide(&detail_popup);
        gui_popup_hide(&confirm_popup);
        show_info_toast("Update these plugins individually");
        store_pending_update_all = false;
        return;
    }
    if (action == ACTION_DISMISS) {
        gui_popup_hide(&detail_popup);
        return;
    }
    if (action == ACTION_PRIMARY) action = detail_primary_action;
    if (gui_popup_is_visible(&confirm_popup)) return;
    gui_popup_hide(&detail_popup);
    if (action == ACTION_INSTALL || action == ACTION_REPLACE)
        (void) start_operation(PLUGIN_STORE_INSTALLING, false);
    else if (action == ACTION_UPDATE)
        (void) start_operation(PLUGIN_STORE_UPDATING, false);
}

void gui_plugin_store_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!store_rows) {
        show_error_toast("Not enough memory to load the plugin store");
        return;
    }
    if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast("Turn on Wi-Fi and connect first");
        return;
    }
    if (store_ui_active || plugin_store_busy()) {
        show_error_toast("A plugin operation is already in progress");
        return;
    }
    if (!plugin_store_refresh()) {
        show_error_toast("Could not start the plugin refresh");
        return;
    }
    store_ui_active = true;
    store_is_refresh = true;
    store_open_when_ready = true;
    store_busy = gui_busy_show("Loading plugins", "This may take a while");
}

void poll_plugin_store(void) {
    /* Home drops a covered store from the stack without unloading it. */
    if (store_changed_dirty && !store_ui_active && !store_push_pending && store_left()) apply_changes();
    if (store_push_pending) {
        /* Pushing while the busy screen still slides away would let that
         * animation load Plugin Manager over the store. */
        if (gui_navigation_transition_in_progress()) return;
        store_push_pending = false;
        nav_push(store_screen);
        return;
    }
    if (store_confirm_pending) {
        /* Replace starts a new busy screen, which is not pushed while the
         * old one is still sliding away: ask only once it is gone. */
        if (gui_navigation_transition_in_progress()) return;
        store_confirm_pending = false;
        gui_popup_show(&confirm_popup);
        return;
    }
    if (!store_ui_active) return;
    plugin_store_status_t status;
    plugin_store_get_status(&status, NULL, 0);
    if (status.state == PLUGIN_STORE_IDLE) {
        /* Reset underneath us (a UI reload): nothing left to report. */
        gui_busy_hide(store_busy);
        store_ui_active = false;
        store_open_when_ready = false;
        store_pending_update_all = false;
        return;
    }
    if (status.state == PLUGIN_STORE_REFRESHING || status.state == PLUGIN_STORE_INSTALLING ||
        status.state == PLUGIN_STORE_UPDATING || status.state == PLUGIN_STORE_UPDATING_ALL ||
        status.state == PLUGIN_STORE_UNINSTALLING) {
        gui_busy_set_progress(store_busy, status.percent);
        return;
    }
    if (status.state == PLUGIN_STORE_NEEDS_CONFIRM) {
        gui_busy_hide(store_busy);
        store_ui_active = false;
        note_changed(status.changed);
        lv_label_set_text(confirm_title, "Some plugin files were changed on the card. Replace them?");
        /* Update All cannot force a batch: offer "Update individually"
         * instead of Replace. Both rows are set each time. */
        lv_obj_t * replace = lv_obj_get_child(confirm_popup.popup, 1);
        lv_obj_t * individual = lv_obj_get_child(confirm_popup.popup, 2);
        if (replace) lv_obj_set_flag(replace, LV_OBJ_FLAG_HIDDEN, store_pending_update_all);
        if (individual) lv_obj_set_flag(individual, LV_OBJ_FLAG_HIDDEN, !store_pending_update_all);
        store_confirm_pending = true;
        return;
    }
    if (status.state == PLUGIN_STORE_FAILED) {
        gui_busy_hide(store_busy);
        store_ui_active = false;
        store_open_when_ready = false;
        store_pending_update_all = false;
        note_changed(status.changed); /* Update All may have updated some before failing */
        populate_store_screen();
        show_error_toast(status.error[0] ? status.error : "Plugin operation failed");
        return;
    }
    if (status.state == PLUGIN_STORE_READY) {
        gui_busy_hide(store_busy);
        store_ui_active = false;
        if (!store_is_refresh) note_changed(status.changed);
        populate_store_screen();
        if (store_open_when_ready) {
            store_open_when_ready = false;
            store_push_pending = true;
        }
        store_is_refresh = false;
        store_pending_update_all = false;
    }
}

void gui_plugin_store_init(void) {
    store_rows = calloc(PLUGIN_STORE_MAX_RESULTS, sizeof(*store_rows));
    store_row_count = 0;
    lv_obj_t * title = NULL;
    store_screen = build_subsonic_list_screen("Plugin Store", &title, &store_list);
    (void) title;
    lv_obj_add_event_cb(store_screen, store_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(store_screen, store_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    build_popups();
}

void gui_plugin_store_teardown(void) {
    gui_popup_teardown(&detail_popup);
    gui_popup_teardown(&remove_popup);
    gui_popup_teardown(&confirm_popup);
    if (store_screen) {
        lv_obj_delete(store_screen);
        store_screen = NULL;
        store_list = NULL;
    }
    free(store_rows);
    store_rows = NULL;
    store_row_count = 0;
    store_push_pending = false;
    store_confirm_pending = false;
    /* A running worker keeps reporting to the rebuilt screens; otherwise
     * start over. */
    if (!plugin_store_busy()) {
        plugin_store_reset();
        store_ui_active = false;
        store_open_when_ready = false;
        store_pending_update_all = false;
    }
}
