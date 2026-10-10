#include "gui_plugin_store.h"
#include "assets.h"
#include "i18n.h"

#include "gui_navigation.h"
#include "gui_notifications.h"
#include "gui_reload.h"
#include "gui_shell.h"
#include "gui_network.h"
#include "gui_theme.h"
#include "plugin_store.h"
#include "screen_builders.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ACTION_INSTALL, ACTION_UPDATE, ACTION_REPLACE, ACTION_REMOVE,
               ACTION_CANCEL, ACTION_CLOSE, ACTION_PRIMARY, ACTION_DISMISS,
               ACTION_PICKER_DONE, ACTION_PICKER_CANCEL } store_action_t;

static lv_obj_t * store_screen;
static lv_obj_t * store_list;
static lv_obj_t * store_title;
static plugin_store_result_t * store_rows;
static size_t store_row_count;
static bool store_changed_dirty;
static bool store_ui_active;
static bool store_open_when_ready;
static lv_obj_t * store_open_origin;
static bool store_is_refresh;
static gui_plugin_store_view_t store_view;
static bool store_pending_update_all;
static bool store_push_pending; /* open the store once the busy screen's pop has finished */
static bool store_confirm_pending; /* likewise for the replace-files question */
static bool store_picker_mode;
static bool store_picker_pending;
static bool store_picker_refresh_pending;
static lv_obj_t * store_picker_origin;
static bool (*store_picker_is_selected)(const char * id);
static void (*store_picker_toggle)(const char * id, const char * name);
static void (*store_picker_done)(void);
static bool store_layout_picker_mode;
static bool store_layout_picker_pending;
static lv_obj_t * store_layout_picker_origin;
static bool (*store_layout_is_selected)(const char * id);
static void (*store_layout_select)(const char * id, const char * name);
static bool store_setup_catalog_pending;
static bool store_setup_catalog_ready;
static uint64_t store_preview_generation_seen;
static int store_selected_index = -1;
static store_action_t detail_primary_action;
static char store_selected_id[64];
static gui_busy_handle_t store_busy;
static gui_popup_t detail_popup, remove_popup, confirm_popup, picker_popup;
static lv_obj_t * detail_title, *detail_body, *remove_title, *confirm_title, *picker_title, *picker_list;
static lv_obj_t * store_header_review_updates_btn;

static void populate_store_screen(bool preserve_scroll);
static bool start_catalog_refresh(gui_plugin_store_view_t view);
static void player_layout_refresh_cb(lv_event_t * e);
static void layout_picker_card_cb(lv_event_t * e);
static void review_updates_cb(lv_event_t * e);
static void store_updates_refresh_cb(lv_event_t * e);
static void theme_store_refresh_cb(lv_event_t * e);

static void set_store_title(void) {
    if (!store_title) return;
    if (store_view == STORE_VIEW_LAYOUTS)
        lv_label_set_text(store_title, TR("Download layouts"));
    else if (store_view == STORE_VIEW_THEMES)
        lv_label_set_text(store_title, TR("Download themes"));
    else if (store_view == STORE_VIEW_UPDATES)
        lv_label_set_text(store_title, TR("Extension updates"));
    else
        lv_label_set_text(store_title, TR("Plugin Store"));
}

static void clear_picker_request(void) {
    store_picker_mode = false;
    store_picker_pending = false;
    store_picker_refresh_pending = false;
    store_picker_origin = NULL;
    store_picker_is_selected = NULL;
    store_picker_toggle = NULL;
    store_picker_done = NULL;
}

static void clear_layout_picker_request(void) {
    store_layout_picker_mode = false;
    store_layout_picker_pending = false;
    store_layout_picker_origin = NULL;
    store_layout_is_selected = NULL;
    store_layout_select = NULL;
}

bool gui_plugin_store_operation_active(void) {
    return store_ui_active || plugin_store_busy() || store_push_pending || store_confirm_pending ||
           store_picker_pending || store_picker_mode || store_layout_picker_mode ||
           store_layout_picker_pending || store_setup_catalog_pending;
}

static bool result_is_installed(const plugin_store_result_t * row) {
    return row->state == PLUGIN_STORE_PLUGIN_INSTALLED ||
           row->state == PLUGIN_STORE_PLUGIN_UPDATE ||
           row->state == PLUGIN_STORE_PLUGIN_REMOVED;
}

/* The store was left (Back, Home), not just covered by the busy screen. */
static bool store_left(void) {
    return !store_screen || !gui_navigation_contains(store_screen);
}

static bool store_preview_popup_visible(void) {
    return gui_popup_is_visible(&detail_popup) || gui_popup_is_visible(&remove_popup) ||
           gui_popup_is_visible(&confirm_popup) || gui_popup_is_visible(&picker_popup);
}

