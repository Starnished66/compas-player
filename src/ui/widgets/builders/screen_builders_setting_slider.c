#include "screen_builders.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_shell.h"

#include <stdio.h>

/* Single-control settings column with an optional pill toggle. */

/* A transparent, padding-free container. */
static lv_obj_t * plain_box(lv_obj_t * parent) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t * styled_label(lv_obj_t * parent, const char * text, gui_font_role_t role, lv_style_t * style) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, style, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    return label;
}

/* Row of two labels, one at each end. The left one wraps and takes the slack. */
static lv_obj_t * two_ends_row(lv_obj_t * parent) {
    lv_obj_t * row = plain_box(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, BOARD_SCALE_PX(12), 0);
    return row;
}

/* Label row: an outer column that takes the shared minimum row height and
 * centres an inner label row in it (flex centres along the main axis even
 * when the height is content-sized). Returns the inner row; the caller adds
 * the control at its right end. */
static lv_obj_t * build_label_row(lv_obj_t * card, const char * text, lv_obj_t ** out_label) {
    lv_obj_t * outer = plain_box(card);
    lv_obj_set_size(outer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_style(outer, &native_row_min_style, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t * row = two_ends_row(outer);
    lv_obj_set_style_pad_hor(row, GUI_TEXT_INSET, 0);
    lv_obj_t * label = styled_label(row, text, GUI_FONT_ROLE_ROW, &style_theme_text_primary);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(label, 1);
    if (out_label) *out_label = label;
    return row;
}

static void build_toggle_row(lv_obj_t * card, const setting_slider_cfg_t * cfg, lv_obj_t ** out_switch) {
    lv_obj_t * label;
    lv_obj_t * row = build_label_row(card, cfg->toggle_label, &label);
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_add_style(sw, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (cfg->checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cfg->switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_update_layout(card);
    int32_t available = lv_obj_get_width(row) -
                        lv_obj_get_style_pad_left(row, LV_PART_MAIN) -
                        lv_obj_get_style_pad_right(row, LV_PART_MAIN) -
                        lv_obj_get_style_pad_column(row, LV_PART_MAIN) - lv_obj_get_width(sw);
    if (available > 0) {
        lv_obj_set_flex_grow(label, 0);
        lv_obj_set_width(label, available);
    }
    set_wrapped_label_two_line_height(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_USER_2);
    *out_switch = sw;
}

void setting_slider_set_index(const setting_slider_t * s, int32_t index) {
    fill_slider_t fill = { .slider = s->slider, .label = s->value_label,
                           .steps = s->steps, .fmt = s->fmt };
    fill_slider_set_value(&fill, index);
}

void setting_slider_set_max_index(const setting_slider_t * s, int32_t max_index) {
    lv_slider_set_range(s->slider, lv_slider_get_min_value(s->slider), max_index);
    setting_slider_set_index(s, lv_slider_get_value(s->slider));
}

static void ui_set_greyed_child(lv_obj_t * obj, bool greyed) {
    lv_obj_set_state(obj, LV_STATE_DISABLED, greyed);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) ui_set_greyed_child(lv_obj_get_child(obj, i), greyed);
}

void ui_set_greyed(lv_obj_t * obj, bool greyed) {
    lv_obj_set_style_opa(obj, greyed ? LV_OPA_40 : LV_OPA_COVER, 0);
    ui_set_greyed_child(obj, greyed);
}

void setting_slider_set_enabled(const setting_slider_t * s, bool enabled) {
    fill_slider_set_greyed(s->control, !enabled);
}

/* Row-style card shell: a column that grows with its content. */
static lv_obj_t * build_row_card(lv_obj_t * parent) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, ui_list_row_width());
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &native_row_min_style, 0);
    lv_obj_add_style(card, &pill_row_bg_style, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

lv_obj_t * build_setting_label_card(lv_obj_t * parent, const char * label, lv_obj_t ** out_row) {
    lv_obj_t * card = build_row_card(parent);
    *out_row = build_label_row(card, label, NULL);
    return card;
}

lv_obj_t * build_setting_hint(lv_obj_t * parent, const char * text) {
    lv_obj_t * note = styled_label(parent, text, GUI_FONT_ROLE_SUBTEXT, &style_theme_text_muted);
    lv_obj_set_width(note, ui_list_row_width() - 2 * GUI_TEXT_INSET);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    return note;
}

lv_obj_t * build_setting_toggle_card(lv_obj_t * parent, const char * label, const char * hint, bool checked,
                                     lv_event_cb_t switch_cb, lv_obj_t ** out_switch) {
    lv_obj_t * card = build_row_card(parent);
    build_toggle_row(card, &(setting_slider_cfg_t) { .toggle_label = label, .checked = checked,
                                                     .switch_cb = switch_cb }, out_switch);
    if (!hint) return card;
    build_setting_hint(parent, hint);
    return card;
}

setting_slider_t build_setting_slider(lv_obj_t * parent, const setting_slider_cfg_t * cfg) {
    setting_slider_t s = { .steps = cfg->steps, .fmt = cfg->fmt };
    s.card = plain_box(parent);
    lv_obj_set_size(s.card, ui_list_row_width(), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.card, GUI_ROW_GAP, 0);
    if (cfg->toggle_label) {
        pill_list_item_t toggle = {
            .label = cfg->toggle_label, .accessory = PILL_ACCESSORY_TOGGLE,
            .toggle_initial_state = cfg->checked, .on_toggle_change = cfg->switch_cb,
            .out_toggle_img = &s.toggle,
        };
        add_pill_list_rows(s.card, &toggle, 1);
    }
    fill_slider_t fill = build_fill_slider(s.card, &(fill_slider_cfg_t) {
        .width = lv_pct(100), .height = FILL_SLIDER_HEIGHT, .radius = FILL_SLIDER_RADIUS,
        .min = cfg->min_index, .max = cfg->max_index, .value = cfg->index,
        .caption = cfg->caption, .steps = cfg->steps, .fmt = cfg->fmt,
        .event_cb = cfg->slider_cb,
        .symmetrical = !cfg->steps && cfg->min_index < 0 && cfg->max_index > 0,
    });
    s.control = s.slider = fill.slider;
    s.value_label = fill.label;
    if (cfg->toggle_label && !cfg->checked && !cfg->control_always_on) setting_slider_set_enabled(&s, false);
    return s;
}

lv_obj_t * build_settings_body(lv_obj_t * scr) {
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    /* The bottom inset keeps content clear of the home indicator (V1). */
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    return body;
}

void setting_slider_sync(const setting_slider_t *s, bool enabled, int32_t index) {
    if (s->toggle) lv_obj_set_state(s->toggle, LV_STATE_CHECKED, enabled);
    setting_slider_set_enabled(s, enabled);
    setting_slider_set_index(s, index);
}

void setting_toggle_size(lv_obj_t *toggle, int32_t width, int32_t height, int32_t hit_padding) {
    lv_obj_set_size(toggle, width, height);
    lv_obj_set_ext_click_area(toggle, hit_padding);
}
