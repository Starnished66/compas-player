#include "gui_color_picker.h"
#include "../widgets/builders/screen_builders.h"
#include "../shell/gui_shell.h"
#include "../shell/gui_notifications.h"
#include "../text/rendering/gui_text_input.h"
#include "../text/i18n/i18n.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The square is the hue as a flat color under a white-to-clear and a
 * clear-to-black gradient, so changing hue only recolors one object. */
#define PICKER_SV_HEIGHT BOARD_SCALE_PX(200)
#define PICKER_HUE_TRACK_HEIGHT BOARD_SCALE_PX(36)
#define PICKER_HUE_BAR_HEIGHT BOARD_SCALE_PX(20)
#define PICKER_CURSOR_SIZE BOARD_SCALE_PX(28)
#define PICKER_PRESET_SIZE BOARD_SCALE_PX(44)

typedef struct {
    gui_color_picker_cb_t on_change;
    void * user_data;
    lv_color_hsv_t hsv;
    uint32_t rgb;
    int32_t width; /* of the square and the hue track */
    bool active;
    lv_obj_t * swatch;
    lv_obj_t * hex_label;
    lv_obj_t * info_label;
    lv_obj_t * card;
    lv_obj_t * sv_area;
    lv_obj_t * sv_cursor;
    lv_obj_t * hue_track;
    lv_obj_t * hue_knob;
    lv_obj_t * hex_value;
    int preset_count;
    struct { lv_obj_t * obj; uint32_t rgb; } presets[];
} color_picker_t;

/* The picker whose hex entry is open, so a late keyboard result never lands
 * on a picker that was deleted meanwhile. */
static color_picker_t * hex_edit_target;
static lv_grad_dsc_t picker_sat_grad;
static lv_grad_dsc_t picker_val_grad;
static lv_grad_dsc_t picker_hue_grad;

lv_obj_t * gui_card_create(lv_obj_t * parent, int32_t width) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_all(card, GUI_CARD_PAD, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t * gui_card_text(lv_obj_t * parent, const char * text, gui_font_role_t role, bool muted) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, muted ? &style_theme_text_muted : &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    return label;
}

/* Plain layer or cursor: never takes touches, so they reach the area below. */
lv_obj_t * gui_plain_obj_create(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

bool gui_color_parse_hex(const char * text, uint32_t * rgb) {
    if (!text) return false;
    if (*text == '#') text++;
    if (strlen(text) != 6) return false;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char) text[i])) return false;
    *rgb = (uint32_t) strtoul(text, NULL, 16);
    return true;
}

static uint32_t picker_hsv_rgb(lv_color_hsv_t hsv) {
    lv_color_t c = lv_color_hsv_to_rgb(hsv.h, hsv.s, hsv.v);
    return ((uint32_t) c.red << 16) | ((uint32_t) c.green << 8) | c.blue;
}

/* Mirrors st->rgb/hsv into every part of the picker. */
static void picker_refresh(color_picker_t * st) {
    char hex[16];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned int) st->rgb);
    lv_obj_set_style_bg_color(st->swatch, lv_color_hex(st->rgb), 0);
    lv_label_set_text(st->hex_label, hex);
    lv_label_set_text(st->hex_value, hex);

    lv_obj_set_style_bg_color(st->sv_area, lv_color_hsv_to_rgb(st->hsv.h, 100, 100), 0);
    int32_t x = (int32_t) st->hsv.s * (st->width - 1) / 100;
    int32_t y = (int32_t) (100 - st->hsv.v) * (PICKER_SV_HEIGHT - 1) / 100;
    lv_obj_set_pos(st->sv_cursor, x - PICKER_CURSOR_SIZE / 2, y - PICKER_CURSOR_SIZE / 2);
    lv_obj_set_style_bg_color(st->sv_cursor, lv_color_hex(st->rgb), 0);
    int32_t hx = (int32_t) st->hsv.h * (st->width - 1) / 359;
    lv_obj_set_pos(st->hue_knob, hx - PICKER_CURSOR_SIZE / 2, (PICKER_HUE_TRACK_HEIGHT - PICKER_CURSOR_SIZE) / 2);
    lv_obj_set_style_bg_color(st->hue_knob, lv_color_hsv_to_rgb(st->hsv.h, 100, 100), 0);

    for (int i = 0; i < st->preset_count; i++) {
        bool selected = st->active && st->presets[i].rgb == st->rgb;
        lv_obj_set_style_border_width(st->presets[i].obj, selected ? BOARD_SCALE_PX(4) : 0, 0);
    }
}

