#include "gui_shell_settings.h"

#include "gui_list_reorder.h"
#include "gui_navigation.h"
#include "gui_reload.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"
#include "settings.h"
#include "ui_customization.h"

#include <stdint.h>
#include <string.h>

static lv_obj_t * ui_order_screen;
static lv_obj_t * ui_order_list;
static lv_obj_t * ui_order_title;
static lv_obj_t * ui_order_rows[UI_ORDER_MAX];
static lv_obj_t * ui_order_drag_handles[UI_ORDER_MAX];
static lv_obj_t * ui_order_visible_buttons[UI_ORDER_MAX];
static ui_order_kind_t ui_order_kind = UI_ORDER_DRAWER;

_Static_assert(UI_DRAWER_COUNT <= GUI_LIST_REORDER_MAX_ROWS,
               "Quick Drawer order editor exceeds shared drag capacity");
_Static_assert(UI_DRAWER_COUNT <= UI_ORDER_MAX,
               "Quick Drawer order exceeds saved order capacity");
_Static_assert(UI_ORDER_MAX <= GUI_LIST_REORDER_MAX_ROWS,
               "Saved order exceeds shared drag capacity");
static gui_list_reorder_state_t ui_order_drag_state = { .drag_id = -1 };

static lv_obj_t * quick_drawer_settings_screen;
static lv_obj_t * quick_drawer_settings_list;
static void populate_quick_drawer_settings(void);
extern void gui_shell_apply_customization(void);
static void topbar_set_clicked_row_switch(lv_event_t * e, bool checked);
static lv_obj_t * settings_topbar_screen;
static lv_obj_t * settings_topbar_list;
static lv_obj_t * settings_topbar_clock_position_screen;
static lv_obj_t * settings_topbar_clock_position_list;
static ui_item_order_t * ui_saved_order(void) {
    switch (ui_order_kind) {
        case UI_ORDER_MUSIC: return &current_settings.ui_customization.music;
        default: return &current_settings.ui_customization.drawer;
    }
}
static void populate_ui_order_screen(void);
static void ui_order_open_kind(ui_order_kind_t kind, const char * title);
static void ui_order_control_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (ui_order_drag_state.active) gui_list_reorder_finish(&ui_order_drag_state, false);
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    int item_count = ui_order_size(ui_order_kind);
    if (id < 0 || id >= item_count) return;
    ui_item_order_t * order = ui_saved_order();
    unsigned saved_count = order->count > UI_ORDER_MAX ? UI_ORDER_MAX : order->count;
    int at = -1;
    for (unsigned i = 0; i < saved_count; ++i)
        if (order->ids[i] == id) { at = (int)i; break; }
    if (at < 0 && order->count < UI_ORDER_MAX) order->ids[order->count++] = (uint8_t)id;
    else if (at >= 0) {
        for (unsigned i = (unsigned)at + 1; i < order->count; ++i) order->ids[i - 1] = order->ids[i];
        --order->count;
    } else return;
    settings_save_async(&current_settings);
    populate_ui_order_screen();
    if (ui_order_kind == UI_ORDER_DRAWER) gui_shell_apply_customization();
    else gui_theme_refresh_request();
}
static void ui_order_drag_prepare_drawer(gui_list_reorder_state_t * state, int id) {
    (void)id;
    ui_item_order_t * saved = ui_saved_order();
    state->count = 0;
    unsigned saved_count = saved->count > UI_ORDER_MAX ? UI_ORDER_MAX : saved->count;
    for (unsigned i = 0; i < saved_count && state->count < UI_ORDER_MAX; ++i)
        if (saved->ids[i] < ui_order_size(ui_order_kind)) state->ids[state->count++] = saved->ids[i];
}
static void ui_order_drag_commit_drawer(gui_list_reorder_state_t * state) {
    if (state->drag_count < 0) return;
    int drag_count = state->drag_count;
    if (drag_count > UI_ORDER_MAX) drag_count = UI_ORDER_MAX;
    if (drag_count > ui_order_size(ui_order_kind)) return;
    ui_item_order_t * saved = ui_saved_order();
    bool changed = saved->count != (unsigned)drag_count;
    for (int i = 0; !changed && i < drag_count; ++i)
        if (saved->ids[i] != (uint8_t)state->ids[i]) changed = true;
    if (!changed) return;
    saved->count = (uint8_t)drag_count;
    for (int i = 0; i < drag_count; ++i) saved->ids[i] = (uint8_t)state->ids[i];
    settings_save_async(&current_settings);
    if (ui_order_kind == UI_ORDER_DRAWER) gui_shell_apply_customization();
    else gui_theme_refresh_request();
}
static void ui_order_drag_restore_drawer(gui_list_reorder_state_t * state) {
    (void)state;
    if (ui_order_screen && ui_order_list) populate_ui_order_screen();
}
static void ui_order_drag_handle_cb(lv_event_t * e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    gui_list_reorder_event(&ui_order_drag_state, id, e);
}
static void ui_order_screen_unloaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOAD_START &&
        lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOADED) return;
    gui_list_reorder_finish(&ui_order_drag_state, false);
}
static void add_customization_help(lv_obj_t *list) {
    lv_obj_t *help = add_section_header(list, TR("Plugins can override these saved preferences."));
    lv_obj_set_width(help, lv_pct(100));
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_LEFT, 0);
}

