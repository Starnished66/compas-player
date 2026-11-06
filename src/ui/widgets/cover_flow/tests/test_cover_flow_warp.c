#include "../cover_flow_warp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE 280
#define BG 0x1082

static const cover_flow_tilt_params_t params = { 22, 80, 75, 35, 35, 40 };

/* A face out_h rows tall whose first FACE rows are the artwork. */
static uint16_t * make_face(const cover_flow_tilt_t * t) {
    uint16_t * face = calloc((size_t) FACE * (size_t) t->out_h, 2);
    assert(face);
    for (int i = 0; i < FACE * FACE; i++) face[i] = (uint16_t) (i * 2654435761u >> 16);
    return face;
}

/* The same warp into a separate buffer, from a copy of the artwork alone. */
static uint16_t * reference(const cover_flow_tilt_t * t, const uint16_t * face, int step, bool near_left) {
    uint16_t * dst = malloc(cover_flow_tilt_bytes(t, step));
    assert(dst);
    cover_flow_tilt_warp(t, step, near_left, face, dst, BG);
    return dst;
}

/* Step 0 is built in place: the warp is the face, with the reflection written under it. */
static void check_flat_alias(cf_warps_t * ws) {
    cf_content_t c = { 0 };
    c.face = make_face(ws->tilt);
    uint16_t * art = malloc(FACE * FACE * 2);
    memcpy(art, c.face, FACE * FACE * 2);
    uint16_t * want = reference(ws->tilt, art, 0, true);
    assert(cf_warp_build(ws, &c.warp[0], 0, 0, c.face));
    assert(c.warp[0].buf == c.face);
    assert(memcmp(c.face, want, cover_flow_tilt_bytes(ws->tilt, 0)) == 0);
    cf_warp_free(&c.warp[0], 0); /* lets go of the face, never frees it */
    assert(!c.warp[0].buf && c.face);
    assert(c.face[0] == want[0]);
    assert(cf_warp_build(ws, &c.warp[0], 0, 0, c.face)); /* and builds again */
    cf_content_clear(&c, 7);
    assert(!c.face && !c.warp[0].buf && c.item == 7);
    free(art);
    free(want);
}

/* A tilted step owns its buffer: built, freed, built again, matching the reference. */
static void check_step(cf_warps_t * ws) {
    cf_content_t c = { 0 };
    c.face = make_face(ws->tilt);
    uint16_t * want = reference(ws->tilt, c.face, 3, true);
    for (int round = 0; round < 2; round++) {
        assert(cf_warp_build(ws, &c.warp[3], 3, -1, c.face) && c.warp[3].side == -1);
        assert(memcmp(c.warp[3].buf, want, cover_flow_tilt_bytes(ws->tilt, 3)) == 0);
        cf_warp_free(&c.warp[3], 3);
        assert(!c.warp[3].buf);
    }
    cf_content_clear(&c, 0);
    free(want);
}

static void check_intervals(cf_warps_t * ws) {
    uint16_t * face = make_face(ws->tilt);
    for (int step = 0; step <= COVER_FLOW_TILT_STEPS; step++) {
        cf_warp_t w = {0};
        int width = ws->tilt->steps[step].width;
        for (int side = -1; side <= 1; side += 2) {
            int direction = step ? side : 0;
            uint16_t * want = reference(ws->tilt, face, step, side < 0);
            int left = width / 3, right = width * 2 / 3;
            int old_left = w.left, old_right = w.right;
            int n = cf_warp_require(ws, &w, step, direction, face, left, right);
            assert(n == ((!step && old_right > old_left) ? 0 : right - left));
            assert(cf_warp_require(ws, &w, step, direction, face, left, right) == 0);
            int missing = width - (w.right - w.left);
            assert(cf_warp_require(ws, &w, step, direction, face, 0, width) == missing);
            assert(w.left == 0 && w.right == width);
            assert(memcmp(w.buf, want, cover_flow_tilt_bytes(ws->tilt, step)) == 0);
            free(want);
        }
        cf_warp_free(&w, step);
        assert(w.left == 0 && w.right == 0);
    }
    free(face);
}

int main(void) {
    cf_warps_t ws = { .tilt = cover_flow_tilt_create(FACE, &params), .bg = BG };
    assert(ws.tilt);
    check_flat_alias(&ws);
    check_step(&ws);
    check_intervals(&ws);
    cover_flow_tilt_destroy(ws.tilt);
    puts("cover_flow_warp: PASS");
    return 0;
}
