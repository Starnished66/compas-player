#include "gui_accent.h"
#include "../settings/gui_settings.h"
#include "../../core/persistence/settings.h"
#include "gui_theme.h"
#include "gui_color_picker.h"
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
 * "Match album art" lives on the options screen; this one is the shared color
 * picker. Dragging previews locally; the accent is applied app wide (styles,
 * decoded play/drawer art, settings file) once, on release. */
static lv_obj_t * accent_dynamic_switch;
static lv_obj_t * accent_picker;

/* Mirrors the live accent state into the screen. */
static void accent_screen_sync(void) {
    if (!accent_picker) return;
    bool dynamic = current_settings.accent_dynamic;
    uint32_t cover = 0;
    bool has_cover = gui_theme_cover_accent(&cover);
    const char * source = !dynamic ? TR("Custom color")
                        : has_cover ? TR("From album art")
                        : TR("From album art (no cover, using custom)");
    gui_color_picker_set_rgb(accent_picker, gui_theme_accent_rgb());
    gui_color_picker_set_info(accent_picker, source);
    gui_color_picker_set_active(accent_picker, !dynamic);

    if (accent_dynamic_switch) {
        if (dynamic) lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(accent_dynamic_switch, LV_STATE_CHECKED);
    }
}

static void accent_picker_change_cb(uint32_t rgb, bool final, void * user_data) {
    (void) user_data;
    if (final) gui_theme_apply_accent(rgb);
    else gui_color_picker_set_info(accent_picker, TR("Custom color"));
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
    lv_obj_t * dyn = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(dyn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dyn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dyn, BOARD_SCALE_PX(12), 0);
    lv_obj_t * dyn_text = gui_plain_obj_create(dyn);
    lv_obj_set_flex_grow(dyn_text, 1);
    lv_obj_set_height(dyn_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dyn_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dyn_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(dyn_text, TR("Match album art"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * dyn_hint = gui_card_text(dyn_text, TR("Takes its color from the cover of the playing track"),
                                      GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(dyn_hint, lv_pct(100));
    lv_label_set_long_mode(dyn_hint, LV_LABEL_LONG_WRAP);
    accent_dynamic_switch = lv_switch_create(dyn);
    lv_obj_add_style(accent_dynamic_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(accent_dynamic_switch, accent_dynamic_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Built-in splash tint is an independent choice from album-art matching.
     * Custom startup images keep their original colors. */
    lv_obj_t * splash_tint = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(splash_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(splash_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(splash_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * splash_tint_text = gui_plain_obj_create(splash_tint);
    lv_obj_set_flex_grow(splash_tint_text, 1);
    lv_obj_set_height(splash_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(splash_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(splash_tint_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(splash_tint_text, TR("Tint startup image"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * splash_tint_hint = gui_card_text(splash_tint_text,
        TR("Uses the accent color for the default splash."), GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(splash_tint_hint, lv_pct(100));
    lv_label_set_long_mode(splash_tint_hint, LV_LABEL_LONG_WRAP);
    lv_obj_t * startup_tint_switch = lv_switch_create(splash_tint);
    lv_obj_add_style(startup_tint_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_tint_switch, accent_startup_tint_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (current_settings.startup_splash_tint)
        lv_obj_add_state(startup_tint_switch, LV_STATE_CHECKED);

    /* Breeze title-bar icons follow the text color unless tinted here. */
    lv_obj_t * titlebar_tint = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(titlebar_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlebar_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titlebar_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * titlebar_tint_text = gui_plain_obj_create(titlebar_tint);
    lv_obj_set_flex_grow(titlebar_tint_text, 1);
    lv_obj_set_height(titlebar_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlebar_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(titlebar_tint_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(titlebar_tint_text, TR("Tint title bar icons"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * titlebar_tint_hint = gui_card_text(titlebar_tint_text,
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

    accent_picker = gui_color_picker_create(body, card_w, current_settings.accent_color, accent_palette,
                                            ACCENT_PALETTE_COUNT, accent_picker_change_cb, NULL);

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
    accent_picker = NULL; /* accent_screen_sync() is a no-op until the screen is rebuilt */
}

void gui_accent_sync(void) {
    accent_screen_sync();
}

