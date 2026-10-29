#pragma once
/* Style setters that touch LVGL only when the value actually changes.
 *
 * Layout properties (padding, position, alignment, size) always mark the layout
 * dirty and invalidate the object, even when the value is the same. The list
 * decorators run again for every row whenever a thumbnail lands, so setting an
 * unchanged value turned one landing into dozens of invalidations and a
 * relayout of every row. Each helper compares against the object's own local
 * style value, which is exactly what the setter would overwrite, so skipping
 * a same-value call can never change what is drawn. */
#include "lvgl.h"
#include <string.h>

static inline bool lv_idem_local_is(lv_obj_t * obj, lv_style_prop_t prop, int32_t value) {
    lv_style_value_t v;
    return lv_obj_get_local_style_prop(obj, prop, &v, LV_PART_MAIN) == LV_STYLE_RES_FOUND && v.num == value;
}

static inline void lv_idem_set_pad_left(lv_obj_t * obj, int32_t value) {
    if (!lv_idem_local_is(obj, LV_STYLE_PAD_LEFT, value)) lv_obj_set_style_pad_left(obj, value, 0);
}

static inline void lv_idem_set_pad_right(lv_obj_t * obj, int32_t value) {
    if (!lv_idem_local_is(obj, LV_STYLE_PAD_RIGHT, value)) lv_obj_set_style_pad_right(obj, value, 0);
}

static inline void lv_idem_align(lv_obj_t * obj, lv_align_t align, int32_t x, int32_t y) {
    if (lv_idem_local_is(obj, LV_STYLE_ALIGN, align) && lv_idem_local_is(obj, LV_STYLE_X, x) &&
        lv_idem_local_is(obj, LV_STYLE_Y, y)) return;
    lv_obj_align(obj, align, x, y);
}

static inline void lv_idem_set_size(lv_obj_t * obj, int32_t w, int32_t h) {
    if (lv_idem_local_is(obj, LV_STYLE_WIDTH, w) && lv_idem_local_is(obj, LV_STYLE_HEIGHT, h)) return;
    lv_obj_set_size(obj, w, h);
}

/* lv_image_set_src() invalidates twice and re-measures the image even for the
 * same picture. File sources are compared by path, because LVGL keeps its own
 * copy of the string. */
static inline void lv_idem_image_set_src(lv_obj_t * image, const void * src) {
    const void * current = lv_image_get_src(image);
    if (current == src) return;
    if (current && src && lv_image_src_get_type(current) == LV_IMAGE_SRC_FILE &&
        lv_image_src_get_type(src) == LV_IMAGE_SRC_FILE && !strcmp((const char *) current, (const char *) src))
        return;
    lv_image_set_src(image, src);
}

static inline void lv_idem_image_set_scale(lv_obj_t * image, uint32_t scale) {
    if (lv_image_get_scale(image) != (int32_t) scale) lv_image_set_scale(image, scale);
}

static inline void lv_idem_set_hidden(lv_obj_t * obj, bool hidden) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