static void reset_store_list_to_rows(void) {
    lv_obj_set_flex_flow(store_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(store_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(store_list, 0, 0);
    lv_obj_set_style_pad_right(store_list, 0, 0);
    lv_obj_set_style_pad_column(store_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_row(store_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(store_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(store_list, BOARD_SCALE_PX(8), 0);
}

static bool store_can_prepare_previews(void) {
    /* An arriving preview must not replace the card under a pressed finger
     * or interrupt a scroll gesture while rebuilding the grid. */
    if (store_list && lv_obj_is_scrolling(store_list)) return false;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return false;
    return (store_view == STORE_VIEW_LAYOUTS || store_view == STORE_VIEW_THEMES) && store_screen && store_list &&
           gui_navigation_is_top(store_screen) && !gui_navigation_transition_in_progress() &&
           !store_ui_active && !plugin_store_busy() && !store_push_pending &&
           !store_confirm_pending && !store_picker_pending && !store_picker_mode &&
           !store_preview_popup_visible();
}

static void store_prepare_previews_if_ready(plugin_store_state_t state) {
    if (state != PLUGIN_STORE_READY || !store_can_prepare_previews()) return;
    const char * ids[PLUGIN_STORE_PREVIEW_CAPACITY];
    size_t count = 0;
    lv_obj_update_layout(store_list);
    lv_area_t viewport;
    lv_obj_get_coords(store_list, &viewport);
    lv_area_t nearby = viewport;
    int32_t half_screen = lv_display_get_vertical_resolution(lv_display_get_default()) / 2;
    nearby.y1 -= half_screen;
    nearby.y2 += half_screen;

    /* Prioritize the cards the user can see, then prepare the next screenful
     * in either scroll direction while keeping the request bounded. */
    for (int pass = 0; pass < 2 && count < PLUGIN_STORE_PREVIEW_CAPACITY; pass++) {
        uint32_t child_count = lv_obj_get_child_count(store_list);
        for (uint32_t child_i = 0; child_i < child_count && count < PLUGIN_STORE_PREVIEW_CAPACITY; child_i++) {
            lv_obj_t * card = lv_obj_get_child(store_list, (int32_t) child_i);
            intptr_t encoded_index = (intptr_t) lv_obj_get_user_data(card);
            if (encoded_index <= 0 || (size_t) (encoded_index - 1) >= store_row_count) continue;
            int index = (int) (encoded_index - 1);
            if (!store_rows[index].player_layout && !store_rows[index].theme_pack) continue;
            lv_area_t card_area;
            lv_obj_get_coords(card, &card_area);
            if (card_area.x2 < viewport.x1 || card_area.x1 > viewport.x2) continue;
            bool visible = card_area.y2 >= viewport.y1 && card_area.y1 <= viewport.y2;
            bool near = card_area.y2 >= nearby.y1 && card_area.y1 <= nearby.y2;
            if ((pass == 0 && !visible) || (pass == 1 && (!near || visible))) continue;
            ids[count++] = store_rows[index].id;
        }
    }
    if (count) (void) plugin_store_prepare_previews(ids, count);
}

static void apply_changes(void) {
    store_changed_dirty = false;
    show_info_toast(TR("Refreshing plugins..."));
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
    plugin_store_cancel_previews();
    if (store_picker_mode && store_left()) clear_picker_request();
    if (store_layout_picker_mode && store_left()) clear_layout_picker_request();
    if (store_changed_dirty && store_left()) apply_changes();
}

static void store_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    populate_store_screen(false);
}

static void store_action_cb(lv_event_t * e);

static lv_obj_t * add_action_row(lv_obj_t * popup, const char * text, lv_color_t color,
                                 store_action_t action) {
    lv_obj_t * row = lv_obj_create(popup);
    style_popup_action_row(row);
    lv_obj_add_event_cb(row, store_action_cb, LV_EVENT_CLICKED, (void *) (intptr_t) action);
    lv_obj_t * label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&detail_popup);
    gui_popup_hide(&remove_popup);
    gui_popup_hide(&confirm_popup);
    gui_popup_hide(&picker_popup);
    clear_picker_request();
}

static void create_popup(gui_popup_t * popup, lv_obj_t ** title_out, lv_obj_t ** body_out,
                         bool with_body) {
    popup->popup = build_popup_surface(popup_backdrop_cb, &popup->backdrop);
    /* A long description scrolls inside the popup instead of pushing the
     * buttons off a small screen. */

    lv_obj_t * title = lv_label_create(popup->popup);
    lv_label_set_text(title, "");
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (title_out) *title_out = title;

    if (with_body) {
        lv_obj_t * scroll = lv_obj_create(popup->popup);
        lv_obj_remove_style_all(scroll);
        lv_obj_set_width(scroll, lv_pct(100));
        lv_obj_set_height(scroll, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(scroll, BOARD_SCALE_PX(180), 0);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_t * body = lv_label_create(scroll);
        lv_label_set_text(body, "");
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_style(body, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        if (body_out) *body_out = body;
        lv_obj_remove_flag(popup->popup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(popup->popup, LV_DIR_NONE);
    }
}

static void build_popups(void) {
    create_popup(&detail_popup, &detail_title, &detail_body, true);
    add_action_row(detail_popup.popup, TR("Install"), accent_lv_color(), ACTION_PRIMARY);
    add_action_row(detail_popup.popup, TR("Update"), accent_lv_color(), ACTION_UPDATE);
    add_action_row(detail_popup.popup, TR("Replace"), accent_lv_color(), ACTION_REPLACE);
    add_action_row(detail_popup.popup, TR("Remove"), lv_color_make(255, 120, 120), ACTION_REMOVE);
    add_action_row(detail_popup.popup, TR("Cancel"), lv_color_make(255, 120, 120), ACTION_CANCEL);

    create_popup(&remove_popup, &remove_title, NULL, false);
    lv_obj_t * body = lv_label_create(remove_popup.popup);
    lv_label_set_text(body, TR("Its settings stay on the card."));
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    add_action_row(remove_popup.popup, TR("Remove"), lv_color_make(255, 120, 120), ACTION_REMOVE);
    add_action_row(remove_popup.popup, TR("Cancel"), lv_color_make(255, 120, 120), ACTION_CANCEL);

    create_popup(&confirm_popup, &confirm_title, NULL, false);
    add_action_row(confirm_popup.popup, TR("Replace"), accent_lv_color(), ACTION_REPLACE);
    add_action_row(confirm_popup.popup, TR("Update individually"), accent_lv_color(), ACTION_CLOSE);
    add_action_row(confirm_popup.popup, TR("Cancel"), lv_color_make(255, 120, 120), ACTION_CANCEL);

    create_popup(&picker_popup, &picker_title, NULL, false);
    lv_label_set_text(picker_title, TR("Choose plugins"));
    picker_list = lv_obj_create(picker_popup.popup);
    lv_obj_remove_style_all(picker_list);
    lv_obj_set_width(picker_list, lv_pct(100));
    lv_obj_set_height(picker_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(picker_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(picker_list, BOARD_SCALE_PX(6), 0);
    lv_obj_set_scroll_dir(picker_list, LV_DIR_VER);
    lv_obj_add_flag(picker_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(picker_list, LV_OBJ_FLAG_CLICKABLE);
    int32_t popup_height = lv_display_get_vertical_resolution(lv_display_get_default()) -
                           2 * STATUS_BAR_CLEARANCE;
    int32_t picker_fixed_height = 2 * BOARD_SCALE_PX(12) +
        lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW)) +
        3 * GUI_ROW_GAP + 2 * BOARD_SCALE_PX(88);
    int32_t popup_list_max = popup_height - picker_fixed_height;
    lv_obj_set_style_max_height(picker_list, LV_MAX(BOARD_SCALE_PX(48), popup_list_max), 0);
    add_action_row(picker_popup.popup, TR("Done"), accent_lv_color(), ACTION_PICKER_DONE);
    add_action_row(picker_popup.popup, TR("Cancel"), lv_color_make(255, 120, 120), ACTION_PICKER_CANCEL);
    lv_obj_remove_flag(picker_popup.popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(picker_popup.popup, LV_DIR_NONE);
}

static const char * plugin_status_label(const plugin_store_result_t * row) {
    if (row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE)
        return TR("Needs newer firmware");
    switch (row->state) {
        case PLUGIN_STORE_PLUGIN_UPDATE:
            return TR("Update available");
        case PLUGIN_STORE_PLUGIN_INSTALLED:
            return TR("Installed");
        case PLUGIN_STORE_PLUGIN_REMOVED:
            return TR("Removed");
        case PLUGIN_STORE_PLUGIN_MANUAL:
            return TR("Installed manually");
        default:
            return TR("Available");
    }
}

static const char * row_status(const plugin_store_result_t * row, char * out, size_t cap) {
    if (row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE)
        return plugin_status_label(row);
    switch (row->state) {
        case PLUGIN_STORE_PLUGIN_UPDATE:
            snprintf(out, cap, TR("Update available · %s"), row->version);
            return out;
        case PLUGIN_STORE_PLUGIN_INSTALLED:
            if (row->version[0]) {
                snprintf(out, cap, TR("Installed · %s"), row->version);
                return out;
            }
            return plugin_status_label(row);
        case PLUGIN_STORE_PLUGIN_REMOVED:
        case PLUGIN_STORE_PLUGIN_MANUAL:
            return plugin_status_label(row);
        default:
            if (row->version[0]) {
                snprintf(out, cap, TR("Available · %s"), row->version);
                return out;
            }
            return plugin_status_label(row);
    }
}

static void show_details(void) {
    if (store_selected_index < 0 || (size_t) store_selected_index >= store_row_count) return;
    const plugin_store_result_t * row = &store_rows[store_selected_index];
    snprintf(store_selected_id, sizeof(store_selected_id), "%s", row->id);
    plugin_store_details_t details = {0};
    bool has_details = plugin_store_get_details(row->id, &details);
    char title[128], body[800], metadata[160];
    snprintf(title, sizeof(title), "%s", row->name[0] ? row->name : row->id);
    const char * state = plugin_status_label(row);
    if (row->version[0]) snprintf(metadata, sizeof(metadata), TR("Version %s · %s"), row->version, state);
    else snprintf(metadata, sizeof(metadata), "%s", state);
    char author_line[160] = "";
    if (has_details && details.author[0]) snprintf(author_line, sizeof(author_line), TR("By %s"), details.author);
    snprintf(body, sizeof(body), "%s\n\n%s%s%s",
             has_details && details.description[0] ? details.description : TR("No description"), metadata,
             author_line[0] ? "\n\n" : "", author_line);
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
        lv_label_set_text(lv_obj_get_child(primary, 0), incompatible ? TR("Close") :
                          manual ? TR("Replace") : update ? TR("Update") : TR("Install"));
        detail_primary_action = incompatible ? ACTION_DISMISS : update ? ACTION_UPDATE :
                                manual ? ACTION_REPLACE : ACTION_INSTALL;
        if (removed || (installed && !update && !incompatible)) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(primary, LV_OBJ_FLAG_HIDDEN);
    }
    if (update_row) lv_obj_add_flag(update_row, LV_OBJ_FLAG_HIDDEN);
    if (replace_row) lv_obj_add_flag(replace_row, LV_OBJ_FLAG_HIDDEN);
    if (remove_row && installed) lv_obj_clear_flag(remove_row, LV_OBJ_FLAG_HIDDEN);
    else if (remove_row) lv_obj_add_flag(remove_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(lv_obj_get_parent(detail_body), 0, LV_ANIM_OFF);
    gui_popup_show(&detail_popup);
}

static void store_row_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_selected_index = (int) (intptr_t) lv_event_get_user_data(e);
    show_details();
}

static void picker_row_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !store_picker_mode || !store_picker_toggle) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || (size_t) index >= store_row_count) return;
    const plugin_store_result_t * row = &store_rows[index];
    if (row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE ||
        row->state == PLUGIN_STORE_PLUGIN_REMOVED) return;
    store_picker_toggle(row->id, row->name[0] ? row->name : row->id);
    /* Rebuild on the next poll, after LVGL has finished dispatching this row's click. */
    store_picker_refresh_pending = true;
}

static bool layout_picker_row_enabled(const plugin_store_result_t * row) {
    return row && row->player_layout && !row->incompatible &&
           row->state != PLUGIN_STORE_PLUGIN_INCOMPATIBLE &&
           row->state != PLUGIN_STORE_PLUGIN_REMOVED;
}

static void layout_picker_card_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !store_layout_picker_mode ||
        !store_layout_select) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || (size_t) index >= store_row_count ||
        !layout_picker_row_enabled(&store_rows[index])) return;
    const plugin_store_result_t * row = &store_rows[index];
    void (*select)(const char *, const char *) = store_layout_select;
    char id[sizeof(row->id)], name[sizeof(row->name)];
    snprintf(id, sizeof(id), "%s", row->id);
    snprintf(name, sizeof(name), "%s", row->name[0] ? row->name : row->id);
    clear_layout_picker_request();
    select(id, name);
    nav_pop();
}

typedef struct {
    char id[64];
    void (*select)(const char * id, const char * name);
} layout_suggestion_ctx_t;

static void layout_suggestion_card_cb(lv_event_t * e) {
    layout_suggestion_ctx_t * ctx = lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !ctx || !ctx->select) return;
    const plugin_store_result_t * row = NULL;
    for (size_t i = 0; i < store_row_count; ++i) {
        if (strcmp(store_rows[i].id, ctx->id) == 0 && layout_picker_row_enabled(&store_rows[i])) {
            row = &store_rows[i];
            break;
        }
    }
    if (!row) return;
    char id[sizeof(row->id)], name[sizeof(row->name)];
    snprintf(id, sizeof(id), "%s", row->id);
    snprintf(name, sizeof(name), "%s", row->name[0] ? row->name : row->id);
    ctx->select(id, name);
}

