#include "gui_accent.h"
#include "../settings/gui_settings.h"
#include "../../core/persistence/settings.h"
#include "gui_theme.h"
#include "../widgets/builders/screen_builders.h"
#include "../shell/gui_navigation.h"
#include "../shell/gui_shell.h"
#include "../assets/assets.h"
#include "../text/i18n/i18n.h"
#include <stdio.h>
#include <stdint.h>

static lv_obj_t * accent_color_screen;
static lv_obj_t * accent_options_screen;

/* ---- Accent Color ----
 * A live preview, "Match album art", a saturation/brightness square with a
 * hue bar, and the preset swatches. The square is the hue as a flat color
 * under a white-to-clear and a clear-to-black gradient, so changing hue only
 * recolors one object. Dragging previews locally; the accent is applied app
 * wide (styles, decoded play/drawer art, settings file) once, on release. */
#define ACCENT_CARD_PAD BOARD_SCALE_PX(16)
#define ACCENT_SV_HEIGHT BOARD_SCALE_PX(200)
#define ACCENT_HUE_TRACK_HEIGHT BOARD_SCALE_PX(36)
#define ACCENT_HUE_BAR_HEIGHT BOARD_SCALE_PX(20)
#define ACCENT_CURSOR_SIZE BOARD_SCALE_PX(28)
#define ACCENT_PRESET_SIZE BOARD_SCALE_PX(44)

static lv_obj_t * accent_preview_swatch;
static lv_obj_t * accent_hex_label;
static lv_obj_t * accent_source_label;
static lv_obj_t * accent_dynamic_switch;
static lv_obj_t * accent_picker_card;
static lv_obj_t * accent_sv_area;
static lv_obj_t * accent_sv_cursor;
static lv_obj_t * accent_hue_track;
static lv_obj_t * accent_hue_knob;
static lv_obj_t * accent_presets[ACCENT_PALETTE_COUNT];
static int32_t accent_sv_width;
static lv_grad_dsc_t accent_sat_grad;
static lv_grad_dsc_t accent_val_grad;
static lv_grad_dsc_t accent_hue_grad;
static lv_color_hsv_t accent_pick = { 207, 86, 95 };
static bool accent_committing;

static uint32_t accent_rgb_of(lv_color_t c) {
    return ((uint32_t) c.red << 16) | ((uint32_t) c.green << 8) | c.blue;
}

static void accent_show_color(uint32_t rgb, const char * source) {
    if (!accent_preview_swatch) return;
    char hex[16];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned int) (rgb & 0xFFFFFF));
    lv_obj_set_style_bg_color(accent_preview_swatch, lv_color_hex(rgb), 0);
    lv_label_set_text(accent_hex_label, hex);
    lv_label_set_text(accent_source_label, source);
}

static void accent_place_picker(void) {
    if (!accent_sv_area) return;
    lv_obj_set_style_bg_color(accent_sv_area, lv_color_hsv_to_rgb(accent_pick.h, 100, 100), 0);
    int32_t x = (int32_t) accent_pick.s * (accent_sv_width - 1) / 100;
    int32_t y = (int32_t) (100 - accent_pick.v) * (ACCENT_SV_HEIGHT - 1) / 100;
    lv_obj_set_pos(accent_sv_cursor, x - ACCENT_CURSOR_SIZE / 2, y - ACCENT_CURSOR_SIZE / 2);
    lv_obj_set_style_bg_color(accent_sv_cursor, lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v), 0);
    int32_t hx = (int32_t) accent_pick.h * (accent_sv_width - 1) / 359;
    lv_obj_set_pos(accent_hue_knob, hx - ACCENT_CURSOR_SIZE / 2, (ACCENT_HUE_TRACK_HEIGHT - ACCENT_CURSOR_SIZE) / 2);
    lv_obj_set_style_bg_color(accent_hue_knob, lv_color_hsv_to_rgb(accent_pick.h, 100, 100), 0);
}

/* Mirrors the live accent state into the screen. The picker follows the
 * saved color, except right after its own release, where re-deriving HSV
 * from the rounded RGB would nudge the cursor. */