static void picker_notify(color_picker_t * st, bool final) {
    if (st->on_change) st->on_change(st->rgb, final, st->user_data);
}

/* A drag changed the hue/saturation/brightness. */
static void picker_hsv_changed(color_picker_t * st) {
    st->rgb = picker_hsv_rgb(st->hsv);
    picker_refresh(st);
    picker_notify(st, false);
}

static void picker_set_rgb(color_picker_t * st, uint32_t rgb) {
    rgb &= 0xFFFFFF;
    if (rgb == st->rgb) return; /* keeps a drag's own HSV rather than re-deriving it */
    lv_color_t c = lv_color_hex(rgb);
    st->rgb = rgb;
    st->hsv = lv_color_rgb_to_hsv(c.red, c.green, c.blue);
    picker_refresh(st);
}

/* Takes a settled color, e.g. a preset or a typed hex code. */
static void picker_pick_rgb(color_picker_t * st, uint32_t rgb) {
    picker_set_rgb(st, rgb);
    picker_notify(st, true);
}

static int32_t picker_touch_offset(lv_obj_t * obj, bool vertical, int32_t span) {
    lv_indev_t * indev = lv_indev_active();
    if (!indev) return 0;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t v = vertical ? p.y - a.y1 : p.x - a.x1;
    if (v < 0) v = 0;
    if (v > span - 1) v = span - 1;
    return v;
}

static void picker_sv_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    color_picker_t * st = lv_event_get_user_data(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = picker_touch_offset(st->sv_area, false, st->width);
        int32_t y = picker_touch_offset(st->sv_area, true, PICKER_SV_HEIGHT);
        st->hsv.s = (uint8_t) (x * 100 / (st->width - 1));
        st->hsv.v = (uint8_t) (100 - y * 100 / (PICKER_SV_HEIGHT - 1));
        picker_hsv_changed(st);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        picker_notify(st, true);
    }
}

static void picker_hue_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    color_picker_t * st = lv_event_get_user_data(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = picker_touch_offset(st->hue_track, false, st->width);
        st->hsv.h = (uint16_t) (x * 359 / (st->width - 1));
        /* A gray pick has no visible hue; lift it so the drag shows color. */
        if (st->hsv.s < 20) st->hsv.s = 80;
        if (st->hsv.v < 20) st->hsv.v = 90;
        picker_hsv_changed(st);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        picker_notify(st, true);
    }
}

static void picker_preset_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    color_picker_t * st = lv_event_get_user_data(e);
    for (int i = 0; i < st->preset_count; i++)
        if (st->presets[i].obj == lv_event_get_target(e)) picker_pick_rgb(st, st->presets[i].rgb);
}

static void picker_hex_done_cb(const char * text, void * user_data) {
    color_picker_t * st = user_data;
    if (st != hex_edit_target) return;
    hex_edit_target = NULL;
    uint32_t rgb;
    if (gui_color_parse_hex(text, &rgb)) picker_pick_rgb(st, rgb);
    else show_info_toast(TR("Invalid hex code"));
}

static void picker_hex_row_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    color_picker_t * st = lv_event_get_user_data(e);
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", (unsigned int) st->rgb);
    hex_edit_target = st;
    show_text_entry(TR("Hex code"), hex, false, false, picker_hex_done_cb, st);
}

static void picker_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    color_picker_t * st = lv_event_get_user_data(e);
    unregister_swipe_dead_zone(st->card);
    if (hex_edit_target == st) hex_edit_target = NULL;
    free(st);
}

/* A drag surface: keeps the drag (no body scroll, no app swipe-back). */
static void picker_make_drag_surface(lv_obj_t * obj, lv_event_cb_t cb, color_picker_t * st) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                       LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_ALL, st);
}