static void populate_ui_order_screen(void) {
    if (!ui_order_list) return;
    int item_count = ui_order_size(ui_order_kind);
    for (int id = 0; id < UI_ORDER_MAX; ++id)
        gui_list_reorder_row_stop_translate(ui_order_rows[id]);
    for (int id = 0; id < UI_ORDER_MAX; ++id) {
        if (!ui_order_rows[id]) continue;
        if (id < item_count) lv_obj_remove_flag(ui_order_rows[id], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui_order_rows[id], LV_OBJ_FLAG_HIDDEN);
    }
    ui_item_order_t * order = ui_saved_order();
    bool listed[UI_ORDER_MAX] = { false };
    int visual_index = 0;
    for (unsigned pos = 0; pos < order->count; ++pos) {
        int id = order->ids[pos];
        if (id >= item_count || listed[id] || !ui_order_rows[id]) continue;
        listed[id] = true;
        lv_obj_move_to_index(ui_order_rows[id], visual_index++);
    }
    for (int id = 0; id < item_count; ++id) {
        if (listed[id] || !ui_order_rows[id]) continue;
        listed[id] = true;
        lv_obj_move_to_index(ui_order_rows[id], visual_index++);
    }
    lv_obj_update_layout(ui_order_list);
    for (int id = 0; id < item_count; ++id) {
        lv_obj_t * row = ui_order_rows[id];
        if (!row) continue;
        lv_obj_t * text = lv_obj_get_child(row, 0);
        lv_label_set_text(text, TR(ui_order_label(ui_order_kind, id)));
        int32_t height = lv_obj_get_height(text) + BOARD_SCALE_PX(18);
        if (height < ui_list_row_height()) height = ui_list_row_height();
        lv_obj_set_height(row, height);
        lv_obj_set_height(ui_order_drag_handles[id], height);
        bool visible = false;
        for (unsigned i = 0; i < order->count; ++i) if (order->ids[i] == id) { visible = true; break; }
        lv_obj_t * handle = ui_order_drag_handles[id];
        if (visible) lv_obj_clear_state(handle, LV_STATE_DISABLED);
        else lv_obj_add_state(handle, LV_STATE_DISABLED);
        if (visible) lv_obj_add_state(ui_order_visible_buttons[id], LV_STATE_CHECKED);
        else lv_obj_clear_state(ui_order_visible_buttons[id], LV_STATE_CHECKED);
    }
}
static lv_obj_t * build_ui_order_screen(void) {
    ui_order_screen = build_subsonic_list_screen(TR("Quick Drawer"), &ui_order_title, &ui_order_list);
    ui_order_drag_state.list = ui_order_list;
    ui_order_drag_state.rows = ui_order_rows;
    ui_order_drag_state.row_capacity = UI_ORDER_MAX;
    ui_order_drag_state.prepare = ui_order_drag_prepare_drawer;
    ui_order_drag_state.commit = ui_order_drag_commit_drawer;
    ui_order_drag_state.restore = ui_order_drag_restore_drawer;
    memset(ui_order_rows, 0, sizeof(ui_order_rows));
    memset(ui_order_drag_handles, 0, sizeof(ui_order_drag_handles));
    memset(ui_order_visible_buttons, 0, sizeof(ui_order_visible_buttons));
    for (int id = 0; id < UI_ORDER_MAX; ++id) {
        const char * label = ui_order_label(UI_ORDER_DRAWER, id);
        if (!label) continue;
        lv_obj_t * row = add_pill_row_base(ui_order_list, TR(label));
        ui_order_rows[id] = row;
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
        ui_order_drag_handles[id] = handle;
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
        lv_obj_set_style_opa(handle, LV_OPA_30, LV_STATE_DISABLED);
        lv_obj_set_style_pad_all(handle, 0, 0);
        lv_obj_t * grip = lv_image_create(handle);
        lv_obj_set_size(grip, BOARD_SCALE_PX(32), BOARD_SCALE_PX(32));
        set_reorder_handle_icon(grip);
        lv_image_set_inner_align(grip, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(grip);
        lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(handle, ui_order_drag_handle_cb, LV_EVENT_ALL, (void *)(intptr_t)id);
        register_swipe_dead_zone(handle);

        lv_obj_t * visible_switch = lv_switch_create(row);
        ui_order_visible_buttons[id] = visible_switch;
        lv_obj_add_style(visible_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_size(visible_switch, switch_width, BOARD_SCALE_PX(28));
        lv_obj_align(visible_switch, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(6), 0);
        lv_obj_add_event_cb(visible_switch, ui_order_control_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    }
    populate_ui_order_screen();
    add_customization_help(ui_order_list);
    lv_obj_add_event_cb(ui_order_screen, ui_order_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(ui_order_screen, ui_order_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    enable_gesture_bubble_recursive(ui_order_screen);
    return ui_order_screen;
}
static void ui_order_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_order_open_kind(UI_ORDER_DRAWER, "Quick Drawer");
}
static void ui_order_open_kind(ui_order_kind_t kind, const char * title) {
    ui_order_kind = kind;
    if (!ui_order_screen) build_ui_order_screen();
    if (ui_order_title) lv_label_set_text(ui_order_title, TR(title));
    if (ui_order_list) populate_ui_order_screen();
    nav_push(ui_order_screen);
}
void gui_shell_settings_music_order_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_order_kind_t kind = (ui_order_kind_t)(intptr_t)lv_event_get_user_data(e);
    ui_order_open_kind(kind, "Music menu");
}

static void quick_drawer_setting_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0) current_settings.ui_customization.drawer_enabled = !current_settings.ui_customization.drawer_enabled;
    else if (which == 1) current_settings.ui_customization.drawer_eq_visible = !current_settings.ui_customization.drawer_eq_visible;
    else current_settings.quick_drawer_volume_visible = !current_settings.quick_drawer_volume_visible;
    settings_save_async(&current_settings);
    bool value = which == 0 ? current_settings.ui_customization.drawer_enabled :
                 which == 1 ? current_settings.ui_customization.drawer_eq_visible :
                              current_settings.quick_drawer_volume_visible;
    topbar_set_clicked_row_switch(e, value);
    if (which == 2) gui_shell_refresh_quick_drawer_volume_visibility();
    else gui_shell_apply_customization();
}
static void populate_quick_drawer_settings(void) {
    if (!quick_drawer_settings_list) return;
    lv_obj_clean(quick_drawer_settings_list);
    struct { const char * label; bool value; intptr_t which; } toggles[] = {
        { N_("Enabled"), current_settings.ui_customization.drawer_enabled, 0 },
        { N_("Equalizer"), current_settings.ui_customization.drawer_eq_visible, 1 },
        { N_("Volume Slider"), current_settings.quick_drawer_volume_visible, 2 },
    };
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); ++i) {
        lv_obj_t * row = add_pill_toggle_row(quick_drawer_settings_list, TR(toggles[i].label), toggles[i].value, NULL);
        lv_obj_add_event_cb(row, quick_drawer_setting_toggle_cb, LV_EVENT_CLICKED, (void *)toggles[i].which);
    }
    lv_obj_t * reorder = add_pill_chevron_row(quick_drawer_settings_list, TR("Reorder Items"), NULL);
    lv_obj_add_event_cb(reorder, ui_order_settings_row_cb, LV_EVENT_CLICKED, NULL);
    add_customization_help(quick_drawer_settings_list);
    if (quick_drawer_settings_screen) enable_gesture_bubble_recursive(quick_drawer_settings_screen);
}
static lv_obj_t * build_quick_drawer_settings_screen(void) {
    lv_obj_t * title_label;
    quick_drawer_settings_screen = build_subsonic_list_screen(TR("Quick Drawer"), &title_label, &quick_drawer_settings_list);
    populate_quick_drawer_settings();
    enable_gesture_bubble_recursive(quick_drawer_settings_screen);
    return quick_drawer_settings_screen;
}
void gui_shell_settings_quick_drawer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!quick_drawer_settings_screen) build_quick_drawer_settings_screen();
    else populate_quick_drawer_settings();
    nav_push(quick_drawer_settings_screen);
}

