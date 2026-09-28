#ifndef IMAGE_THUMB_H
#define IMAGE_THUMB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "artwork_coordinator.h"

/* Scales a JPEG, PNG or BMP (the formats cover_decode.c reads) to fit inside
 * max_w x max_h, keeping its aspect ratio (never cropped, never enlarged),
 * and writes it as an LVGL binary image (RGB565, lv_image_header_t followed
 * by the pixels) that lv_image can draw from a file with no further decode.
 * dest_path is replaced atomically (written to dest_path.tmp, then renamed).
 * Decoding goes through the shared artwork coordinator at prio.
 *
 * Returns false and a short reason ("unsupported image", "could not decode
 * image", "could not write image", "cancelled", or "busy" when the decoder
 * or its memory was taken by other artwork and a later try may work) on
 * failure; *reason may be NULL. Nothing is left at dest_path on failure. */
bool image_thumb_write_bin(const uint8_t * data, size_t size, int max_w, int max_h,
                           const char * dest_path, artwork_priority_t prio,
                           artwork_cancel_fn cancel_cb, void * cancel_user,
                           const char ** reason);

/* Native size of a JPEG, PNG or BMP from its header, without decoding. */
bool image_thumb_native_size(const uint8_t * data, size_t size, int * out_w, int * out_h);

/* Largest size with the image's aspect ratio that fits max_w x max_h,
 * no larger than the native size, at least 1x1. */
void image_thumb_fit(int native_w, int native_h, int max_w, int max_h, int * out_w, int * out_h);

#endif /* IMAGE_THUMB_H */
