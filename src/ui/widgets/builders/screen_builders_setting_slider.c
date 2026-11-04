#include "screen_builders.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_shell.h"

#include <stdio.h>

/* Single-control settings card: an optional toggle row, a divider and a
 * control area (caption + value, slider, range ends) in one row-style card. */

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
static lv_obj_t * build_label_row(lv_obj_t * card, const char * text) {
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
    return row;
}

static void build_toggle_row(lv_obj_t * card, const setting_slider_cfg_t * cfg, lv_obj_t ** out_switch) {
    lv_obj_t * row = build_label_row(card, cfg->toggle_label);
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_add_style(sw, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (cfg->checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cfg->switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *out_switch = sw;
}

static void build_divider(lv_obj_t * card) {
    lv_obj_t * divider = plain_box(card);
    lv_obj_set_size(divider, lv_pct(100), 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, 0);
}

static lv_obj_t * build_slider(lv_obj_t * parent, const setting_slider_cfg_t * cfg) {
    lv_obj_t * slider = lv_slider_create(parent);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
    lv_slider_set_range(slider, cfg->min_index, cfg->max_index);
    lv_slider_set_value(slider, cfg->index, LV_ANIM_OFF);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_margin_ver(slider, SLIDER_KNOB_SIZE / 4, 0);
    lv_obj_add_event_cb(slider, cfg->slider_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(slider, BOARD_SCALE_PX(26));
    return slider;
}

/* Value shown for slider position `index`: the step's value, or the index
 * itself when the slider moves over raw values. */
static void format_at(const setting_slider_t * s, int32_t index, char * buf, size_t size) {
    s->fmt(buf, size, s->steps ? s->steps[index] : (int) index);
}

static void set_label_at(const setting_slider_t * s, lv_obj_t * label, int32_t index) {
    char buf[32];
    format_at(s, index, buf, sizeof(buf));
    lv_label_set_text(label, buf);
}

void setting_slider_set_index(const setting_slider_t * s, int32_t index) {
    set_label_at(s, s->value_label, index);
}

void setting_slider_set_max_index(const setting_slider_t * s, int32_t max_index) {
    lv_slider_set_range(s->slider, lv_slider_get_min_value(s->slider), max_index);
    set_label_at(s, s->max_label, max_index);
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
    ui_set_greyed(s->control, !enabled);
}

/* Control area: caption and value on one line, the slider, then the range ends. */
static lv_obj_t * build_control_area(lv_obj_t * card, const setting_slider_cfg_t * cfg, setting_slider_t * s) {
    lv_obj_t * area = plain_box(card);
    lv_obj_set_size(area, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(area, GUI_TEXT_INSET, 0);
    lv_obj_set_style_pad_ver(area, GUI_TEXT_INSET / 2, 0);
    lv_obj_set_style_pad_gap(area, GUI_ROW_GAP, 0);

    lv_obj_t * head = two_ends_row(area);
    lv_obj_t * caption = styled_label(head, cfg->caption, GUI_FONT_ROLE_SUBTEXT, &style_theme_text_muted);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(caption, 1);
    s->value_label = styled_label(head, "", GUI_FONT_ROLE_ROW, &style_theme_text_primary);
    s->slider = build_slider(area, cfg);
    lv_obj_t * ends = two_ends_row(area);
    s->min_label = styled_label(ends, "", GUI_FONT_ROLE_SUBTEXT, &style_theme_text_muted);
    s->max_label = styled_label(ends, "", GUI_FONT_ROLE_SUBTEXT, &style_theme_text_muted);
    return area;
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
    *out_row = build_label_row(card, label);
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
    s.card = build_row_card(parent);
    if (cfg->toggle_label) {
        build_toggle_row(s.card, cfg, &s.toggle);
        build_divider(s.card);
    }
    s.control = build_control_area(s.card, cfg, &s);
    set_label_at(&s, s.value_label, cfg->index);
    set_label_at(&s, s.min_label, cfg->min_index);
    set_label_at(&s, s.max_label, cfg->max_index);
    if (cfg->toggle_label && !cfg->checked && !cfg->control_always_on) setting_slider_set_enabled(&s, false);
    lv_obj_remove_flag(s.card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(s.card);
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
