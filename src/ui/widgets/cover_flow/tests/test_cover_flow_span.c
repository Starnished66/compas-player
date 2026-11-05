#include "../cover_flow_span.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE 280
#define SLOTS 11
#define CENTRE 5
#define ROW_W 480
#define GAP 4
#define PITCH 11
#define BG 0x1082

static const cover_flow_tilt_params_t params = { 22, 80, 75, 35, 35, 40 };

/* One slot's extent for a cover `d` items (16.16, signed) from the centre. */
static cover_flow_span_t extent(const cover_flow_tilt_t * t, int32_t d) {
    int off1 = FACE / 2 + GAP + t->steps[COVER_FLOW_TILT_STEPS].width / 2;
    int32_t ad = d < 0 ? -d : d;
    int w = t->steps[cover_flow_step_for(ad)].width;
    int32_t off = cover_flow_offset_for(ad, off1, PITCH);
    int32_t x = ROW_W / 2 + (d < 0 ? -off : off) - w / 2;
    return (cover_flow_span_t) { x, x + w };
}

/* The row as cover_flow_layout.c lays it out for a scroll position. */
static void layout(const cover_flow_tilt_t * t, int32_t pos, cover_flow_span_t * ext) {
    int base = (pos + COVER_FLOW_ONE / 2) / COVER_FLOW_ONE;
    for (int i = 0; i < SLOTS; i++) ext[i] = extent(t, (base + i - CENTRE) * COVER_FLOW_ONE - pos);
}

static void check_position(const cover_flow_tilt_t * t, int32_t pos) {
    cover_flow_span_t ext[SLOTS], vis[SLOTS];
    layout(t, pos, ext);
    cover_flow_visible(ext, SLOTS, CENTRE, vis);

    int drawn[ROW_W + 64] = { 0 }, any[ROW_W + 64] = { 0 };
    const int shift = 32; /* extents may reach a little past the row */
    for (int i = 0; i < SLOTS; i++) {
        assert(ext[i].from >= -shift && ext[i].to <= ROW_W + shift);
        assert(vis[i].from >= ext[i].from && vis[i].to <= ext[i].to); /* never grows */
        for (int x = ext[i].from; x < ext[i].to; x++) any[x + shift] = 1;
        for (int x = vis[i].from; x < vis[i].to; x++) drawn[x + shift]++;
    }
    for (int x = 0; x < ROW_W + 64; x++) {
        assert(drawn[x] == any[x]); /* every covered column is drawn, exactly once, none outside */
    }
}

/* With 11 slots the covers 6 items out are not needed: the one a step inside them always reaches
 * the row's edge first, so nothing of them shows. */
static void check_outer_hidden(const cover_flow_tilt_t * t) {
    for (int32_t ad = 5 * COVER_FLOW_ONE + COVER_FLOW_ONE / 2; ad <= 6 * COVER_FLOW_ONE + COVER_FLOW_ONE / 2; ad += COVER_FLOW_ONE / 64) {
        assert(extent(t, ad - COVER_FLOW_ONE).to >= ROW_W);
        assert(extent(t, -(ad - COVER_FLOW_ONE)).from <= 0);
    }
}

/* A slot without a face is laid out like one with a face: a warp's size and silhouette depend only on
 * its step and side (the tables), never on the pixels it was built from. */
static void check_faceless_geometry(const cover_flow_tilt_t * t) {
    uint16_t * art = malloc(FACE * FACE * 2), * glyph = malloc(FACE * FACE * 2);
    assert(art && glyph);
    for (int i = 0; i < FACE * FACE; i++) {
        art[i] = (uint16_t) (0x8410 | ((i * 2654435761u) >> 16)); /* bright in every channel */
        glyph[i] = 0xFFFF;
    }
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++)
        for (int side = 0; side < 2; side++) {
            size_t bytes = cover_flow_tilt_bytes(t, s);
            assert(bytes == (size_t) t->steps[s].width * t->out_h * 2); /* the width the extents use */
            uint16_t * a = malloc(bytes), * b = malloc(bytes);
            assert(a && b);
            cover_flow_tilt_warp(t, s, side == 0, art, a, BG);
            cover_flow_tilt_warp(t, s, side == 0, glyph, b, BG);
            int w = t->steps[s].width;
            for (int x = 0; x < w; x++) {
                const cover_flow_tilt_col_t * c = &t->steps[s].cols[x];
                int dx = side == 0 ? x : w - 1 - x;
                for (int y = 0; y < t->out_h; y++) {
                    bool cover = y >= c->top && y < c->top + c->height, outside = y < c->top || y >= c->top + c->height + c->refl;
                    assert(!cover || (a[y * w + dx] != BG && b[y * w + dx] != BG)); /* both paint the cover */
                    assert(!outside || (a[y * w + dx] == BG && b[y * w + dx] == BG));
                }
            }
            free(a); free(b);
        }
    free(art); free(glyph);
}

int main(void) {
    cover_flow_tilt_t * t = cover_flow_tilt_create(FACE, &params);
    assert(t);
    int moving = 0;
    for (int item = 0; item < 4; item++) check_position(t, (CENTRE + item) * COVER_FLOW_ONE);
    for (int32_t pos = 6 * COVER_FLOW_ONE; pos <= 8 * COVER_FLOW_ONE; pos += COVER_FLOW_ONE / 64, moving++)
        check_position(t, pos);
    check_outer_hidden(t);
    /* A hand-made split and a hidden one, the middle cover being the centre. */
    cover_flow_span_t ext[3] = { { 0, 100 }, { 40, 60 }, { 45, 55 } }, vis[3];
    cover_flow_visible(ext, 3, 1, vis);
    assert(vis[1].from == 40 && vis[1].to == 60);
    assert(vis[2].to <= vis[2].from);             /* fully behind the centre */
    assert(vis[0].from == 0 && vis[0].to == 40);  /* split in two: the left piece only */
    ext[0] = (cover_flow_span_t) { 0, 100 };
    ext[1] = (cover_flow_span_t) { 30, 150 };
    cover_flow_visible(ext, 2, 1, vis);           /* one piece: clipped */
    assert(vis[0].from == 0 && vis[0].to == 30);
    check_faceless_geometry(t);
    cover_flow_tilt_destroy(t);
    printf("cover_flow_span: PASS (%d positions)\n", moving + 4);
    return 0;
}
