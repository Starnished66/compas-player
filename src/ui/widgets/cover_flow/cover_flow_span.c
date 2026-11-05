#include "cover_flow_span.h"

int cover_flow_step_for(int32_t ad) {
    return ad >= COVER_FLOW_ONE ? COVER_FLOW_TILT_STEPS
                                : (ad * COVER_FLOW_TILT_STEPS + COVER_FLOW_ONE / 2) / COVER_FLOW_ONE;
}

int32_t cover_flow_offset_for(int32_t ad, int32_t off1, int32_t pitch) {
    if (ad >= COVER_FLOW_ONE) return off1 + (int32_t) (((int64_t) (ad - COVER_FLOW_ONE) * pitch) / COVER_FLOW_ONE);
    return (int32_t) (((int64_t) ad * off1) / COVER_FLOW_ONE);
}

/* The part of `e` outside the covered columns [lo, hi) on its own side (-1 left of the centre, +1 right, 0 the centre). */
static cover_flow_span_t outside(cover_flow_span_t e, int side, int32_t lo, int32_t hi) {
    cover_flow_span_t v = e;
    if (side < 0 && v.to > lo) v.to = lo;
    if (side > 0 && v.from < hi) v.from = hi;
    if (v.to <= v.from) v.to = v.from = e.from; /* nothing left */
    return v;
}

static void take(const cover_flow_span_t * ext, cover_flow_span_t * vis, int i, int side, int32_t * lo, int32_t * hi) {
    vis[i] = outside(ext[i], side, *lo, *hi);
    if (ext[i].to <= ext[i].from) return; /* absent */
    if (ext[i].from < *lo) *lo = ext[i].from;
    if (ext[i].to > *hi) *hi = ext[i].to;
}

void cover_flow_visible(const cover_flow_span_t * ext, int n, int centre, cover_flow_span_t * vis) {
    int32_t lo = INT32_MAX, hi = INT32_MIN; /* extents grow outward, so the covered columns stay one run */
    take(ext, vis, centre, 0, &lo, &hi);
    for (int d = 1; d < n; d++) {
        if (centre - d >= 0) take(ext, vis, centre - d, -1, &lo, &hi);
        if (centre + d < n) take(ext, vis, centre + d, 1, &lo, &hi);
    }
}
