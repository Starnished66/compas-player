#include "star_rating.h"
#include "star_rating_icons.h"
#include "../builders/screen_builders.h"
#include <stdlib.h>

extern lv_style_t style_theme_screen_bg;

typedef struct {
    int stars;
    bool compact, filled_only;
    lv_color_t lit, unlit;
    star_rating_cb_t cb;
    void * user_data;
} star_rating_state_t;

/* Compact: the star sits top-left with the count in a chip over its lower
 * right; unrated shows the outlined star alone, centred. */
static void compact_show(lv_obj_t * control, const star_rating_state_t * state) {
    lv_obj_t * star = lv_obj_get_child(control, 0);
    lv_obj_t * chip = lv_obj_get_child(control, 1);
    lv_obj_align(star, state->stars ? LV_ALIGN_TOP_LEFT : LV_ALIGN_CENTER, 0, 0);
    if (!state->stars) {
        lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_HIDDEN);
    lv_style_value_t dark = {.color = lv_color_black()};
    lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &dark);
    lv_obj_set_style_bg_color(chip, state->lit, 0);
    lv_obj_set_style_border_color(chip, dark.color, 0);
    lv_obj_t * digit = lv_obj_get_child(chip, 0);
    lv_obj_set_style_text_color(digit, dark.color, 0);
    lv_label_set_text_fmt(digit, "%d", state->stars);
}

void star_rating_set(lv_obj_t * control, int stars) {
    star_rating_state_t * state = lv_obj_get_user_data(control);
    state->stars = LV_CLAMP(0, stars, 5);
    for (int i = 0; i < (state->compact ? 1 : 5); i++) {
        lv_obj_t * image = lv_obj_get_child(control, i);
        if (state->filled_only && i >= state->stars) lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(image, i < state->stars ? &star_rating_icon_on : &star_rating_icon_off);
        lv_obj_set_style_image_recolor(image, i < state->stars ? state->lit : state->unlit, 0);
        lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    }
    if (state->compact) compact_show(control, state);
}

void star_rating_set_filled_only(lv_obj_t *control) {
    star_rating_state_t *state = lv_obj_get_user_data(control);
    state->filled_only = true;
    star_rating_set(control, state->stars);
}

void star_rating_set_colors(lv_obj_t * control, lv_color_t lit, lv_color_t unlit) {
    star_rating_state_t * state = lv_obj_get_user_data(control);
    state->lit = lit;
    state->unlit = unlit;
    star_rating_set(control, state->stars);
}

void star_rating_set_lit_color(lv_obj_t * control, lv_color_t lit) {
    star_rating_state_t * state = lv_obj_get_user_data(control);
    star_rating_set_colors(control, lit, state->unlit);
}

int star_rating_get(lv_obj_t * control) {
    star_rating_state_t * state = lv_obj_get_user_data(control);
    return state->stars;
}

static void star_clicked(lv_event_t * e) {
    lv_obj_t * image = lv_event_get_current_target(e);
    lv_obj_t * control = lv_obj_get_parent(image);
    star_rating_state_t * state = lv_obj_get_user_data(control);
    if (!state->cb) return;
    int stars = (int) (intptr_t) lv_obj_get_user_data(image);
    stars = state->compact ? -1 : (stars == state->stars ? 0 : stars);
    if (stars >= 0) star_rating_set(control, stars);
    state->cb(stars, state->user_data);
}

static void star_deleted(lv_event_t * e) {
    free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

void star_rating_set_on_change(lv_obj_t * control, star_rating_cb_t cb, void * user_data) {
    star_rating_state_t * state = lv_obj_get_user_data(control);
    state->cb = cb;
    state->user_data = user_data;
    for (uint32_t i = 0; i < lv_obj_get_child_count(control); i++) {
        lv_obj_t * child = lv_obj_get_child(control, i);
        if (cb) lv_obj_add_flag(child, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void add_star(lv_obj_t * control, int32_t px, int index) {
    lv_obj_t * image = lv_image_create(control);
    lv_obj_set_size(image, px, px);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_user_data(image, (void *) (intptr_t) index);
    lv_obj_add_event_cb(image, star_clicked, LV_EVENT_CLICKED, NULL);
}

/* The chip overhangs the star, so the control is a little larger than the star
 * and the pair stays centred in its parent. The chip has a legible minimum, so
 * on small stars it overhangs more: it covers at most a quarter of the star. */
static void add_chip(lv_obj_t * control, int32_t star_px) {
    int32_t chip_px = LV_MAX(20, star_px / 2);
    int32_t side = star_px + LV_MAX(chip_px / 6, chip_px - star_px / 2);
    lv_obj_remove_flag(control, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(control, LV_LAYOUT_NONE);
    lv_obj_set_size(control, side, side);
    lv_obj_t * chip = lv_obj_create(control);
    lv_obj_remove_style_all(chip);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(chip, chip_px, chip_px);
    lv_obj_align(chip, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, LV_MAX(2, chip_px / 12), 0);
    lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
    lv_obj_t * digit = lv_label_create(chip);
    lv_obj_set_style_text_font(digit, &app_font_16, 0);
    lv_obj_center(digit);
    lv_obj_add_event_cb(chip, star_clicked, LV_EVENT_CLICKED, NULL);
}

lv_obj_t * star_rating_create(lv_obj_t * parent, int32_t star_px, bool compact) {
    star_rating_state_t * state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->compact = compact;
    lv_style_value_t text = {.color = lv_color_hex(GUI_COLOR_PRIMARY)};
    lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text);
    state->lit = state->unlit = text.color;
    lv_obj_t * control = lv_obj_create(parent);
    lv_obj_remove_style_all(control);
    lv_obj_remove_flag(control, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(control, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(control, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(control, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(control, state);
    lv_obj_add_event_cb(control, star_deleted, LV_EVENT_DELETE, NULL);
    for (int i = 1; i <= (compact ? 1 : 5); i++) add_star(control, star_px, i);
    if (compact) add_chip(control, star_px);
    star_rating_set(control, 0);
    star_rating_set_on_change(control, NULL, NULL);
    return control;
}
