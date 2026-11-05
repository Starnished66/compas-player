/* The A-Z strip: one row of letters under the labels. Pressing or dragging on
 * it sets the carousel to that letter's item at once and shows the letter in a
 * bubble; releasing lets the covers load. */
#include "cover_flow_internal.h"
#include "../../appearance/gui_theme.h"
#include "../../library/library_az_index.h"
#include "../../shell/gui_shell.h"
#include "../builders/screen_builders.h"

/* 27 letters share one row, so they are a size smaller than the library's A-Z column. */
#if defined(BOARD_TEMPOTEC_V1) || defined(BOARD_R3II_2025)
#define CF_AZ_FONT (&lv_font_montserrat_12)
#else
#define CF_AZ_FONT (&lv_font_montserrat_16)
#endif
#define CF_AZ_PAD BOARD_SCALE_PX(14) /* room above and below the letters: a finger-sized strip */

int32_t cf_az_height(void) {
    return lv_font_get_line_height(CF_AZ_FONT) + CF_AZ_PAD;
}

static char letter_char(int i) {
    return i < 26 ? (char) ('A' + i) : '#';
}

static void style_letter(const cf_az_t * az, int i) {
    lv_obj_t * l = az->letter[i];
    lv_obj_remove_style(l, &style_theme_text_primary, 0);
    lv_obj_remove_style(l, &style_theme_text_muted, 0);
    lv_obj_add_style(l, az->has[i] ? &style_theme_text_primary : &style_theme_text_muted, 0);
}

void cf_az_content(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    az->scrubbing = false;
    az->shown = -1;
    if (!cf->az_on || !az->touch) return;
    lv_obj_add_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
    cover_flow_az_build(cf->items, cf->count, az->jump, az->has);
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++) style_letter(az, i);
}

/* The letter under x, clamped to the row. */
static int letter_at(const cf_az_t * az, int32_t x) {
    int i = (x - az->left) / az->cell;
    return i < 0 ? 0 : i >= COVER_FLOW_AZ_LETTERS ? COVER_FLOW_AZ_LETTERS - 1 : i;
}

static void show_letter(cover_flow_t * cf, int i) {
    cf_az_t * az = &cf->az;
    if (i == az->shown) return;
    az->shown = i;
    lv_label_set_text_fmt(az->bubble_label, "%c", letter_char(i));
    lv_obj_remove_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
    if (az->jump[i] >= 0) cf_jump_to(az->jump[i]);
}

static void end_scrub(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    if (!az->scrubbing) return;
    az->scrubbing = false;
    az->shown = -1;
    lv_obj_add_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
    cf_pump_kick();
}

static void strip_cb(lv_event_t * e) {
    cover_flow_t * cf = cf_instance;
    if (!cf || !cf->az_on) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { end_scrub(cf); return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_point_t p = { 0, 0 };
    lv_indev_t * indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        cf_input_stop(); /* a fling in flight ends here */
        cf->az.scrubbing = true;
    }
    if (cf->az.scrubbing) show_letter(cf, letter_at(&cf->az, p.x));
}

static void make_letters(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    int32_t row = cf->g.screen_w - 2 * GUI_TEXT_INSET;
    az->left = GUI_TEXT_INSET;
    az->cell = row / COVER_FLOW_AZ_LETTERS;
    az->left += (row - az->cell * COVER_FLOW_AZ_LETTERS) / 2;
    int32_t y = cf->g.strip_y + (cf->g.strip_h - lv_font_get_line_height(CF_AZ_FONT)) / 2;
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++) {
        lv_obj_t * l = lv_label_create(cf->screen);
        lv_obj_set_style_text_font(l, CF_AZ_FONT, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, az->cell);
        lv_label_set_text_fmt(l, "%c", letter_char(i));
        lv_obj_set_pos(l, az->left + i * az->cell, y);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        az->letter[i] = l;
    }
}

static void build(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    /* The full width, so A is reachable; the back swipe keeps the left edge everywhere above. */
    az->touch = lv_obj_create(cf->screen);
    lv_obj_remove_style_all(az->touch);
    lv_obj_set_pos(az->touch, 0, cf->g.strip_y);
    lv_obj_set_size(az->touch, cf->g.screen_w, cf->g.strip_h);
    lv_obj_remove_flag(az->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(az->touch, LV_OBJ_FLAG_CLICKABLE);
    register_swipe_dead_zone(az->touch);
    lv_obj_add_event_cb(az->touch, strip_cb, LV_EVENT_ALL, NULL);
    make_letters(cf);

    az->bubble = library_az_index_bubble_create(cf->screen, &az->bubble_label);
}

void cf_az_destroy(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    if (!az->touch) return;
    unregister_swipe_dead_zone(az->touch);
    lv_obj_delete(az->touch);
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++) lv_obj_delete(az->letter[i]);
    lv_obj_delete(az->bubble);
    *az = (cf_az_t) { .shown = -1 };
}

void cf_az_sync(cover_flow_t * cf) {
    if (cf->az_on && !cf->az.touch) build(cf);
    else if (!cf->az_on) cf_az_destroy(cf);
}
