#include "cover_flow_warp.h"
#include <stdlib.h>
#include <string.h>

bool cf_warp_build(cf_warps_t * ws, cf_warp_t * w, int step, int side, uint16_t * src) {
    if (step == 0) w->buf = src;
    else if (!w->buf) w->buf = malloc(cover_flow_tilt_bytes(ws->tilt, step));
    if (!w->buf) return false;
    cover_flow_tilt_warp(ws->tilt, step, side < 0, src, w->buf, ws->bg);
    w->side = side;
    return true;
}

void cf_warp_free(cf_warp_t * w, int step) {
    if (step) free(w->buf);
    memset(w, 0, sizeof(*w));
}

void cf_content_clear(cf_content_t * c, int item) {
    free(c->face);
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++) cf_warp_free(&c->warp[s], s);
    memset(c, 0, sizeof(*c));
    c->item = item;
}
