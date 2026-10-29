#ifndef LIBRARY_THUMBNAIL_PLACEHOLDER_H
#define LIBRARY_THUMBNAIL_PLACEHOLDER_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

/* The list's "no cover yet" picture, rendered once at its final on-screen size.
 *
 * The stock asset is 72x72 and a list row drew it scaled by
 * ALBUM_THUMBNAIL_PX/72 about its centre, which made LVGL run its image
 * transform for every row on every bind. This renders exactly that picture once
 * (through LVGL itself, so every pixel is the one the transform produced) into a
 * shared immutable ARGB8888 buffer drawn at LV_SCALE_NONE.
 *
 * Boards that enlarge the picture (80 px) get the asset's 72x72 box plus the
 * overhang of the scaled picture (4 px a side); `overhang` is that margin, so a
 * row can shift the image left by it and keep the picture where the transform
 * put it. Boards that shrink it (40, 53 px) get exactly ALBUM_THUMBNAIL_PX
 * square, overhang 0: the placeholder fills the square the real cover draws in. */
typedef struct {
    const lv_image_dsc_t * dsc;
    int32_t overhang;
} thumbnail_placeholder_t;

/* UI thread. Builds on first use of each source path (the theme's asset or an
 * override); the buffers are kept until shutdown, so a row still bound to an
 * earlier theme's buffer never reads freed memory. False if the asset or memory
 * is unavailable: the caller draws the old way. */
bool thumbnail_placeholder_get(thumbnail_placeholder_t * out);
/* Frees every buffer (final shutdown only: rows may still point at them). */
void thumbnail_placeholder_release(void);

#endif