typedef struct {
    const char * label;
    uint32_t hidden_bit;
} topbar_visibility_option_t;

static const topbar_visibility_option_t topbar_visibility_options[] = {
    { N_("Volume"), TOPBAR_HIDE_VOLUME },
    { N_("Clock"), TOPBAR_HIDE_CLOCK },
    { N_("Wi-Fi"), TOPBAR_HIDE_WIFI },
    { N_("Bluetooth"), TOPBAR_HIDE_BLUETOOTH },
    { N_("Wired Headphones"), TOPBAR_HIDE_HEADPHONES },
    { N_("Bluetooth Audio (A2DP)"), TOPBAR_HIDE_A2DP },
    { N_("USB Audio"), TOPBAR_HIDE_USB_AUDIO },
    { N_("Playback Status"), TOPBAR_HIDE_PLAYBACK },
    { N_("Bluetooth Codec"), TOPBAR_HIDE_CODEC },
    { N_("Battery Icon"), TOPBAR_HIDE_BATTERY },
};

static void populate_topbar_settings_screen(void);
static void populate_topbar_clock_position_screen(void);
static void topbar_clock_position_rebuild_async_cb(void * user_data) {
    (void)user_data;
    populate_topbar_clock_position_screen();
}

static void topbar_set_clicked_row_switch(lv_event_t * e, bool checked) {
    lv_obj_t * row = lv_event_get_target(e);
    uint32_t child_count = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(row, i);
        if (!lv_obj_check_type(child, &lv_switch_class)) continue;
        if (checked) lv_obj_add_state(child, LV_STATE_CHECKED);
        else lv_obj_remove_state(child, LV_STATE_CHECKED);
        return;
    }
}

