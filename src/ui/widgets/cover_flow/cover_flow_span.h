#pragma once
/* Pure geometry of the cover row: where each cover sits and which part of it is
 * left uncovered by the covers nearer the centre. No LVGL, so it builds and tests on the host. */
#include "cover_flow_tilt.h"
#include <stdint.h>

#define COVER_FLOW_ONE 65536 /* one item in 16.16 */

typedef struct {
    int32_t from, to; /* columns [from, to); empty when to <= from */
} cover_flow_span_t;

/* Tilt step for a cover `ad` items (16.16, >= 0) from the centre. */
int cover_flow_step_for(int32_t ad);
/* Distance of its centre from the row's centre, in pixels. `off1` is the first
 * tilted cover's, `pitch` the spacing of those beyond it. */
int32_t cover_flow_offset_for(int32_t ad, int32_t off1, int32_t pitch);

/* Visible columns of each of `n` covers, left to right, `centre` being the middle one. `ext`
 * holds their full extents (empty for an absent cover). Extents grow outward from the centre,
 * so what the nearer covers hide is one run; each cover keeps the part outside it on its own
 * side and no column is drawn twice. A fully covered cover gets an empty span. */
void cover_flow_visible(const cover_flow_span_t * ext, int n, int centre, cover_flow_span_t * vis);