static void accent_screen_sync(void) {
    if (!accent_preview_swatch) return;
    bool dynamic = current_settings.accent_dynamic;
    uint32_t cover = 0;
    bool has_cover = gui_theme_cover_accent(&cover);
    const char * source = !dynamic ? TR("Custom color")
                        : has_cover ? TR("From album art")
                        : TR("From album art (no cover, using custom)");
    accent_show_color(gui_theme_accent_rgb(), source);

    if (accent_dynamic_switch) {
        if (dynamic) lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(accent_dynamic_switch, LV_STATE_CHECKED);
    }
    if (accent_picker_card) lv_obj_set_style_opa(accent_picker_card, dynamic ? LV_OPA_50 : LV_OPA_COVER, 0);

    for (int i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        bool selected = !dynamic && accent_palette[i] == current_settings.accent_color;
        lv_obj_set_style_border_width(accent_presets[i], selected ? BOARD_SCALE_PX(4) : 0, 0);
    }

    if (!accent_committing) {
        lv_color_t c = lv_color_hex(current_settings.accent_color);
        accent_pick = lv_color_rgb_to_hsv(c.red, c.green, c.blue);
        accent_place_picker();
    }
}

static void accent_commit_pick(void) {
    accent_committing = true;
    gui_theme_apply_accent(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)));
    accent_committing = false;
}

static int32_t accent_touch_offset(lv_obj_t * obj, bool vertical, int32_t span) {
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

static void accent_sv_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = accent_touch_offset(accent_sv_area, false, accent_sv_width);
        int32_t y = accent_touch_offset(accent_sv_area, true, ACCENT_SV_HEIGHT);
        accent_pick.s = (uint8_t) (x * 100 / (accent_sv_width - 1));
        accent_pick.v = (uint8_t) (100 - y * 100 / (ACCENT_SV_HEIGHT - 1));
        accent_place_picker();
        accent_show_color(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)),
                          TR("Custom color"));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        accent_commit_pick();
    }
}

static void accent_hue_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int32_t x = accent_touch_offset(accent_hue_track, false, accent_sv_width);
        accent_pick.h = (uint16_t) (x * 359 / (accent_sv_width - 1));
        /* A gray pick has no visible hue; lift it so the drag shows color. */
        if (accent_pick.s < 20) accent_pick.s = 80;
        if (accent_pick.v < 20) accent_pick.v = 90;
        accent_place_picker();
        accent_show_color(accent_rgb_of(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, accent_pick.v)),
                          TR("Custom color"));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        accent_commit_pick();
    }
}

static void accent_preset_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_theme_apply_accent((uint32_t) (uintptr_t) lv_event_get_user_data(e));
}

