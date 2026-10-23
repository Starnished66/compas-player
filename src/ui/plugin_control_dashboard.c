#include "plugin_control_dashboard.h"

#include "plugin_manager.h"
#include "gui_theme.h"
#include "board_config.h"
#include "fallback_font.h"
#include "screen_builders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern lv_style_t style_theme_card_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_muted;
extern void register_swipe_dead_zone(lv_obj_t * obj);
extern void unregister_swipe_dead_zone(lv_obj_t * obj);

#define DASH_SCALE(x) BOARD_SCALE_PX(x)

typedef struct plugin_control_dashboard_context plugin_control_dashboard_context_t;
typedef struct {
    plugin_control_dashboard_context_t * context;
    int row;
    int delta;
    bool long_fired;
    lv_obj_t * arc;
} dash_event_data_t;

struct plugin_control_dashboard_context {
    int slot;
    int tempo_min;
    int tempo_max;
    plugin_control_dashboard_callbacks_t callbacks;
    lv_obj_t * value_label;
    lv_obj_t * arc;
    dash_event_data_t row_events[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    dash_event_data_t decrement_event;
    dash_event_data_t increment_event;
};

static void dash_context_deleted(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    plugin_control_dashboard_context_t * context = lv_event_get_user_data(e);
    if (context->arc) unregister_swipe_dead_zone(context->arc);
    free(context);
}

static lv_obj_t * dash_panel(lv_obj_t * parent, int32_t height) {
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_add_style(panel, &style_theme_card_bg, 0);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, height);
    lv_obj_set_style_radius(panel, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, DASH_SCALE(12), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

static void dash_tempo_set(plugin_control_dashboard_context_t * context, int value, bool notify) {
    if (value < context->tempo_min) value = context->tempo_min;
    if (value > context->tempo_max) value = context->tempo_max;
    char text[12];
    snprintf(text, sizeof(text), "%d", value);
    lv_label_set_text(context->value_label, text);
    if (notify && context->callbacks.value)
        context->callbacks.value(context->slot, context->row_events[1].row, value);
}

static void dash_arc_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    dash_event_data_t * data = lv_event_get_user_data(e);
    plugin_control_dashboard_context_t * context = data->context;
    int value = lv_arc_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_VALUE_CHANGED) dash_tempo_set(context, value, false);
    else if (code == LV_EVENT_RELEASED) dash_tempo_set(context, value, true);
}

static void dash_nudge_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_CLICKED) return;
    dash_event_data_t * data = lv_event_get_user_data(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        data->long_fired = true;
        int value = lv_arc_get_value(data->arc) + data->delta * 10;
        if (value < data->context->tempo_min) value = data->context->tempo_min;
        if (value > data->context->tempo_max) value = data->context->tempo_max;
        lv_arc_set_value(data->arc, value);
        dash_tempo_set(data->context, value, true);
    } else if (code == LV_EVENT_CLICKED) {
        bool was_long = data->long_fired;
        data->long_fired = false;
        if (was_long) return;
        int value = lv_arc_get_value(data->arc) + data->delta;
        if (value < data->context->tempo_min) value = data->context->tempo_min;
        if (value > data->context->tempo_max) value = data->context->tempo_max;
        lv_arc_set_value(data->arc, value);
        dash_tempo_set(data->context, value, true);
    }
}

static void dash_tap_event(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dash_event_data_t * data = lv_event_get_user_data(e);
    if (data->context->callbacks.tap)
        data->context->callbacks.tap(data->context->slot, data->row);
}

static void dash_label(lv_obj_t * label, const char * text, gui_font_role_t role, bool muted) {
    lv_obj_add_style(label, muted ? &style_theme_text_muted : &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    lv_label_set_text(label, text ? text : "");
}

static lv_color_t dash_theme_text_color(void) {
    lv_style_value_t color;
    if (lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &color) == LV_STYLE_RES_FOUND)
        return color.color;
    return lv_color_hex(GUI_COLOR_PRIMARY);
}

