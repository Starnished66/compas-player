#pragma once
/* Tilt tables and the warp for cover flow covers. No LVGL objects, so it
 * builds and tests on the host.
 *
 * A cover tilts in COVER_FLOW_TILT_STEPS steps, 0 (flat) to the last (fully
 * tilted). For each step the tables hold, per destination column, the source
 * column, the top row, the height, a shade and the vertical source step, plus the
 * rows of the reflection that hangs under the column. The columns are listed
 * from the near edge, the one that faces the viewer. A warp is then a walk down
 * each column: one source read and one shade multiply per destination pixel,
 * no divisions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COVER_FLOW_TILT_STEPS 8

typedef struct {
    int far_width_pct;     /* projected width of the fully tilted cover, % of the face */
    int side_height_pct;   /* height of its near edge, % of the face */
    int far_height_pct;    /* height of its far edge, % of its near edge */
    int far_shade_pct;     /* darkening of its far edge, % */
    int reflect_pct;       /* height of the reflection, % of the cover's height */
    int reflect_alpha_pct; /* opacity of the reflection at the cover's bottom edge, % */
} cover_flow_tilt_params_t;

typedef struct {
    uint16_t src_x;       /* source column, counted from the near edge */
    int16_t top;          /* first destination row of the column */
    uint16_t height;      /* cover rows in the column */
    uint16_t shade;       /* 0..256, 256 leaves the colour alone */
    uint16_t refl;        /* reflection rows under the cover rows */
    uint32_t v_step;      /* source rows per destination row, 16.16 */
    uint32_t refl_step;   /* alpha lost per reflection row, 16.16 */
} cover_flow_tilt_col_t;

typedef struct {
    int width;                    /* destination bitmap width */
    cover_flow_tilt_col_t * cols; /* width entries, the near edge first */
} cover_flow_tilt_step_t;

typedef struct {
    int face_px;    /* source side */
    int out_h;      /* destination height: the face and its reflection */
    uint32_t alpha_fp; /* reflection strength at the cover's bottom edge, 16.16 */
    cover_flow_tilt_step_t steps[COVER_FLOW_TILT_STEPS + 1];
    void * block;
} cover_flow_tilt_t;

/* Builds the tables for square faces of `face_px`. NULL on failure. */
cover_flow_tilt_t * cover_flow_tilt_create(int face_px, const cover_flow_tilt_params_t * params);
void cover_flow_tilt_destroy(cover_flow_tilt_t * tilt);

/* Bytes of a warped bitmap of `step`: plain RGB565, width x out_h. */
size_t cover_flow_tilt_bytes(const cover_flow_tilt_t * tilt, int step);

/* Warps the face_px x face_px RGB565 `src` into `dst` (cover_flow_tilt_bytes()
 * bytes, width x out_h, stride width * 2). The bitmap is opaque: outside the
 * trapezoid it is `bg`, and the soft top edge and the fading reflection are
 * blended into `bg`, so it only suits a solid background of that colour.
 * `near_left` puts the near edge at the left (covers left of the centre); false
 * puts it at the right. The artwork is never mirrored. Step 0 may be warped in place:
 * `dst` == `src`, which then holds out_h rows. */
void cover_flow_tilt_warp(const cover_flow_tilt_t * tilt, int step, bool near_left,
                          const uint16_t * src, uint16_t * dst, uint16_t bg);

/* Same pixels, only destination columns [left, right); all other columns are untouched. */
void cover_flow_tilt_warp_columns(const cover_flow_tilt_t * tilt, int step, bool near_left,
                                  const uint16_t * src, uint16_t * dst, uint16_t bg, int left, int right);
