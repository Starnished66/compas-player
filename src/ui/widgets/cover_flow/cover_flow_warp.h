#pragma once
/* Warped bitmaps of a cover and who owns their pixels. No LVGL, so it builds
 * and tests on the host. */
#include "cover_flow_tilt.h"

typedef struct {
    uint16_t * buf;     /* opaque RGB565, background colour baked in; step 0 borrows the face */
    int left, right;    /* initialized destination columns [left, right) */
    int side;           /* -1 left of the centre, +1 right, 0 flat */
} cf_warp_t;

/* What a build needs. */
typedef struct {
    cover_flow_tilt_t * tilt;
    uint16_t bg;
} cf_warps_t;

/* What an item brings along: it moves between slots as the column shifts. */
typedef struct {
    int item;
    uint16_t * face;    /* decoded cover, owned; NULL while pending. out_h rows tall: step 0 is built in place */
    int failed;         /* 0 not tried, 1 failed without generating, 2 failed generating, 3 owner face failed: only cover_flow_retry_faces() retries */
    cf_warp_t warp[COVER_FLOW_TILT_STEPS + 1]; /* [0] is the flat centre cover */
} cf_content_t;

/* Builds `step` from the `src` face (out_h rows tall). Step 0 is the face itself, its reflection written below. */
bool cf_warp_build(cf_warps_t * ws, cf_warp_t * w, int step, int side, uint16_t * src);
/* Extends the initialized interval, returning the number of columns newly warped, or -1 on failure. */
int cf_warp_require(cf_warps_t * ws, cf_warp_t * w, int step, int side, uint16_t * src, int left, int right);
/* Frees the buffer. Step 0 only lets go of the face. */
void cf_warp_free(cf_warp_t * w, int step);
/* Frees what an item brought and rebinds it to `item`. */
void cf_content_clear(cf_content_t * c, int item);