static lv_obj_t * picker_cursor(lv_obj_t * parent) {
    lv_obj_t * c = gui_plain_obj_create(parent);
    lv_obj_set_size(c, PICKER_CURSOR_SIZE, PICKER_CURSOR_SIZE);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_outline_color(c, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_outline_width(c, 1, 0);
    return c;
}

static void picker_build_preview(color_picker_t * st, lv_obj_t * parent, int32_t width) {
    lv_obj_t * preview = gui_card_create(parent, width);
    lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview, BOARD_SCALE_PX(16), 0);
    st->swatch = gui_plain_obj_create(preview);
    lv_obj_set_size(st->swatch, BOARD_SCALE_PX(76), BOARD_SCALE_PX(76));
    lv_obj_set_style_radius(st->swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(st->swatch, LV_OPA_COVER, 0);
    lv_obj_t * info = gui_plain_obj_create(preview);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(6), 0);
    st->hex_label = gui_card_text(info, "", GUI_FONT_ROLE_TITLE, false);
    st->info_label = gui_card_text(info, "", GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(st->info_label, lv_pct(100));
    lv_label_set_long_mode(st->info_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(st->info_label, LV_OBJ_FLAG_HIDDEN);
}

static void picker_build_sliders(color_picker_t * st, lv_obj_t * parent, int32_t width) {
    st->card = gui_card_create(parent, width);
    lv_obj_set_flex_flow(st->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(st->card, BOARD_SCALE_PX(14), 0);
    gui_card_text(st->card, TR("Custom color"), GUI_FONT_ROLE_ROW, false);

    st->sv_area = lv_obj_create(st->card);
    lv_obj_remove_style_all(st->sv_area);
    lv_obj_set_size(st->sv_area, st->width, PICKER_SV_HEIGHT);
    lv_obj_set_style_radius(st->sv_area, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_bg_opa(st->sv_area, LV_OPA_COVER, 0);
    picker_make_drag_surface(st->sv_area, picker_sv_event_cb, st);
    static const lv_opa_t fade_out[2] = { LV_OPA_COVER, LV_OPA_TRANSP };
    static const lv_opa_t fade_in[2] = { LV_OPA_TRANSP, LV_OPA_COVER };
    lv_color_t whites[2] = { lv_color_white(), lv_color_white() };
    lv_color_t blacks[2] = { lv_color_black(), lv_color_black() };
    lv_grad_init_stops(&picker_sat_grad, whites, fade_out, NULL, 2);
    lv_grad_horizontal_init(&picker_sat_grad);
    lv_grad_init_stops(&picker_val_grad, blacks, fade_in, NULL, 2);
    lv_grad_vertical_init(&picker_val_grad);
    lv_grad_dsc_t * layers[2] = { &picker_sat_grad, &picker_val_grad };
    for (int i = 0; i < 2; i++) {
        lv_obj_t * layer = gui_plain_obj_create(st->sv_area);
        lv_obj_set_size(layer, st->width, PICKER_SV_HEIGHT);
        lv_obj_set_style_radius(layer, BOARD_SCALE_PX(12), 0);
        lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad(layer, layers[i], 0);
    }
    st->sv_cursor = picker_cursor(st->sv_area);

    st->hue_track = lv_obj_create(st->card);
    lv_obj_remove_style_all(st->hue_track);
    lv_obj_set_size(st->hue_track, st->width, PICKER_HUE_TRACK_HEIGHT);
    picker_make_drag_surface(st->hue_track, picker_hue_event_cb, st);
    lv_color_t hues[7];
    for (int i = 0; i < 7; i++) hues[i] = lv_color_hsv_to_rgb((uint16_t) (i * 60 % 360), 100, 100);
    lv_grad_init_stops(&picker_hue_grad, hues, NULL, NULL, 7);
    lv_grad_horizontal_init(&picker_hue_grad);
    lv_obj_t * hue_bar = gui_plain_obj_create(st->hue_track);
    lv_obj_set_size(hue_bar, st->width, PICKER_HUE_BAR_HEIGHT);
    lv_obj_align(hue_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(hue_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hue_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(hue_bar, &picker_hue_grad, 0);
    st->hue_knob = picker_cursor(st->hue_track);

    /* Tap to type the value with the keyboard. */
    lv_obj_t * row = lv_obj_create(st->card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, st->width, BOARD_SCALE_PX(44));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, picker_hex_row_event_cb, LV_EVENT_CLICKED, st);
    gui_card_text(row, TR("Hex code"), GUI_FONT_ROLE_BODY, true);
    st->hex_value = gui_card_text(row, "", GUI_FONT_ROLE_BODY, false);
    lv_obj_add_style(st->hex_value, gui_theme_accent_style(), 0);

    lv_obj_remove_flag(st->card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(st->card);
}

static void picker_build_presets(color_picker_t * st, lv_obj_t * parent, int32_t width, const uint32_t * presets) {
    lv_obj_t * card = gui_card_create(parent, width);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(14), 0);
    lv_obj_t * title = gui_card_text(card, TR("Presets"), GUI_FONT_ROLE_ROW, false);
    lv_obj_set_width(title, lv_pct(100));
    for (int i = 0; i < st->preset_count; i++) {
        lv_obj_t * sw = lv_obj_create(card);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, PICKER_PRESET_SIZE, PICKER_PRESET_SIZE);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(presets[i]), 0);
        lv_obj_set_style_border_color(sw, lv_color_white(), 0);
        lv_obj_set_style_outline_color(sw, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(sw, LV_OPA_30, 0);
        lv_obj_set_style_outline_width(sw, 1, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(sw, BOARD_SCALE_PX(6));
        lv_obj_add_event_cb(sw, picker_preset_event_cb, LV_EVENT_CLICKED, st);
        st->presets[i].obj = sw;
        st->presets[i].rgb = presets[i] & 0xFFFFFF;
    }
}

lv_obj_t * gui_color_picker_create(lv_obj_t * parent, int32_t width, uint32_t rgb, const uint32_t * presets,
                                   int preset_count, gui_color_picker_cb_t on_change, void * user_data) {
    if (!presets || preset_count < 0) preset_count = 0;
    if (preset_count > GUI_COLOR_PICKER_MAX_PRESETS) preset_count = GUI_COLOR_PICKER_MAX_PRESETS;
    color_picker_t * st = calloc(1, sizeof(*st) + (size_t) preset_count * sizeof(st->presets[0]));
    if (!st) return NULL;
    st->on_change = on_change;
    st->user_data = user_data;
    st->width = width - 2 * GUI_CARD_PAD;
    st->active = true;
    st->preset_count = preset_count;

    lv_obj_t * root = gui_plain_obj_create(parent);
    lv_obj_set_size(root, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, BOARD_SCALE_PX(14), 0);
    picker_build_preview(st, root, width);
    picker_build_sliders(st, root, width);
    if (preset_count > 0) picker_build_presets(st, root, width, presets);
    lv_obj_set_user_data(root, st);
    lv_obj_add_event_cb(root, picker_delete_cb, LV_EVENT_DELETE, st);
    st->rgb = ~rgb & 0xFFFFFF; /* differs, so the first set_rgb draws */
    picker_set_rgb(st, rgb);
    return root;
}

void gui_color_picker_set_rgb(lv_obj_t * picker, uint32_t rgb) {
    picker_set_rgb(lv_obj_get_user_data(picker), rgb);
}

uint32_t gui_color_picker_get_rgb(lv_obj_t * picker) {
    color_picker_t * st = lv_obj_get_user_data(picker);
    return st->rgb;
}

void gui_color_picker_set_info(lv_obj_t * picker, const char * text) {
    color_picker_t * st = lv_obj_get_user_data(picker);
    if (text) lv_label_set_text(st->info_label, text);
    if (text) lv_obj_remove_flag(st->info_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(st->info_label, LV_OBJ_FLAG_HIDDEN);
}

void gui_color_picker_set_active(lv_obj_t * picker, bool active) {
    color_picker_t * st = lv_obj_get_user_data(picker);
    st->active = active;
    lv_obj_set_style_opa(st->card, active ? LV_OPA_COVER : LV_OPA_50, 0);
    picker_refresh(st);
}