static void layout_suggestion_card_delete_cb(lv_event_t * e) {
    lv_free(lv_event_get_user_data(e));
}

static void add_plugin_row(int index) {
    plugin_store_result_t * row = &store_rows[index];
    plugin_store_details_t details = {0};
    (void) plugin_store_get_details(row->id, &details);
    char status[64];
    const char * secondary = row_status(row, status, sizeof(status));
    lv_obj_t * pill = add_pill_row_base(store_list, row->name[0] ? row->name : row->id);
    lv_obj_delete(lv_obj_get_child(pill, 0)); /* Replace the base row's one-line title. */
    const lv_font_t * body_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    const lv_font_t * subtext_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t subtext_line = lv_font_get_line_height(subtext_font);
    int32_t row_height = lv_font_get_line_height(body_font) + 3 * subtext_line +
                         2 * BOARD_SCALE_PX(4) + 2 * BOARD_SCALE_PX(10);
    if (row_height < GUI_SETTINGS_ROW_HEIGHT + BOARD_SCALE_PX(8))
        row_height = GUI_SETTINGS_ROW_HEIGHT + BOARD_SCALE_PX(8);
    lv_obj_set_height(pill, row_height);

    lv_obj_t * text = lv_obj_create(pill);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, pill_row_default_width() - GUI_TEXT_INSET - BOARD_SCALE_PX(72),
                    LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, BOARD_SCALE_PX(4), 0);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(text);
    lv_label_set_text(title, row->name[0] ? row->name : row->id);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, body_font, 0);
    lv_obj_t * description = lv_label_create(text);
    lv_label_set_text(description, details.description[0] ? details.description : TR("No description"));
    lv_obj_set_width(description, lv_pct(100));
    lv_obj_set_height(description, 2 * subtext_line);
    lv_label_set_long_mode(description, LV_LABEL_LONG_DOT);
    lv_obj_add_style(description, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(description, subtext_font, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(description, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (secondary[0]) {
        lv_obj_t * label = lv_label_create(text);
        lv_label_set_text(label, secondary);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(label, subtext_font, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t * chevron = lv_label_create(pill);
    lv_label_set_text(chevron, ">");
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(20), 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, store_row_clicked_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static void add_picker_popup_row(int index) {
    plugin_store_result_t * row = &store_rows[index];
    plugin_store_details_t details = {0};
    (void) plugin_store_get_details(row->id, &details);
    bool incompatible = row->incompatible || row->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE;
    bool removed = row->state == PLUGIN_STORE_PLUGIN_REMOVED;
    bool disabled = incompatible || removed;
    bool selected = store_picker_is_selected && store_picker_is_selected(row->id);
    char status[80];
    const char * status_text = NULL;
    if (incompatible) status_text = TR("Needs newer firmware");
    else if (removed) status_text = TR("No longer in the catalog");
    else if (row->state == PLUGIN_STORE_PLUGIN_INSTALLED) status_text = TR("Already installed");
    else if (row->state == PLUGIN_STORE_PLUGIN_MANUAL) status_text = TR("Installed manually");
    else if (row->state == PLUGIN_STORE_PLUGIN_UPDATE) {
        if (row->version[0]) snprintf(status, sizeof(status), TR("Update available · %s"), row->version);
        else snprintf(status, sizeof(status), "%s", TR("Update available"));
        status_text = status;
    }
    lv_obj_t * row_obj = lv_obj_create(picker_list);
    lv_obj_set_width(row_obj, lv_pct(100));
    lv_obj_set_height(row_obj, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row_obj, ui_list_row_height(), 0);
    lv_obj_set_style_radius(row_obj, BOARD_SCALE_PX(10), 0);
    lv_obj_add_style(row_obj, &pill_row_bg_style, 0);
    lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row_obj, 0, 0);
    lv_obj_set_style_pad_hor(row_obj, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_ver(row_obj, BOARD_SCALE_PX(6), 0);
    lv_obj_set_style_pad_column(row_obj, BOARD_SCALE_PX(10), 0);
    lv_obj_add_style(row_obj, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row_obj, picker_row_clicked_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);

    lv_obj_t * text = lv_obj_create(row_obj);
    lv_obj_remove_style_all(text);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, BOARD_SCALE_PX(2), 0);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * title = lv_label_create(text);
    lv_label_set_text(title, row->name[0] ? row->name : row->id);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_t * desc = lv_label_create(text);
    lv_label_set_text(desc, removed ? TR("No longer in the catalog") :
                      details.description[0] ? details.description : TR("No description"));
    lv_obj_set_width(desc, lv_pct(100));
    lv_obj_set_height(desc, 2 * lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
    lv_obj_add_style(desc, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(desc, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (status_text) {
        lv_obj_t * status_label = lv_label_create(text);
        lv_label_set_text(status_label, status_text);
        lv_obj_set_width(status_label, lv_pct(100));
        lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
        lv_obj_add_style(status_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_remove_flag(status_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    build_check_box(row_obj, selected);
    if (disabled) {
        lv_obj_set_style_opa(row_obj, LV_OPA_50, 0);
        lv_obj_add_state(row_obj, LV_STATE_DISABLED);
    }
}

static void populate_picker_popup(bool preserve_scroll) {
    if (!picker_list || !store_picker_mode || !store_rows) return;
    int32_t scroll_y = preserve_scroll ? lv_obj_get_scroll_y(picker_list) : 0;
    lv_obj_clean(picker_list);
    bool any = false;
    for (size_t i = 0; i < store_row_count; ++i) {
        if (store_rows[i].player_layout || store_rows[i].theme_pack) continue;
        add_picker_popup_row((int) i);
        any = true;
    }
    if (!any) {
        lv_obj_t * empty = lv_label_create(picker_list);
        lv_label_set_text(empty, TR("No plugins are available in the catalog."));
        lv_obj_add_style(empty, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_update_layout(picker_list);
    lv_obj_scroll_to_y(picker_list, scroll_y, LV_ANIM_OFF);
}

static void show_picker_popup(void) {
    if (!store_picker_mode || !store_picker_is_selected || !store_picker_toggle) return;
    populate_picker_popup(false);
    gui_popup_show(&picker_popup);
}

static void update_all_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (store_view != STORE_VIEW_UPDATES) return;
    if (!plugin_store_update_all()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return;
    }
    store_setup_catalog_ready = false;
    store_ui_active = true;
    store_is_refresh = false;
    store_pending_update_all = true;
    store_busy = gui_busy_show(TR("Updating plugins"), TR("This may take a while"));
    gui_busy_set_progress(store_busy, 0);
}

static void review_updates_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (store_picker_mode || store_layout_picker_mode || gui_plugin_store_operation_active()) return;
    plugin_store_cancel_previews();
    store_view = STORE_VIEW_UPDATES;
    set_store_title();
    populate_store_screen(false);
}

static void store_updates_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return;
    }
    (void) start_catalog_refresh(STORE_VIEW_UPDATES);
}

static void populate_store_screen(bool preserve_scroll) {
    if (!store_screen || !store_list || !store_rows) return;
    int32_t scroll_y = preserve_scroll ? lv_obj_get_scroll_y(store_list) : 0;
    uint64_t generation_snapshot = plugin_store_preview_generation();
    plugin_store_status_t status;
    plugin_store_get_status(&status, store_rows, PLUGIN_STORE_MAX_RESULTS);
    store_row_count = status.result_count;
    reset_store_list_to_rows();
    lv_obj_clean(store_list);

    bool has_any_updates = false;
    for (size_t i = 0; i < store_row_count; i++) {
        if (store_rows[i].state == PLUGIN_STORE_PLUGIN_UPDATE && !store_rows[i].incompatible) {
            has_any_updates = true;
            break;
        }
    }

    if (store_header_review_updates_btn) {
        if (store_view == STORE_VIEW_LAYOUTS && has_any_updates && !store_layout_picker_mode && !store_picker_mode) {
            lv_obj_remove_flag(store_header_review_updates_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(store_header_review_updates_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (store_view == STORE_VIEW_UPDATES) {
        if (!has_any_updates && status.state == PLUGIN_STORE_READY) {
            lv_obj_t * empty = lv_label_create(store_list);
            lv_label_set_text(empty, TR("No updates available."));
            lv_obj_add_style(empty, &style_theme_text_muted, 0);
            lv_obj_set_style_text_font(empty, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
            lv_obj_set_width(empty, lv_pct(90));
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_top(empty, BOARD_SCALE_PX(48), 0);
            add_pill_chevron_row(store_list, TR("Refresh plugin catalog"), store_updates_refresh_cb);
            store_preview_generation_seen = generation_snapshot;
            lv_obj_update_layout(store_list);
            if (preserve_scroll) lv_obj_scroll_to_y(store_list, scroll_y, LV_ANIM_OFF);
            return;
        }

        if (has_any_updates) {
            add_section_header(store_list, TR("Updates"));
            add_pill_chevron_row(store_list, TR("Update All"), update_all_cb);
            for (size_t i = 0; i < store_row_count; i++) {
                if (store_rows[i].state == PLUGIN_STORE_PLUGIN_UPDATE && !store_rows[i].incompatible)
                    add_plugin_row((int) i);
            }
        }
        store_preview_generation_seen = generation_snapshot;
        lv_obj_update_layout(store_list);
        if (preserve_scroll) lv_obj_scroll_to_y(store_list, scroll_y, LV_ANIM_OFF);
        return;
    }

    bool any_visible = false;
    bool visible[PLUGIN_STORE_MAX_RESULTS];
    for (size_t i = 0; i < store_row_count; i++) {
        if (store_view == STORE_VIEW_LAYOUTS)
            visible[i] = store_rows[i].player_layout;
        else if (store_view == STORE_VIEW_THEMES)
            visible[i] = store_rows[i].theme_pack;
        else
            visible[i] = !store_rows[i].player_layout && !store_rows[i].theme_pack;
        if (visible[i]) any_visible = true;
    }

    if (!any_visible && status.state == PLUGIN_STORE_READY) {
        if (store_view == STORE_VIEW_PLUGINS && has_any_updates && !store_picker_mode) {
            add_section_header(store_list, TR("Updates"));
            add_pill_chevron_row(store_list, TR("Review updates"), review_updates_cb);
        }
        lv_obj_t * empty = lv_label_create(store_list);
        if (store_view == STORE_VIEW_LAYOUTS)
            lv_label_set_text(empty, TR("No layouts are available in the catalog."));
        else if (store_view == STORE_VIEW_THEMES)
            lv_label_set_text(empty, TR("No themes are available in the catalog."));
        else
            lv_label_set_text(empty, TR("No plugins are available in the catalog."));
        lv_obj_add_style(empty, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_width(empty, lv_pct(90));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, BOARD_SCALE_PX(48), 0);
        if (!store_layout_picker_mode && !store_picker_mode) {
            lv_event_cb_t refresh_cb = gui_plugin_store_row_cb;
            if (store_view == STORE_VIEW_LAYOUTS) refresh_cb = player_layout_refresh_cb;
            else if (store_view == STORE_VIEW_THEMES) refresh_cb = theme_store_refresh_cb;
            add_pill_chevron_row(store_list, TR("Refresh plugin catalog"), refresh_cb);
        }
        store_preview_generation_seen = generation_snapshot;
        lv_obj_update_layout(store_list);
        if (preserve_scroll) lv_obj_scroll_to_y(store_list, scroll_y, LV_ANIM_OFF);
        return;
    }

    if ((store_view == STORE_VIEW_LAYOUTS || store_view == STORE_VIEW_THEMES) && any_visible) {
        configure_cover_card_grid(store_list, 2);
        if (store_layout_picker_mode) {
            lv_obj_set_style_pad_top(store_list, BOARD_SCALE_PX(6), 0);
            lv_obj_set_style_pad_bottom(store_list, BOARD_SCALE_PX(6), 0);
        }
        for (size_t i = 0; i < store_row_count; i++) {
            if (!visible[i]) continue;
            char preview[512], resolved[520], status_text[96];
            const char * preview_src = NULL;
            if (plugin_store_get_preview(store_rows[i].id, preview, sizeof(preview))) {
                snprintf(resolved, sizeof(resolved), "S:%s", preview);
                preview_src = resolved;
            }
            const char * label = store_rows[i].name[0] ? store_rows[i].name : store_rows[i].id;
            lv_event_cb_t on_click = store_layout_picker_mode ? layout_picker_card_cb : store_row_clicked_cb;
            lv_obj_t * card = add_cover_card(store_list, label, preview_src, 2,
                                              on_click, (void *) (intptr_t) i);
            if (card) lv_obj_set_user_data(card, (void *) (intptr_t) (i + 1));
            if (card && store_layout_picker_mode) {
                bool selected = store_layout_is_selected && store_layout_is_selected(store_rows[i].id);
                lv_obj_set_style_radius(card, BOARD_SCALE_PX(10), 0);
                lv_obj_set_style_outline_width(card, selected ? BOARD_SCALE_PX(2) : 0, 0);
                lv_obj_set_style_outline_color(card, accent_lv_color(), 0);
                lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
                lv_obj_set_style_outline_pad(card, BOARD_SCALE_PX(2), 0);
                if (!layout_picker_row_enabled(&store_rows[i]))
                    lv_obj_set_style_opa(card, LV_OPA_50, 0);
            }
            /* The worker already fits the RGB565 thumbnail to these bounds.
             * File-backed binary images stream by row and cannot transform. */
            if (card && preview_src) {
                lv_obj_t * cover = lv_obj_get_child(card, 0);
                lv_obj_t * image = cover ? lv_obj_get_child(cover, 0) : NULL;
                if (image && lv_obj_check_type(image, &lv_image_class))
                    lv_image_set_scale(image, LV_SCALE_NONE);
            }
            const char * status_label = row_status(&store_rows[i], status_text, sizeof(status_text));
            if (card && status_label && status_label[0]) {
                lv_obj_t * status = lv_label_create(card);
                lv_label_set_text(status, status_label);
                lv_obj_set_width(status, lv_pct(100));
                lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
                lv_obj_add_style(status, &style_theme_text_muted, 0);
                lv_obj_set_style_text_font(status, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
                lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            }
        }
        store_preview_generation_seen = generation_snapshot;
        lv_obj_update_layout(store_list);
        if (preserve_scroll) lv_obj_scroll_to_y(store_list, scroll_y, LV_ANIM_OFF);
        store_prepare_previews_if_ready(status.state);
        return;
    }

    if (has_any_updates && !store_picker_mode) {
        add_section_header(store_list, TR("Updates"));
        add_pill_chevron_row(store_list, TR("Review updates"), review_updates_cb);
        for (size_t i = 0; i < store_row_count; i++)
            if (visible[i] && store_rows[i].state == PLUGIN_STORE_PLUGIN_UPDATE && !store_rows[i].incompatible)
                add_plugin_row((int) i);
    }

    bool installed = false;
    for (size_t i = 0; i < store_row_count; i++) if (visible[i] && result_is_installed(&store_rows[i])) installed = true;
    if (installed) {
        add_section_header(store_list, TR("Installed"));
        for (size_t i = 0; i < store_row_count; i++)
            if (visible[i] && result_is_installed(&store_rows[i])) add_plugin_row((int) i);
    }

    bool available = false;
    for (size_t i = 0; i < store_row_count; i++) if (visible[i] && !result_is_installed(&store_rows[i])) available = true;
    if (available) {
        add_section_header(store_list, TR("Available"));
        for (size_t i = 0; i < store_row_count; i++)
            if (visible[i] && !result_is_installed(&store_rows[i])) add_plugin_row((int) i);
    }
    lv_obj_update_layout(store_list);
    store_preview_generation_seen = generation_snapshot;
    if (preserve_scroll) lv_obj_scroll_to_y(store_list, scroll_y, LV_ANIM_OFF);
}

static bool start_operation(plugin_store_state_t state, bool force) {
    bool started = state == PLUGIN_STORE_INSTALLING ? plugin_store_install(store_selected_id, force) :
                   state == PLUGIN_STORE_UPDATING ? plugin_store_update(store_selected_id, force) :
                   state == PLUGIN_STORE_UNINSTALLING ? plugin_store_uninstall(store_selected_id) : false;
    if (!started) {
        show_error_toast(TR("Could not start the plugin operation"));
        return false;
    }
    store_setup_catalog_ready = false;
    store_ui_active = true;
    store_is_refresh = false;
    store_pending_update_all = false;
    const char * title = state == PLUGIN_STORE_INSTALLING ? TR("Installing plugin") :
                         state == PLUGIN_STORE_UPDATING ? TR("Updating plugins") : TR("Removing plugin");
    store_busy = gui_busy_show(title, TR("This may take a while"));
    gui_busy_set_progress(store_busy, 0);
    return true;
}

static void store_action_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_action_t action = (store_action_t) (intptr_t) lv_event_get_user_data(e);
    if (action == ACTION_PICKER_DONE || action == ACTION_PICKER_CANCEL) {
        void (*done)(void) = action == ACTION_PICKER_DONE ? store_picker_done : NULL;
        gui_popup_hide(&picker_popup);
        clear_picker_request();
        if (done) done();
        return;
    }
    if (action == ACTION_CANCEL) {
        gui_popup_hide(&detail_popup);
        gui_popup_hide(&remove_popup);
        gui_popup_hide(&confirm_popup);
        return;
    }
    if (action == ACTION_REMOVE && gui_popup_is_visible(&detail_popup)) {
        const plugin_store_result_t * row = &store_rows[store_selected_index];
        char title[128];
        snprintf(title, sizeof(title), TR("Remove %s?"), row->name[0] ? row->name : row->id);
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
            show_info_toast(TR("Update these plugins individually"));
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
        show_info_toast(TR("Update these plugins individually"));
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

static bool start_catalog_refresh(gui_plugin_store_view_t view) {
    if (!store_rows) {
        show_error_toast(TR("Not enough memory to load the plugin store"));
        return false;
    }
    if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast(TR("Turn on Wi-Fi and connect first"));
        return false;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    if (!plugin_store_refresh()) {
        show_error_toast(TR("Could not start the plugin refresh"));
        return false;
    }
    store_view = view;
    store_setup_catalog_ready = false;
    store_open_origin = lv_screen_active();
    store_ui_active = true;
    store_is_refresh = true;
    store_open_when_ready = (store_open_origin != store_screen);
    set_store_title();
    const char * busy_title;
    if (store_view == STORE_VIEW_LAYOUTS) busy_title = TR("Loading layouts");
    else if (store_view == STORE_VIEW_THEMES) busy_title = TR("Loading themes");
    else if (store_view == STORE_VIEW_UPDATES) busy_title = TR("Loading updates");
    else busy_title = TR("Loading plugins");
    store_busy = gui_busy_show(busy_title, TR("This may take a while"));
    return true;
}

bool gui_plugin_store_open_player_layouts(void) {
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    clear_layout_picker_request();
    return start_catalog_refresh(STORE_VIEW_LAYOUTS);
}

bool gui_plugin_store_open_themes(void) {
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    clear_layout_picker_request();
    return start_catalog_refresh(STORE_VIEW_THEMES);
}

static void theme_store_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return;
    }
    (void) start_catalog_refresh(STORE_VIEW_THEMES);
}

bool gui_plugin_store_open_updates(void) {
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    clear_layout_picker_request();
    return start_catalog_refresh(STORE_VIEW_UPDATES);
}

static void player_layout_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return;
    }
    (void) start_catalog_refresh(STORE_VIEW_LAYOUTS);
}

bool gui_plugin_store_setup_catalog_ready(void) {
    if (!store_rows) return false;
    plugin_store_status_t status;
    plugin_store_get_status(&status, NULL, 0);
    if (status.state == PLUGIN_STORE_READY) {
        /* A backend READY state can belong to an earlier Plugin Manager
         * session. Only trust rows fetched by this setup request, an existing
         * fresh cache, or the store UI's own in-flight catalog refresh. */
        if (store_setup_catalog_pending || store_setup_catalog_ready ||
            (store_ui_active && store_is_refresh)) {
            plugin_store_get_status(&status, store_rows, PLUGIN_STORE_MAX_RESULTS);
            store_row_count = status.result_count;
            store_setup_catalog_ready = true;
            store_setup_catalog_pending = false;
        }
    } else if (status.state == PLUGIN_STORE_FAILED || status.state == PLUGIN_STORE_IDLE) {
        if (store_setup_catalog_pending) store_setup_catalog_pending = false;
        store_setup_catalog_ready = false;
    } else {
        store_setup_catalog_ready = false;
    }
    return store_setup_catalog_ready;
}

bool gui_plugin_store_setup_catalog_prepare(void) {
    if (store_setup_catalog_pending) return true;
    /* Entering the setup plugin step must validate catalog data with a new
     * request even when the shared backend still says READY from an older
     * store visit. */
    store_setup_catalog_ready = false;
    if (gui_plugin_store_operation_active()) return false;
    if (!store_rows) {
        show_error_toast(TR("Not enough memory to load the plugin store"));
        return false;
    }
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (!plugin_store_refresh()) {
        show_error_toast(TR("Could not start the plugin refresh"));
        return false;
    }
    store_view = STORE_VIEW_PLUGINS;
    set_store_title();
    store_setup_catalog_ready = false;
    store_setup_catalog_pending = true;
    return true;
}

bool gui_plugin_store_setup_get_plugin(const char * id, plugin_store_result_t * row,
                                      plugin_store_details_t * details) {
    if (!id || !id[0] || !row || !details || !gui_plugin_store_setup_catalog_ready()) return false;
    for (size_t i = 0; i < store_row_count; ++i) {
        if (store_rows[i].state == PLUGIN_STORE_PLUGIN_REMOVED || strcmp(store_rows[i].id, id) != 0) continue;
        *row = store_rows[i];
        return plugin_store_get_details(id, details);
    }
    return false;
}

uint64_t gui_plugin_store_setup_populate_layout_suggestions(
    lv_obj_t * parent, bool (*is_selected)(const char * id),
    void (*select)(const char * id, const char * name)) {
    uint64_t generation = plugin_store_preview_generation();
    if (!gui_plugin_store_setup_catalog_ready()) return generation;

    /* Pin a stable bounded set, including ready entries. Cycling through
     * missing-only batches would repeatedly evict and redownload previews. */
    const char * preview_ids[PLUGIN_STORE_PREVIEW_CAPACITY];
    size_t preview_count = 0;
    int vinyl_index = -1;
    for (size_t i = 0; i < store_row_count; ++i) {
        if (strcmp(store_rows[i].id, GUI_SETUP_LAYOUT_PLUGIN_ID) == 0 &&
            layout_picker_row_enabled(&store_rows[i])) {
            vinyl_index = (int) i;
            break;
        }
    }
    if (vinyl_index >= 0) {
        preview_ids[preview_count++] = store_rows[vinyl_index].id;
    }
    for (size_t i = 0; i < store_row_count && preview_count < PLUGIN_STORE_PREVIEW_CAPACITY; ++i) {
        const plugin_store_result_t * candidate = &store_rows[i];
        if ((int) i == vinyl_index || !layout_picker_row_enabled(candidate)) continue;
        preview_ids[preview_count++] = candidate->id;
    }
    if (preview_count) (void) plugin_store_prepare_previews(preview_ids, preview_count);

    if (!parent || !is_selected || !select || vinyl_index < 0)
        return generation;

    plugin_store_result_t * row = &store_rows[vinyl_index];
    char preview[512], resolved[520];
    const char * preview_src = NULL;
    if (plugin_store_get_preview(row->id, preview, sizeof(preview))) {
        snprintf(resolved, sizeof(resolved), "S:%s", preview);
        preview_src = resolved;
    }
    const char * label = row->name[0] ? row->name : row->id;
    layout_suggestion_ctx_t * ctx = lv_malloc(sizeof(*ctx));
    if (ctx) {
        snprintf(ctx->id, sizeof(ctx->id), "%s", row->id);
        ctx->select = select;
    }
    lv_obj_t * card = add_cover_card(parent, label, preview_src, 2,
                                     ctx ? layout_suggestion_card_cb : NULL, ctx);
    if (!card && ctx) lv_free(ctx);
    if (card) {
        if (ctx) lv_obj_add_event_cb(card, layout_suggestion_card_delete_cb, LV_EVENT_DELETE, ctx);
        bool selected = is_selected(row->id);
        lv_obj_set_style_radius(card, BOARD_SCALE_PX(10), 0);
        lv_obj_set_style_outline_width(card, selected ? BOARD_SCALE_PX(2) : 0, 0);
        lv_obj_set_style_outline_color(card, accent_lv_color(), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_pad(card, BOARD_SCALE_PX(2), 0);
    }
    return generation;
}

bool gui_plugin_store_open_layout_picker(bool (*is_selected)(const char * id),
                                         void (*select)(const char * id, const char * name)) {
    if (!is_selected || !select || store_layout_picker_mode || store_picker_mode ||
        store_ui_active || store_push_pending || store_confirm_pending ||
        (plugin_store_busy() && !store_setup_catalog_pending))
        return false;

    bool ready = gui_plugin_store_setup_catalog_ready();
    if (!ready && !store_setup_catalog_pending && !gui_plugin_store_setup_catalog_prepare()) return false;

    clear_layout_picker_request();
    store_view = STORE_VIEW_LAYOUTS;
    set_store_title();
    store_layout_picker_mode = true;
    store_layout_picker_pending = true;
    store_layout_picker_origin = lv_screen_active();
    store_layout_is_selected = is_selected;
    store_layout_select = select;
    if (ready) store_push_pending = true;
    return true;
}

bool gui_plugin_store_open_picker(bool (*is_selected)(const char * id),
                                  void (*toggle)(const char * id, const char * name),
                                  void (*done)(void)) {
    if (!is_selected || !toggle) return false;
    if (store_layout_picker_mode || store_layout_picker_pending) return false;
    clear_layout_picker_request();
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (store_picker_mode) return false;
    if (store_setup_catalog_pending) {
        store_view = STORE_VIEW_PLUGINS;
        set_store_title();
        store_picker_mode = true;
        store_picker_pending = true;
        store_picker_origin = lv_screen_active();
        store_picker_is_selected = is_selected;
        store_picker_toggle = toggle;
        store_picker_done = done;
        return true;
    }
    if (store_ui_active || store_push_pending || store_confirm_pending || plugin_store_busy()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    if (gui_plugin_store_setup_catalog_ready()) {
        store_view = STORE_VIEW_PLUGINS;
        set_store_title();
        store_picker_mode = true;
        store_picker_origin = lv_screen_active();
        store_picker_is_selected = is_selected;
        store_picker_toggle = toggle;
        store_picker_done = done;
        show_picker_popup();
        return true;
    }
    if (!gui_plugin_store_setup_catalog_prepare()) {
        return false;
    }
    store_picker_mode = true;
    store_picker_pending = true;
    store_picker_origin = lv_screen_active();
    store_picker_is_selected = is_selected;
    store_picker_toggle = toggle;
    store_picker_done = done;
    return true;
}

void gui_plugin_store_row_cb(lv_event_t * e) {
    if (e && lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return;
    }
    if (gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return;
    }
    clear_layout_picker_request();
    (void) start_catalog_refresh(STORE_VIEW_PLUGINS);
}

void poll_plugin_store(void) {
    if (store_picker_mode && !store_picker_pending && store_picker_origin &&
        !gui_navigation_is_top(store_picker_origin)) {
        gui_popup_hide(&picker_popup);
        clear_picker_request();
    }
    if (store_picker_pending && store_picker_origin && !gui_navigation_is_top(store_picker_origin))
        clear_picker_request();
    if (store_layout_picker_mode && !store_layout_picker_pending && store_left())
        clear_layout_picker_request();
    if (store_layout_picker_pending && store_layout_picker_origin &&
        !gui_navigation_is_top(store_layout_picker_origin))
        clear_layout_picker_request();
    if (store_layout_picker_pending && store_layout_picker_origin &&
        gui_navigation_is_top(store_layout_picker_origin)) {
        plugin_store_status_t layout_status;
        plugin_store_get_status(&layout_status, NULL, 0);
        if (layout_status.state == PLUGIN_STORE_READY && gui_plugin_store_setup_catalog_ready()) {
            store_push_pending = true;
        } else if ((layout_status.state == PLUGIN_STORE_FAILED ||
                    layout_status.state == PLUGIN_STORE_IDLE) && !store_setup_catalog_pending) {
            clear_layout_picker_request();
            if (layout_status.state == PLUGIN_STORE_FAILED && layout_status.error[0])
                show_error_toast(layout_status.error);
        }
    }
    /* A setup screen may poll catalog readiness before this module's poll.
     * Readiness must still complete a waiting More request even if that poll
     * already consumed store_setup_catalog_pending. */
    if (store_picker_pending) {
        plugin_store_status_t pending_status;
        plugin_store_get_status(&pending_status, NULL, 0);
        if (gui_plugin_store_setup_catalog_ready()) {
            if (store_picker_origin && gui_navigation_is_top(store_picker_origin)) {
                store_picker_pending = false;
                show_picker_popup();
            } else {
                clear_picker_request();
            }
        } else if (pending_status.state == PLUGIN_STORE_FAILED || pending_status.state == PLUGIN_STORE_IDLE) {
            clear_picker_request();
            show_error_toast(pending_status.error[0] ? pending_status.error : TR("Could not load the plugin catalog."));
        }
    }
    if (store_setup_catalog_pending) {
        plugin_store_status_t setup_status;
        plugin_store_get_status(&setup_status, NULL, 0);
        if (setup_status.state == PLUGIN_STORE_READY) {
            (void) gui_plugin_store_setup_catalog_ready();
            if (store_layout_picker_pending && store_layout_picker_origin &&
                gui_navigation_is_top(store_layout_picker_origin)) store_push_pending = true;
            if (store_picker_pending) {
                if (store_picker_origin && gui_navigation_is_top(store_picker_origin)) {
                    store_picker_pending = false;
                    show_picker_popup();
                } else {
                    clear_picker_request();
                }
            }
        } else if (setup_status.state == PLUGIN_STORE_FAILED || setup_status.state == PLUGIN_STORE_IDLE) {
            store_setup_catalog_pending = false;
            store_setup_catalog_ready = false;
            if (store_layout_picker_pending) {
                clear_layout_picker_request();
                if (setup_status.state == PLUGIN_STORE_FAILED && setup_status.error[0])
                    show_error_toast(setup_status.error);
            }
            if (store_picker_pending) {
                clear_picker_request();
                if (setup_status.state == PLUGIN_STORE_FAILED && setup_status.error[0])
                    show_error_toast(setup_status.error);
            }
        }
    }
    if (store_picker_refresh_pending) {
        store_picker_refresh_pending = false;
        if (gui_popup_is_visible(&picker_popup)) populate_picker_popup(true);
    }
    /* Home drops a covered store from the stack without unloading it. */
    if (store_changed_dirty && !store_ui_active && !store_push_pending && store_left()) apply_changes();
    if (store_open_when_ready && store_ui_active && store_is_refresh &&
        !gui_navigation_is_top(gui_busy_get_screen())) {
        /* The user backed out of the loading screen. Let the catalog worker
         * finish, but do not later open the store. */
        store_open_when_ready = false;
        store_open_origin = NULL;
        clear_picker_request();
    }
    if (store_push_pending) {
        /* Pushing while the busy screen still slides away would let that
         * animation load Plugin Manager over the store. */
        if (gui_navigation_transition_in_progress()) return;
        store_push_pending = false;
        if (store_picker_pending &&
            (!store_picker_origin || !gui_navigation_is_top(store_picker_origin))) {
            clear_picker_request();
            store_open_origin = NULL;
            return;
        }
        if (store_layout_picker_pending &&
            (!store_layout_picker_origin || !gui_navigation_is_top(store_layout_picker_origin))) {
            clear_layout_picker_request();
            store_open_origin = NULL;
            return;
        }
        if (store_open_origin && !gui_navigation_is_top(store_open_origin)) {
            store_open_origin = NULL;
            return;
        }
        store_open_origin = NULL;
        nav_push(store_screen);
        store_picker_pending = false;
        store_layout_picker_pending = false;
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
    if ((store_view == STORE_VIEW_LAYOUTS || store_view == STORE_VIEW_THEMES) && store_can_prepare_previews()) {
        plugin_store_status_t preview_status;
        plugin_store_get_status(&preview_status, NULL, 0);
        if (preview_status.state == PLUGIN_STORE_READY) {
            store_prepare_previews_if_ready(preview_status.state);
            uint64_t generation = plugin_store_preview_generation();
            if (generation != store_preview_generation_seen) {
                populate_store_screen(true);
                return;
            }
        }
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
        clear_picker_request();
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
        lv_label_set_text(confirm_title, TR("Some plugin files were changed on the card. Replace them?"));
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
        store_setup_catalog_ready = false;
        store_setup_catalog_pending = false;
        clear_picker_request();
        note_changed(status.changed); /* Update All may have updated some before failing */
        populate_store_screen(false);
        show_error_toast(status.error[0] ? status.error : TR("Plugin operation failed"));
        return;
    }
    if (status.state == PLUGIN_STORE_READY) {
        gui_busy_hide(store_busy);
        store_ui_active = false;
        store_setup_catalog_pending = false;
        store_setup_catalog_ready = store_is_refresh;
        if (!store_is_refresh) note_changed(status.changed);
        populate_store_screen(false);
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
    store_view = STORE_VIEW_PLUGINS;
    clear_layout_picker_request();
    store_preview_generation_seen = plugin_store_preview_generation();
    store_screen = build_subsonic_list_screen(TR("Plugin Store"), &store_title, &store_list);
    store_header_review_updates_btn = build_top_right_icon_button(store_screen,
                                                                 asset_path("stream_media/download.png"),
                                                                 review_updates_cb);
    if (store_header_review_updates_btn)
        lv_obj_add_flag(store_header_review_updates_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(store_screen, store_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(store_screen, store_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    build_popups();
}

void gui_plugin_store_teardown(void) {
    plugin_store_cancel_previews();
    clear_picker_request();
    clear_layout_picker_request();
    store_setup_catalog_pending = false;
    store_setup_catalog_ready = false;
    gui_popup_teardown(&detail_popup);
    gui_popup_teardown(&remove_popup);
    gui_popup_teardown(&confirm_popup);
    gui_popup_teardown(&picker_popup);
    if (store_screen) {
        lv_obj_delete(store_screen);
        store_screen = NULL;
        store_list = NULL;
        store_header_review_updates_btn = NULL;
    }
    free(store_rows);
    store_rows = NULL;
    store_row_count = 0;
    store_view = STORE_VIEW_PLUGINS;
    store_title = NULL;
    store_push_pending = false;
    store_confirm_pending = false;
    store_open_when_ready = false;
    store_open_origin = NULL;
    /* A running worker keeps reporting to the rebuilt screens; otherwise
     * start over. */
    if (!plugin_store_busy()) {
        plugin_store_reset();
        store_ui_active = false;
        store_open_when_ready = false;
        store_pending_update_all = false;
    }
}