static void topbar_visibility_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t bit = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    current_settings.topbar_hidden_elements ^= bit;
    current_settings.topbar_hidden_elements &= TOPBAR_HIDE_ALL;
    settings_save_async(&current_settings);
    gui_shell_apply_topbar_settings();
    topbar_set_clicked_row_switch(e,
        (current_settings.topbar_hidden_elements & bit) == 0);
}

static void topbar_battery_percentage_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.show_battery_percent = !current_settings.show_battery_percent;
    settings_save_async(&current_settings);
    gui_shell_apply_topbar_settings();
    topbar_set_clicked_row_switch(e, current_settings.show_battery_percent);
}

typedef struct {
    int position;
    const char * label;
} topbar_clock_position_option_t;

static const topbar_clock_position_option_t topbar_clock_position_options[] = {
    { TOPBAR_CLOCK_LEFT, N_("Left") },
    { TOPBAR_CLOCK_CENTER, N_("Center") },
    { TOPBAR_CLOCK_RIGHT, N_("Right") },
};

static void topbar_clock_position_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || (size_t)index >= sizeof(topbar_clock_position_options) / sizeof(topbar_clock_position_options[0])) return;
    current_settings.topbar_clock_position = topbar_clock_position_options[index].position;
    settings_save_async(&current_settings);
    gui_shell_apply_topbar_settings();
    lv_async_call(topbar_clock_position_rebuild_async_cb, NULL);
}

