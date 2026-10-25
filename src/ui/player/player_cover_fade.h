#ifndef PLAYER_COVER_FADE_H
#define PLAYER_COVER_FADE_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attach a static, spatially dithered A8 tint overlay to a cover image. The
 * mask follows the image object's bounds and is released with its LVGL child. */
bool player_cover_fade_attach(lv_obj_t * image, lv_color_t color);

/* True only while the parent is still the untouched placeholder and its
 * sole child is the exact A8 overlay created by player_cover_fade_attach. */
bool player_cover_fade_cache_eligible(lv_obj_t * parent);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_COVER_FADE_H */
