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

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_COVER_FADE_H */
