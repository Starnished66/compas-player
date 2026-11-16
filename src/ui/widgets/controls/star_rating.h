#ifndef STAR_RATING_H
#define STAR_RATING_H
#include "lvgl/lvgl.h"
#include <stdbool.h>

typedef void (*star_rating_cb_t)(int stars, void * user_data);
/* Tapping the lit count clears it; compact taps report -1. */
lv_obj_t * star_rating_create(lv_obj_t * parent, int32_t star_px, bool compact);
void star_rating_set(lv_obj_t * control, int stars);
/* Section headings show only the filled stars. */
void star_rating_set_filled_only(lv_obj_t *control);
void star_rating_set_colors(lv_obj_t * control, lv_color_t lit, lv_color_t unlit);
void star_rating_set_lit_color(lv_obj_t * control, lv_color_t lit);
int star_rating_get(lv_obj_t * control);
/* Without a callback the control only displays. */
void star_rating_set_on_change(lv_obj_t * control, star_rating_cb_t cb, void * user_data);
#endif
