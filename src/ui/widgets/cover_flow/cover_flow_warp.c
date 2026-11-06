#include "cover_flow_warp.h"
#include <stdlib.h>
#include <string.h>

/* Keep one initialized interval, filling gaps when a previously hidden slice appears. */
int cf_warp_require(cf_warps_t * ws, cf_warp_t * w, int step, int side, uint16_t * src, int left, int right) {
    if (left < 0) left = 0;
    if (right > ws->tilt->steps[step].width) right = ws->tilt->steps[step].width;
    if (left >= right) return 0;
    if (step == 0) w->buf = src;
    else if (!w->buf) w->buf = malloc(cover_flow_tilt_bytes(ws->tilt, step));
    if (!w->buf) return -1;
    if (w->side != side || w->left == w->right) w->left = w->right = left;
    int from = left < w->left ? left : w->left;
    int to = right > w->right ? right : w->right;
    int columns = (w->left - from) + (to - w->right);
    if (from < w->left)
        cover_flow_tilt_warp_columns(ws->tilt, step, side < 0, src, w->buf, ws->bg, from, w->left);
    if (to > w->right)
        cover_flow_tilt_warp_columns(ws->tilt, step, side < 0, src, w->buf, ws->bg, w->right, to);
    w->left = from; w->right = to;
    w->side = side;
    return columns;
}

bool cf_warp_build(cf_warps_t * ws, cf_warp_t * w, int step, int side, uint16_t * src) {
    return cf_warp_require(ws, w, step, side, src, 0, ws->tilt->steps[step].width) >= 0;
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