static void populate_topbar_clock_position_screen(void) {
    if (!settings_topbar_clock_position_list) return;
    lv_obj_clean(settings_topbar_clock_position_list);
    for (size_t i = 0; i < sizeof(topbar_clock_position_options) / sizeof(topbar_clock_position_options[0]); ++i) {
        add_pill_option_row(settings_topbar_clock_position_list,
                            TR(topbar_clock_position_options[i].label),
                            current_settings.topbar_clock_position == topbar_clock_position_options[i].position,
                            topbar_clock_position_option_cb, (void *)(intptr_t)i);
    }
}

static lv_obj_t * build_topbar_clock_position_screen(void) {
    lv_obj_t * title_label;
    settings_topbar_clock_position_screen = build_subsonic_list_screen(
        TR("Clock Position"), &title_label, &settings_topbar_clock_position_list);
    populate_topbar_clock_position_screen();
    finalize_screen_navigation(settings_topbar_clock_position_screen);
    return settings_topbar_clock_position_screen;
}

static void topbar_clock_position_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_topbar_clock_position_screen)
        settings_topbar_clock_position_screen = build_topbar_clock_position_screen();
    else populate_topbar_clock_position_screen();
    if (settings_topbar_clock_position_screen) nav_push(settings_topbar_clock_position_screen);
}

static void populate_topbar_settings_screen(void) {
    if (!settings_topbar_list) return;
    lv_obj_clean(settings_topbar_list);
    for (size_t i = 0; i < sizeof(topbar_visibility_options) / sizeof(topbar_visibility_options[0]); ++i) {
        lv_obj_t * row = add_pill_toggle_row(settings_topbar_list,
            TR(topbar_visibility_options[i].label),
            (current_settings.topbar_hidden_elements & topbar_visibility_options[i].hidden_bit) == 0,
            NULL);
        lv_obj_add_event_cb(row, topbar_visibility_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)topbar_visibility_options[i].hidden_bit);
    }
    add_pill_toggle_row(settings_topbar_list, TR("Battery Percentage"),
                        current_settings.show_battery_percent,
                        topbar_battery_percentage_toggle_cb);
    add_pill_chevron_row(settings_topbar_list, TR("Clock Position"), topbar_clock_position_row_cb);
}

static lv_obj_t * build_topbar_settings_screen(void) {
    lv_obj_t * title_label;
    settings_topbar_screen = build_subsonic_list_screen(TR("Top Bar"), &title_label,
                                                         &settings_topbar_list);
    populate_topbar_settings_screen();
    finalize_screen_navigation(settings_topbar_screen);
    return settings_topbar_screen;
}

void gui_shell_settings_topbar_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_topbar_screen) settings_topbar_screen = build_topbar_settings_screen();
    else populate_topbar_settings_screen();
    if (settings_topbar_screen) nav_push(settings_topbar_screen);
}

void gui_shell_settings_teardown_order(void) {
    gui_list_reorder_finish(&ui_order_drag_state, false);
    for (int id = 0; id < UI_DRAWER_COUNT; ++id)
        if (ui_order_drag_handles[id]) unregister_swipe_dead_zone(ui_order_drag_handles[id]);
    if (ui_order_screen) { lv_obj_delete(ui_order_screen); ui_order_screen = NULL; }
    ui_order_list = NULL;
    ui_order_title = NULL;
    memset(ui_order_rows, 0, sizeof(ui_order_rows));
    memset(ui_order_drag_handles, 0, sizeof(ui_order_drag_handles));
    memset(ui_order_visible_buttons, 0, sizeof(ui_order_visible_buttons));
    memset(&ui_order_drag_state, 0, sizeof(ui_order_drag_state));
    ui_order_drag_state.drag_id = -1;
}

void gui_shell_settings_teardown_after_plugin_order(void) {
    if (quick_drawer_settings_screen) { lv_obj_delete(quick_drawer_settings_screen); quick_drawer_settings_screen = NULL; }
    quick_drawer_settings_list = NULL;
    if (settings_topbar_screen) { lv_obj_delete(settings_topbar_screen); settings_topbar_screen = NULL; }
    settings_topbar_list = NULL;
    if (settings_topbar_clock_position_screen) {
        lv_obj_delete(settings_topbar_clock_position_screen);
        settings_topbar_clock_position_screen = NULL;
    }
    settings_topbar_clock_position_list = NULL;
}