static void dash_recolor_latest_icon(lv_obj_t * row, lv_color_t color) {
    uint32_t count = lv_obj_get_child_count(row);
    if (count == 0) return;
    lv_obj_t * wrapper = lv_obj_get_child(row, (int32_t)count - 1);
    if (lv_obj_get_child_count(wrapper) == 0) return;
    lv_obj_t * image = lv_obj_get_child(wrapper, 0);
    lv_obj_set_style_image_recolor(image, color, 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

static void dash_build_markers(lv_obj_t * parent, const plugin_control_dashboard_options_t * options) {
    int count = options ? options->markers : 4;
    if (count <= 0) return;
    if (count > 12) count = 12;
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, DASH_SCALE(30));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, DASH_SCALE(10), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < count; i++) {
        lv_obj_t * dot = lv_obj_create(row);
        lv_obj_set_size(dot, DASH_SCALE(i == 0 ? 12 : 9), DASH_SCALE(i == 0 ? 12 : 9));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_color_t muted = lv_color_hex(GUI_COLOR_SECONDARY);
        lv_style_value_t color;
        if (lv_style_get_prop(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, &color) == LV_STYLE_RES_FOUND)
            muted = color.color;
        bool accented = options && options->accent_first && i == 0;
        lv_obj_set_style_bg_color(dot, accented ? accent_lv_color() : muted, 0);
        lv_obj_set_style_bg_opa(dot, options && !options->active ? LV_OPA_40 : LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static void dash_build_tile(lv_obj_t * parent, plugin_control_dashboard_context_t * context,
                            const plugin_control_dashboard_row_t * item, int row_index,
                            int grid_col, int grid_row, const char * fallback_detail, bool primary) {
    lv_obj_t * tile = lv_button_create(parent);
    lv_obj_set_size(tile, LV_PCT(100), DASH_SCALE(78));
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, grid_col, 1,
                         LV_GRID_ALIGN_STRETCH, grid_row, 1);
    /* The same framed surface as the menu rows; the primary action is the
     * accent, like a Breeze default button. */
    lv_obj_add_style(tile, &pill_row_bg_style, 0);
    lv_obj_add_style(tile, &list_row_pressed_style, LV_STATE_PRESSED);
    if (primary) {
        lv_obj_set_style_bg_color(tile, accent_lv_color(), 0);
        lv_obj_set_style_border_width(tile, 0, 0);
    }
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(tile, dash_tap_event, LV_EVENT_ALL, &context->row_events[row_index]);
    lv_obj_t * title = lv_label_create(tile);
    dash_label(title, item->label, GUI_FONT_ROLE_SUBTEXT, false);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));
    const char * detail = item->detail && item->detail[0] ? item->detail : fallback_detail;
    lv_obj_t * caption = NULL;
    if (detail && detail[0]) {
        caption = lv_label_create(tile);
        dash_label(caption, detail, GUI_FONT_ROLE_STATUS, true);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        lv_obj_set_width(caption, LV_PCT(100));
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -DASH_SCALE(8));
    }
    if (item->icon_path && item->icon_path[0])
        pill_row_apply_icon(tile, title, item->icon_path, DASH_SCALE(26), LV_ALIGN_LEFT_MID,
                            DASH_SCALE(10), caption ? -DASH_SCALE(12) : 0);
    lv_obj_set_width(title, LV_PCT(item->icon_path && item->icon_path[0] ? 64 : 90));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, caption ? -DASH_SCALE(12) : 0);
    if (detail && detail[0]) lv_obj_set_width(caption, LV_PCT(90));
    if (primary) {
        lv_color_t foreground = lv_color_hex(gui_theme_prefers_light_text(accent_lv_color())
                                                 ? GUI_BREEZE_TEXT_ON_DARK : GUI_BREEZE_TEXT_ON_LIGHT);
        lv_obj_set_style_text_color(title, foreground, 0);
        if (item->icon_path && item->icon_path[0]) dash_recolor_latest_icon(tile, foreground);
    } else if (item->icon_path && item->icon_path[0]) {
        dash_recolor_latest_icon(tile, dash_theme_text_color());
    }
}

