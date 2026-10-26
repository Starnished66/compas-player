#ifndef COVER_CARD_PREVIEW_H
#define COVER_CARD_PREVIEW_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a small file-backed PNG to owned RGB565 pixels for a cover card.
 * On success, image points at the pre-rasterized image until it is deleted.
 * On failure, the image source and scale are left untouched so LVGL can use
 * its normal file decoder/scale path. resolved_src must be an LVGL POSIX
 * filesystem path such as "S:/path/to/preview.png". */
bool cover_card_preview_set(lv_obj_t * image, const char * resolved_src,
                           int max_w, int max_h);

#ifdef __cplusplus
}
#endif

#endif /* COVER_CARD_PREVIEW_H */