static void accent_dynamic_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_theme_set_accent_dynamic(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void accent_startup_tint_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_splash_tint = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

static void accent_titlebar_tint_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_theme_set_titlebar_tint(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static lv_obj_t * accent_card(lv_obj_t * parent, int32_t width) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_all(card, ACCENT_CARD_PAD, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t * accent_text(lv_obj_t * parent, const char * text, gui_font_role_t role, bool muted) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, muted ? &style_theme_text_muted : &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    return label;
}

/* Plain layer or cursor: never takes touches, so they reach the area below. */
static lv_obj_t * accent_plain_obj(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* A drag surface: keeps the drag (no body scroll, no app swipe-back). */
static void accent_make_drag_surface(lv_obj_t * obj, lv_event_cb_t cb) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                       LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_ALL, NULL);
}

static lv_obj_t * accent_cursor(lv_obj_t * parent) {
    lv_obj_t * c = accent_plain_obj(parent);
    lv_obj_set_size(c, ACCENT_CURSOR_SIZE, ACCENT_CURSOR_SIZE);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_outline_color(c, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_outline_width(c, 1, 0);
    return c;
}

/* The accent's on/off choices, opened from the Accent Color title bar so the
 * color itself keeps the main screen. */
static lv_obj_t * build_accent_options_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Accent Options"), generic_back_cb, NULL, NULL);

    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t card_w = BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(20);

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    /* Match album art */
    lv_obj_t * dyn = accent_card(body, card_w);
    lv_obj_set_flex_flow(dyn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dyn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dyn, BOARD_SCALE_PX(12), 0);
    lv_obj_t * dyn_text = accent_plain_obj(dyn);
    lv_obj_set_flex_grow(dyn_text, 1);
    lv_obj_set_height(dyn_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dyn_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dyn_text, BOARD_SCALE_PX(4), 0);
    accent_text(dyn_text, TR("Match album art"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * dyn_hint = accent_text(dyn_text, TR("Takes its color from the cover of the playing track"),
                                      GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(dyn_hint, lv_pct(100));
    lv_label_set_long_mode(dyn_hint, LV_LABEL_LONG_WRAP);
    accent_dynamic_switch = lv_switch_create(dyn);
    lv_obj_add_style(accent_dynamic_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(accent_dynamic_switch, accent_dynamic_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Built-in splash tint is an independent choice from album-art matching.
     * Custom startup images keep their original colors. */
    lv_obj_t * splash_tint = accent_card(body, card_w);
    lv_obj_set_flex_flow(splash_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(splash_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(splash_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * splash_tint_text = accent_plain_obj(splash_tint);
    lv_obj_set_flex_grow(splash_tint_text, 1);
    lv_obj_set_height(splash_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(splash_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(splash_tint_text, BOARD_SCALE_PX(4), 0);
    accent_text(splash_tint_text, TR("Tint startup image"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * splash_tint_hint = accent_text(splash_tint_text,
        TR("Uses the accent color for the default splash."), GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(splash_tint_hint, lv_pct(100));
    lv_label_set_long_mode(splash_tint_hint, LV_LABEL_LONG_WRAP);
    lv_obj_t * startup_tint_switch = lv_switch_create(splash_tint);
    lv_obj_add_style(startup_tint_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_tint_switch, accent_startup_tint_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (current_settings.startup_splash_tint)
        lv_obj_add_state(startup_tint_switch, LV_STATE_CHECKED);

    /* Breeze title-bar icons follow the text color unless tinted here. */
    lv_obj_t * titlebar_tint = accent_card(body, card_w);
    lv_obj_set_flex_flow(titlebar_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlebar_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titlebar_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * titlebar_tint_text = accent_plain_obj(titlebar_tint);
    lv_obj_set_flex_grow(titlebar_tint_text, 1);
    lv_obj_set_height(titlebar_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlebar_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(titlebar_tint_text, BOARD_SCALE_PX(4), 0);
    accent_text(titlebar_tint_text, TR("Tint title bar icons"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * titlebar_tint_hint = accent_text(titlebar_tint_text,
        TR("Uses the accent color for the icons next to screen titles."), GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(titlebar_tint_hint, lv_pct(100));
    lv_label_set_long_mode(titlebar_tint_hint, LV_LABEL_LONG_WRAP);
    lv_obj_t * titlebar_tint_switch = lv_switch_create(titlebar_tint);
    lv_obj_add_style(titlebar_tint_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(titlebar_tint_switch, accent_titlebar_tint_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (current_settings.titlebar_icon_tint)
        lv_obj_add_state(titlebar_tint_switch, LV_STATE_CHECKED);

    accent_screen_sync();
    finalize_screen_navigation(scr);
    return scr;
}

static void accent_options_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !accent_options_screen) return;
    nav_push(accent_options_screen);
}

static lv_obj_t * build_accent_color_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Accent Color"), generic_back_cb, asset_path("settings/playback.png"),
                        accent_options_open_cb);

    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t card_w = BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(20);
    accent_sv_width = card_w - 2 * ACCENT_CARD_PAD;

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    /* Preview: the accent as a large swatch with its hex value and source.
     * No sample switch here: a switch that cannot be turned off read as a
     * broken setting. */
    lv_obj_t * preview = accent_card(body, card_w);
    lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview, BOARD_SCALE_PX(16), 0);
    accent_preview_swatch = accent_plain_obj(preview);
    lv_obj_set_size(accent_preview_swatch, BOARD_SCALE_PX(76), BOARD_SCALE_PX(76));
    lv_obj_set_style_radius(accent_preview_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(accent_preview_swatch, LV_OPA_COVER, 0);
    lv_obj_t * info = accent_plain_obj(preview);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(6), 0);
    accent_hex_label = accent_text(info, "", GUI_FONT_ROLE_TITLE, false);
    accent_source_label = accent_text(info, "", GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(accent_source_label, lv_pct(100));
    lv_label_set_long_mode(accent_source_label, LV_LABEL_LONG_WRAP);

    /* Custom color picker */
    accent_picker_card = accent_card(body, card_w);
    lv_obj_set_flex_flow(accent_picker_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(accent_picker_card, BOARD_SCALE_PX(14), 0);
    accent_text(accent_picker_card, TR("Custom color"), GUI_FONT_ROLE_ROW, false);

    accent_sv_area = lv_obj_create(accent_picker_card);
    lv_obj_remove_style_all(accent_sv_area);
    lv_obj_set_size(accent_sv_area, accent_sv_width, ACCENT_SV_HEIGHT);
    lv_obj_set_style_radius(accent_sv_area, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_bg_opa(accent_sv_area, LV_OPA_COVER, 0);
    accent_make_drag_surface(accent_sv_area, accent_sv_event_cb);
    static const lv_opa_t fade_out[2] = { LV_OPA_COVER, LV_OPA_TRANSP };
    static const lv_opa_t fade_in[2] = { LV_OPA_TRANSP, LV_OPA_COVER };
    lv_color_t whites[2] = { lv_color_white(), lv_color_white() };
    lv_color_t blacks[2] = { lv_color_black(), lv_color_black() };
    lv_grad_init_stops(&accent_sat_grad, whites, fade_out, NULL, 2);
    lv_grad_horizontal_init(&accent_sat_grad);
    lv_grad_init_stops(&accent_val_grad, blacks, fade_in, NULL, 2);
    lv_grad_vertical_init(&accent_val_grad);
    lv_grad_dsc_t * layers[2] = { &accent_sat_grad, &accent_val_grad };
    for (int i = 0; i < 2; i++) {
        lv_obj_t * layer = accent_plain_obj(accent_sv_area);
        lv_obj_set_size(layer, accent_sv_width, ACCENT_SV_HEIGHT);
        lv_obj_set_style_radius(layer, BOARD_SCALE_PX(12), 0);
        lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad(layer, layers[i], 0);
    }
    accent_sv_cursor = accent_cursor(accent_sv_area);

    accent_hue_track = lv_obj_create(accent_picker_card);
    lv_obj_remove_style_all(accent_hue_track);
    lv_obj_set_size(accent_hue_track, accent_sv_width, ACCENT_HUE_TRACK_HEIGHT);
    accent_make_drag_surface(accent_hue_track, accent_hue_event_cb);
    lv_color_t hues[7];
    for (int i = 0; i < 7; i++) hues[i] = lv_color_hsv_to_rgb((uint16_t) (i * 60 % 360), 100, 100);
    lv_grad_init_stops(&accent_hue_grad, hues, NULL, NULL, 7);
    lv_grad_horizontal_init(&accent_hue_grad);
    lv_obj_t * hue_bar = accent_plain_obj(accent_hue_track);
    lv_obj_set_size(hue_bar, accent_sv_width, ACCENT_HUE_BAR_HEIGHT);
    lv_obj_align(hue_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(hue_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hue_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(hue_bar, &accent_hue_grad, 0);
    accent_hue_knob = accent_cursor(accent_hue_track);

    lv_obj_remove_flag(accent_picker_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(accent_picker_card);

    /* Presets */
    lv_obj_t * presets = accent_card(body, card_w);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(presets, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(presets, BOARD_SCALE_PX(14), 0);
    lv_obj_t * presets_title = accent_text(presets, TR("Presets"), GUI_FONT_ROLE_ROW, false);
    lv_obj_set_width(presets_title, lv_pct(100));
    for (int i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_t * sw = lv_obj_create(presets);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, ACCENT_PRESET_SIZE, ACCENT_PRESET_SIZE);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(accent_palette[i]), 0);
        lv_obj_set_style_border_color(sw, lv_color_white(), 0);
        lv_obj_set_style_outline_color(sw, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(sw, LV_OPA_30, 0);
        lv_obj_set_style_outline_width(sw, 1, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(sw, BOARD_SCALE_PX(6));
        lv_obj_add_event_cb(sw, accent_preset_event_cb, LV_EVENT_CLICKED, (void *) (uintptr_t) accent_palette[i]);
        accent_presets[i] = sw;
    }

    accent_screen_sync();
    finalize_screen_navigation(scr);
    return scr;
}

void gui_accent_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(accent_color_screen);
}

void gui_accent_build_screens(void) {
    accent_options_screen = build_accent_options_screen();
    accent_color_screen = build_accent_color_screen();
}

void gui_accent_teardown(void) {
    if (accent_color_screen) { lv_obj_delete(accent_color_screen); accent_color_screen = NULL; }
    if (accent_options_screen) { lv_obj_delete(accent_options_screen); accent_options_screen = NULL; }
    accent_dynamic_switch = NULL;
    accent_picker_card = NULL;
    accent_preview_swatch = NULL; /* accent_screen_sync() is a no-op until the screen is rebuilt */
    accent_sv_area = NULL;
}

void gui_accent_sync(void) {
    accent_screen_sync();
}

lv_obj_t * gui_settings_get_accent_screen(void) { return accent_color_screen; }
