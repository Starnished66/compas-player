#include "gui_plugin_order_editor.h"

#include "screen_builders.h"
#include "gui_list_reorder.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "plugin_manager.h"
#include "i18n.h"
#include "board_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The plugin editor shares the reorder primitive while keeping its own state. */
typedef struct {
    char key[64];
    char label[128];
    bool visible;
} plugin_order_editor_item_t;
static lv_obj_t * plugin_order_editor_screen;
static lv_obj_t * plugin_order_editor_title;
static lv_obj_t * plugin_order_editor_list;
static lv_obj_t * plugin_order_editor_rows[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static lv_obj_t * plugin_order_editor_handles[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static lv_obj_t * plugin_order_editor_switches[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static plugin_order_editor_item_t plugin_order_editor_items[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static int plugin_order_editor_order[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static int plugin_order_editor_count;
static gui_list_reorder_state_t plugin_order_editor_drag_state = { .drag_id = -1 };

static void plugin_order_editor_emit_change(void) {
    const char * keys[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    const char * labels[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    bool visible[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    for (int i = 0; i < plugin_order_editor_count; ++i) {
        int item = plugin_order_editor_order[i];
        keys[i] = plugin_order_editor_items[item].key;
        labels[i] = plugin_order_editor_items[item].label;
        visible[i] = plugin_order_editor_items[item].visible;
    }
    plugin_manager_order_editor_changed(0, keys, labels, visible, plugin_order_editor_count);
}

static void plugin_order_editor_populate(void) {
    if (!plugin_order_editor_list) return;
    for (int id = 0; id < plugin_order_editor_count; ++id)
        gui_list_reorder_row_stop_translate(plugin_order_editor_rows[id]);
    for (int pos = 0; pos < plugin_order_editor_count; ++pos)
        lv_obj_move_to_index(plugin_order_editor_rows[plugin_order_editor_order[pos]], pos);
    lv_obj_update_layout(plugin_order_editor_list);
    for (int id = 0; id < plugin_order_editor_count; ++id) {
        bool visible = plugin_order_editor_items[id].visible;
        if (visible) lv_obj_add_state(plugin_order_editor_switches[id], LV_STATE_CHECKED);
        else lv_obj_clear_state(plugin_order_editor_switches[id], LV_STATE_CHECKED);
    }
}

static void plugin_order_editor_prepare_drag(gui_list_reorder_state_t * state, int id) {
    (void)id;
    state->count = plugin_order_editor_count;
    memcpy(state->ids, plugin_order_editor_order,
           (size_t)state->count * sizeof(state->ids[0]));
}
static void plugin_order_editor_commit_drag(gui_list_reorder_state_t * state) {
    bool changed = false;
    for (int i = 0; i < plugin_order_editor_count; ++i)
        if (plugin_order_editor_order[i] != state->ids[i]) { changed = true; break; }
    if (!changed) return;
    memcpy(plugin_order_editor_order, state->ids,
           (size_t)plugin_order_editor_count * sizeof(plugin_order_editor_order[0]));
    plugin_order_editor_emit_change();
}
static void plugin_order_editor_restore_drag(gui_list_reorder_state_t * state) {
    (void)state;
    plugin_order_editor_populate();
}
static void plugin_order_editor_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (plugin_order_editor_drag_state.active)
        gui_list_reorder_finish(&plugin_order_editor_drag_state, false);
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id >= plugin_order_editor_count) return;
    plugin_order_editor_items[id].visible =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    plugin_order_editor_populate();
    plugin_order_editor_emit_change();
}
static void plugin_order_editor_drag_handle_cb(lv_event_t * e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    gui_list_reorder_event(&plugin_order_editor_drag_state, id, e);
}

static void plugin_order_editor_screen_unloaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOAD_START &&
        lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOADED) return;
    gui_list_reorder_finish(&plugin_order_editor_drag_state, false);
    if (plugin_order_editor_screen && !gui_navigation_contains(plugin_order_editor_screen))
        plugin_manager_order_editor_closed(0);
}

static void plugin_order_editor_build_rows(void) {
    if (!plugin_order_editor_list) return;
    for (int id = 0; id < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++id) {
        if (plugin_order_editor_handles[id]) unregister_swipe_dead_zone(plugin_order_editor_handles[id]);
    }
    lv_obj_clean(plugin_order_editor_list);
    memset(plugin_order_editor_rows, 0, sizeof(plugin_order_editor_rows));
    memset(plugin_order_editor_handles, 0, sizeof(plugin_order_editor_handles));
    memset(plugin_order_editor_switches, 0, sizeof(plugin_order_editor_switches));
    for (int id = 0; id < plugin_order_editor_count; ++id) {
        lv_obj_t * row = add_pill_row_base(plugin_order_editor_list, TR(plugin_order_editor_items[id].label));
        plugin_order_editor_rows[id] = row;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_USER_1);
        int32_t handle_px = BOARD_SCALE_PX(72);
        if (handle_px < 48) handle_px = 48;
        int32_t switch_width = BOARD_SCALE_PX(50);
        int32_t text_width = LIST_ROW_WIDTH - handle_px - switch_width - GUI_TEXT_INSET - BOARD_SCALE_PX(20);
        lv_obj_t * text = lv_obj_get_child(row, 0);
        lv_obj_set_width(text, text_width);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(text, LV_OBJ_FLAG_USER_3);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(text, 0, 0);
        lv_obj_align(text, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET + handle_px, 0);

        lv_obj_t * handle = lv_button_create(row);
        plugin_order_editor_handles[id] = handle;
        lv_obj_set_size(handle, handle_px, ui_list_row_height());
        lv_obj_align(handle, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(handle, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_USER_1);
        lv_obj_set_style_radius(handle, BOARD_SCALE_PX(8), 0);
        lv_obj_set_style_bg_opa(handle, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(handle, 0, 0);
        lv_obj_set_style_shadow_width(handle, 0, 0);
        lv_obj_set_style_pad_all(handle, 0, 0);
        lv_obj_t * grip = lv_image_create(handle);
        lv_obj_set_size(grip, BOARD_SCALE_PX(32), BOARD_SCALE_PX(32));
        set_reorder_handle_icon(grip);
        lv_image_set_inner_align(grip, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(grip);
        lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(handle, plugin_order_editor_drag_handle_cb, LV_EVENT_ALL,
                            (void *)(intptr_t)id);
        register_swipe_dead_zone(handle);

        lv_obj_t * visible_switch = lv_switch_create(row);
        plugin_order_editor_switches[id] = visible_switch;
        lv_obj_add_style(visible_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_size(visible_switch, switch_width, BOARD_SCALE_PX(28));
        lv_obj_align(visible_switch, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(6), 0);
        lv_obj_add_event_cb(visible_switch, plugin_order_editor_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)id);
    }
    lv_obj_update_layout(plugin_order_editor_list);
    for (int id = 0; id < plugin_order_editor_count; ++id) {
        lv_obj_t * row = plugin_order_editor_rows[id];
        lv_obj_t * text = lv_obj_get_child(row, 0);
        int32_t height = lv_obj_get_height(text) + BOARD_SCALE_PX(18);
        if (height < ui_list_row_height()) height = ui_list_row_height();
        lv_obj_set_height(row, height);
        lv_obj_set_height(plugin_order_editor_handles[id], height);
    }
    plugin_order_editor_populate();
}

int gui_plugin_show_order_editor(const char * title, const char * const * keys,
                                 const char * const * labels, const bool * visible, int count) {
    if (!keys || !labels || !visible || count < 1 || count > PLUGIN_ORDER_EDITOR_MAX_ITEMS) return -1;
    if (plugin_order_editor_screen && gui_navigation_contains(plugin_order_editor_screen)) return -1;
    if (!plugin_order_editor_screen) {
        plugin_order_editor_screen = build_subsonic_list_screen(
            title && title[0] ? title : TR("Plugin Order"), &plugin_order_editor_title,
            &plugin_order_editor_list);
        lv_obj_add_event_cb(plugin_order_editor_screen, plugin_order_editor_screen_unloaded_cb,
                            LV_EVENT_SCREEN_UNLOAD_START, NULL);
        lv_obj_add_event_cb(plugin_order_editor_screen, plugin_order_editor_screen_unloaded_cb,
                            LV_EVENT_SCREEN_UNLOADED, NULL);
        enable_gesture_bubble_recursive(plugin_order_editor_screen);
    } else {
        lv_label_set_text(plugin_order_editor_title,
                          title && title[0] ? title : TR("Plugin Order"));
    }
    plugin_order_editor_drag_state.list = plugin_order_editor_list;
    plugin_order_editor_drag_state.rows = plugin_order_editor_rows;
    plugin_order_editor_drag_state.row_capacity = PLUGIN_ORDER_EDITOR_MAX_ITEMS;
    plugin_order_editor_drag_state.prepare = plugin_order_editor_prepare_drag;
    plugin_order_editor_drag_state.commit = plugin_order_editor_commit_drag;
    plugin_order_editor_drag_state.restore = plugin_order_editor_restore_drag;
    plugin_order_editor_count = count;
    for (int i = 0; i < count; ++i) {
        snprintf(plugin_order_editor_items[i].key, sizeof(plugin_order_editor_items[i].key), "%s", keys[i]);
        snprintf(plugin_order_editor_items[i].label, sizeof(plugin_order_editor_items[i].label), "%s", labels[i]);
        plugin_order_editor_items[i].visible = visible[i];
        plugin_order_editor_order[i] = i;
    }
    plugin_order_editor_build_rows();
    nav_push(plugin_order_editor_screen);
    return 0;
}

void gui_plugin_order_editor_teardown(void) {
    gui_list_reorder_finish(&plugin_order_editor_drag_state, false);
    for (int id = 0; id < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++id)
        if (plugin_order_editor_handles[id]) unregister_swipe_dead_zone(plugin_order_editor_handles[id]);
    if (plugin_order_editor_screen) { lv_obj_delete(plugin_order_editor_screen); plugin_order_editor_screen = NULL; }
    plugin_order_editor_title = plugin_order_editor_list = NULL;
    memset(plugin_order_editor_rows, 0, sizeof(plugin_order_editor_rows));
    memset(plugin_order_editor_handles, 0, sizeof(plugin_order_editor_handles));
    memset(plugin_order_editor_switches, 0, sizeof(plugin_order_editor_switches));
    plugin_order_editor_count = 0;
    memset(&plugin_order_editor_drag_state, 0, sizeof(plugin_order_editor_drag_state));
    plugin_order_editor_drag_state.drag_id = -1;
}
