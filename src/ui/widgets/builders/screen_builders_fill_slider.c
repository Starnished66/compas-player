#include "screen_builders.h"
#include "../../shell/gui_shell.h"

/* Shared surface and fill for horizontal rows and vertical pills. */
void style_fill_slider(lv_obj_t * slider, int32_t radius) {
    lv_obj_remove_style_all(slider);
    lv_obj_set_ext_click_area(slider, 0);
    lv_obj_add_style(slider, gui_shell_drawer_row_style(), LV_PART_MAIN);
    /* The patched bar renderer clips the fill to this pill without a child layer. */
    lv_obj_set_style_radius(slider, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    /* LVGL reserves half the bar height around a slider for its knob, even an
     * invisible one. Shrinking the knob away keeps the draw area to the bar,
     * which the drawer's cached expansion frames depend on. */
    int32_t no_knob = -(BOARD_SCREEN_WIDTH + BOARD_SCREEN_HEIGHT);
    lv_obj_set_style_transform_width(slider, no_knob, LV_PART_KNOB);
    lv_obj_set_style_transform_height(slider, no_knob, LV_PART_KNOB);
}

void fill_slider_set_value(fill_slider_t * ui, int32_t value) {
    if (!ui->slider) return;
    lv_slider_set_value(ui->slider, value, LV_ANIM_OFF);
    if (!ui->label) return;
    int displayed = lv_slider_get_value(ui->slider);
    if (ui->steps) displayed = ui->steps[displayed];
    if (ui->fmt) {
        char buf[64];
        ui->fmt(buf, sizeof(buf), displayed);
        lv_label_set_text(ui->label, buf);
    } else {
        lv_label_set_text_fmt(ui->label, ui->value_format, displayed);
    }
}

/* Disabled keeps the row surface and fades the fill and its text, so the
 * row stays readable as a row. LV_STATE_DISABLED also blocks input. */
void fill_slider_set_greyed(lv_obj_t * slider, bool greyed) {
    if (!slider) return;
    lv_obj_set_state(slider, LV_STATE_DISABLED, greyed);
    for (uint32_t i = 0; i < lv_obj_get_child_count(slider); i++)
        lv_obj_set_style_opa(lv_obj_get_child(slider, i), greyed ? LV_OPA_40 : LV_OPA_COVER, 0);
}

/* A row's number sits as far from the right edge as from the top and bottom,
 * measured on the drawn digits rather than the font's line box. */
void fill_slider_place_value(fill_slider_t * ui) {
    if (!ui->slider || !ui->label) return;
    const lv_font_t * font = lv_obj_get_style_text_font(ui->label, LV_PART_MAIN);
    lv_font_glyph_dsc_t digit;
    if (!font || !lv_font_get_glyph_dsc(font, &digit, '0', '\0')) return;
    int32_t height = lv_obj_get_style_height(ui->slider, LV_PART_MAIN);
    int32_t line = lv_font_get_line_height(font);
    int32_t digit_top = line - font->base_line - digit.ofs_y - digit.box_h;
    /* The label ends at the advance, past the last digit's drawn pixels. */
    int32_t bearing = digit.adv_w - digit.ofs_x - digit.box_w;
    int32_t inset = (height - digit.box_h) / 2 - bearing;
    lv_obj_align(ui->label, LV_ALIGN_RIGHT_MID, -inset, line / 2 - (digit_top + digit.box_h / 2));
}

static void fill_slider_deleted_cb(lv_event_t * e) {
    unregister_swipe_dead_zone(lv_event_get_target(e));
}

fill_slider_t build_fill_slider(lv_obj_t * parent, const fill_slider_cfg_t * cfg) {
    fill_slider_t ui = { .slider = lv_slider_create(parent), .value_format = cfg->value_format,
                         .steps = cfg->steps, .fmt = cfg->fmt };
    bool vertical = cfg->width < cfg->height;
    style_fill_slider(ui.slider, cfg->radius);
    lv_obj_set_size(ui.slider, cfg->width, cfg->height);
    lv_slider_set_range(ui.slider, cfg->min, cfg->max);
    if (cfg->symmetrical) lv_slider_set_mode(ui.slider, LV_SLIDER_MODE_SYMMETRICAL);
    if (!cfg->icon_src && cfg->caption) {
        lv_obj_set_flex_flow(ui.slider, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ui.slider, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ui.slider, BOARD_SCALE_PX(12), LV_PART_MAIN);
        lv_obj_t * caption = lv_label_create(ui.slider);
        lv_label_set_text(caption, cfg->caption);
        lv_obj_add_style(caption, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(caption, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_style_pad_left(caption, BOARD_SCALE_PX(18), 0);
        lv_obj_set_width(caption, 0);
        lv_obj_set_flex_grow(caption, 1);
        row_label_apply_bounded_height(caption, gui_theme_font(GUI_FONT_ROLE_ROW));
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);
    }
    if (cfg->icon_src) {
        ui.icon = lv_image_create(ui.slider);
        lv_image_set_src(ui.icon, cfg->icon_src);
        lv_obj_add_style(ui.icon, &style_theme_text_primary, 0);
        lv_obj_align(ui.icon, vertical ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_LEFT_MID,
                     vertical ? 0 : BOARD_SCALE_PX(21), vertical ? -BOARD_SCALE_PY(16) : 0);
        if (cfg->icon_clickable) lv_obj_add_flag(ui.icon, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(ui.icon, LV_OBJ_FLAG_CLICKABLE);
    }
    if (cfg->value_format || cfg->fmt) {
        ui.label = lv_label_create(ui.slider);
        lv_obj_add_style(ui.label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(ui.label, &app_font_player_meta, 0);
        lv_obj_align(ui.label, vertical ? LV_ALIGN_TOP_MID : LV_ALIGN_RIGHT_MID,
                     vertical ? 0 : -BOARD_SCALE_PX(18), vertical ? BOARD_SCALE_PY(14) : 0);
        if (!cfg->icon_src && cfg->caption)
            lv_obj_set_style_margin_right(ui.label, BOARD_SCALE_PX(18), 0);
        lv_obj_remove_flag(ui.label, LV_OBJ_FLAG_CLICKABLE);
    }
    if (!vertical && cfg->icon_src) fill_slider_place_value(&ui);
    fill_slider_set_value(&ui, cfg->value);
    if (cfg->event_cb) lv_obj_add_event_cb(ui.slider, cfg->event_cb, LV_EVENT_ALL, cfg->user_data);
    /* The whole row is a drag target: back and Now Playing swipes must not
     * start on it. Screen-wide gestures skip this area until it is deleted. */
    lv_obj_remove_flag(ui.slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(ui.slider);
    lv_obj_add_event_cb(ui.slider, fill_slider_deleted_cb, LV_EVENT_DELETE, NULL);
    return ui;
}