bool plugin_control_dashboard_build(lv_obj_t * parent,
                                    const plugin_control_dashboard_row_t * rows,
                                    int count,
                                    int slot,
                                    const plugin_control_dashboard_options_t * options,
                                    const plugin_control_dashboard_callbacks_t * callbacks) {
    if (!parent || !rows || count != 7 || !callbacks) return false;
    int tempo = -1;
    for (int i = 0; i < count; i++) {
        if (rows[i].type == PLUGIN_SETTINGS_ROW_SLIDER) {
            if (tempo >= 0) return false;
            tempo = i;
        }
    }
    if (tempo != 1 || rows[0].type != PLUGIN_SETTINGS_ROW_TAP || rows[2].type != PLUGIN_SETTINGS_ROW_TAP)
        return false;
    for (int i = 3; i < 7; i++) if (rows[i].type != PLUGIN_SETTINGS_ROW_TAP) return false;

    plugin_control_dashboard_context_t * context = calloc(1, sizeof(*context));
    if (!context) return false;
    context->slot = slot;
    context->tempo_min = rows[tempo].slider_min;
    context->tempo_max = rows[tempo].slider_max > context->tempo_min ? rows[tempo].slider_max : context->tempo_min + 1;
    context->callbacks = *callbacks;
    for (int i = 0; i < count; i++) {
        context->row_events[i].context = context;
        context->row_events[i].row = i;
    }
    context->decrement_event.context = context;
    context->decrement_event.row = tempo;
    context->decrement_event.delta = -1;
    context->decrement_event.arc = NULL;
    context->increment_event.context = context;
    context->increment_event.row = tempo;
    context->increment_event.delta = 1;
    context->increment_event.arc = NULL;

    /* The body owns its callbacks and context, so replacing its contents also
     * releases every screen-specific pointer without cross-slot state. */
    lv_obj_clean(parent);
    lv_obj_t * body = lv_obj_create(parent);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, DASH_SCALE(12), 0);
    lv_obj_set_style_pad_row(body, DASH_SCALE(10), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, dash_context_deleted, LV_EVENT_DELETE, context);

    dash_build_markers(body, options);
    lv_obj_t * dial_panel = dash_panel(body, DASH_SCALE(300));
    lv_obj_set_style_bg_opa(dial_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(dial_panel, 0, 0);
    lv_obj_t * arc = lv_arc_create(dial_panel);
    lv_obj_set_size(arc, DASH_SCALE(280), DASH_SCALE(280));
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, context->tempo_min, context->tempo_max);
    lv_arc_set_value(arc, rows[tempo].slider_value);
    lv_obj_set_style_arc_width(arc, DASH_SCALE(9), LV_PART_MAIN);
    lv_color_t arc_muted = lv_color_hex(GUI_COLOR_SECONDARY);
    lv_style_value_t arc_muted_prop;
    if (lv_style_get_prop(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, &arc_muted_prop) == LV_STYLE_RES_FOUND)
        arc_muted = arc_muted_prop.color;
    lv_obj_set_style_arc_color(arc, arc_muted, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, DASH_SCALE(9), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, accent_lv_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    context->arc = arc;
    lv_obj_add_event_cb(arc, dash_arc_event, LV_EVENT_ALL, &context->row_events[tempo]);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(arc);

    lv_obj_t * center = lv_obj_create(dial_panel);
    lv_obj_set_size(center, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_center(center);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_style_pad_all(center, 0, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    context->value_label = lv_label_create(center);
    char bpm[12]; snprintf(bpm, sizeof(bpm), "%d", rows[tempo].slider_value);
    dash_label(context->value_label, bpm, GUI_FONT_ROLE_TITLE, false);
    lv_obj_set_style_text_font(context->value_label, &app_font_display, 0);
    lv_obj_t * unit = lv_label_create(center);
    dash_label(unit, "BPM", GUI_FONT_ROLE_STATUS, true);

    lv_obj_t * down = lv_button_create(dial_panel);
    lv_obj_set_size(down, DASH_SCALE(42), DASH_SCALE(42));
    lv_obj_add_style(down, &pill_row_bg_style, 0);
    lv_obj_add_style(down, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_radius(down, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(down, 0, 0);
    lv_obj_add_event_cb(down, dash_nudge_event, LV_EVENT_ALL, &context->decrement_event);
    lv_obj_t * down_label = lv_label_create(down);
    dash_label(down_label, "-", GUI_FONT_ROLE_TITLE, false);
    lv_obj_center(down_label);
    lv_obj_align(down, LV_ALIGN_LEFT_MID, DASH_SCALE(10), 0);
    context->decrement_event.arc = arc;

    lv_obj_t * up = lv_button_create(dial_panel);
    lv_obj_set_size(up, DASH_SCALE(42), DASH_SCALE(42));
    lv_obj_add_style(up, &pill_row_bg_style, 0);
    lv_obj_add_style(up, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_radius(up, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(up, 0, 0);
    lv_obj_add_event_cb(up, dash_nudge_event, LV_EVENT_ALL, &context->increment_event);
    lv_obj_t * up_label = lv_label_create(up);
    dash_label(up_label, "+", GUI_FONT_ROLE_TITLE, false);
    lv_obj_center(up_label);
    lv_obj_align(up, LV_ALIGN_RIGHT_MID, -DASH_SCALE(10), 0);
    context->increment_event.arc = arc;

    lv_obj_t * tiles = lv_obj_create(body);
    lv_obj_set_width(tiles, LV_PCT(100));
    lv_obj_set_height(tiles, 3 * DASH_SCALE(78) + 2 * DASH_SCALE(8));
    lv_obj_set_style_bg_opa(tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tiles, 0, 0);
    lv_obj_set_style_pad_all(tiles, 0, 0);
    lv_obj_set_style_pad_row(tiles, DASH_SCALE(8), 0);
    lv_obj_set_style_pad_column(tiles, DASH_SCALE(8), 0);
    static int32_t grid_columns[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t grid_rows[] = {
        DASH_SCALE(78), DASH_SCALE(78), DASH_SCALE(78), LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_layout(tiles, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(tiles, grid_columns, grid_rows);
    lv_obj_remove_flag(tiles, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    static const int dashboard_rows[6] = { 0, 2, 3, 4, 5, 6 };
    for (int i = 0; i < 6; i++) {
        int row = dashboard_rows[i];
        const char * detail = (row == 4 && options) ? options->detail : NULL;
        dash_build_tile(tiles, context, &rows[row], row, i % 2, i / 2, detail, row == 0);
    }
    return true;
}
