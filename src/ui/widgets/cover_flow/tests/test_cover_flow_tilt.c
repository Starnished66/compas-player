#include "../cover_flow_tilt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE 280

static const cover_flow_tilt_params_t params = { 22, 80, 75, 35, 35, 40 };

static void check_monotonic(const cover_flow_tilt_t * t, int step) {
    const cover_flow_tilt_step_t * st = &t->steps[step];
    for (int x = 1; x < st->width; x++) {
        assert(st->cols[x].height <= st->cols[x - 1].height);
        assert(st->cols[x].top >= st->cols[x - 1].top);
        assert(st->cols[x].src_x >= st->cols[x - 1].src_x);
        assert(st->cols[x].shade <= st->cols[x - 1].shade);
        assert(st->cols[x].refl <= st->cols[x - 1].refl);
    }
    /* The near edge is never shorter than the far edge, and heights fall step by step. */
    if (step) assert(st->cols[0].height <= t->steps[step - 1].cols[0].height);
}

static void check_full_tilt(const cover_flow_tilt_t * t) {
    const cover_flow_tilt_step_t * st = &t->steps[COVER_FLOW_TILT_STEPS];
    assert(st->width == FACE * params.far_width_pct / 100);
    int near_h = FACE * params.side_height_pct / 100;
    assert(st->cols[0].height >= near_h - 2 && st->cols[0].height <= near_h); /* shorter than the centre */
    int far_h = st->cols[st->width - 1].height, want = near_h * params.far_height_pct / 100;
    assert(far_h >= want - 3 && far_h <= want + 3);
    assert(t->out_h == FACE + FACE * params.reflect_pct / 100);
}

/* A colour that is none of the test artwork's. */
#define BG 0x1082

static int green(uint16_t p) { return (p >> 5) & 63; }

/* Step 0 is the face with its reflection under it, blended towards the background. */
static void check_identity(const cover_flow_tilt_t * t) {
    uint16_t * src = malloc(FACE * FACE * 2);
    size_t bytes = cover_flow_tilt_bytes(t, 0);
    uint16_t * dst = calloc(1, bytes);
    assert(src && dst && t->steps[0].width == FACE && bytes == (size_t) FACE * t->out_h * 2);
    for (int i = 0; i < FACE * FACE; i++) src[i] = (uint16_t) (i * 2654435761u >> 16);
    cover_flow_tilt_warp(t, 0, true, src, dst, BG);
    assert(memcmp(dst, src, FACE * FACE * 2) == 0); /* the cover part is the face */
    free(src);
    free(dst);
}

/* Reflection rows start near the configured strength and fade into the background. */
static void check_reflection(const cover_flow_tilt_t * t, int step, bool near_left) {
    const cover_flow_tilt_step_t * st = &t->steps[step];
    int w = st->width;
    uint16_t * src = malloc(FACE * FACE * 2);
    uint16_t * dst = calloc(1, cover_flow_tilt_bytes(t, step));
    assert(src && dst);
    for (int i = 0; i < FACE * FACE; i++) src[i] = 0xFFFF;
    cover_flow_tilt_warp(t, step, near_left, src, dst, BG);
    int x = near_left ? 0 : w - 1; /* the tallest column */
    const cover_flow_tilt_col_t * c = &st->cols[0];
    int bottom = c->top + c->height, prev = 64;
    int g_bg = green(BG), g_first = green(dst[(size_t) bottom * w + x]);
    assert(green(dst[(size_t) (bottom - 1) * w + x]) >= 62); /* the cover itself is bright (shaded at most) */
    /* about reflect_alpha of the way from the background to white */
    int want = g_bg + (63 - g_bg) * params.reflect_alpha_pct / 100;
    assert(g_first >= want - 3 && g_first <= want + 3);
    for (int y = bottom; y < bottom + c->refl; y++) {
        int g = green(dst[(size_t) y * w + x]);
        assert(g <= prev && g >= g_bg); /* only ever moves towards the background */
        prev = g;
    }
    assert(prev <= g_bg + 2); /* the last reflection row is all but gone */
    for (int y = bottom + c->refl; y < t->out_h; y++) assert(dst[(size_t) y * w + x] == BG);
    free(src);
    free(dst);
}

/* A full-tilt warp: the corners outside the trapezoid are the background, the
 * near edge is at the outer side, and the two sides are mirror images. */
static void check_warp_mirror(const cover_flow_tilt_t * t) {
    int step = COVER_FLOW_TILT_STEPS, w = t->steps[step].width;
    size_t bytes = cover_flow_tilt_bytes(t, step);
    uint16_t * src = malloc(FACE * FACE * 2);
    uint16_t * a = calloc(1, bytes), * b = calloc(1, bytes);
    assert(src && a && b);
    for (int i = 0; i < FACE * FACE; i++) src[i] = 0xFFFF;
    cover_flow_tilt_warp(t, step, true, src, a, BG);
    cover_flow_tilt_warp(t, step, false, src, b, BG);
    int mid = FACE / 2, last = t->out_h - 1;
    assert(a[mid * w] != BG && b[mid * w + w - 1] != BG); /* near edge, at the outer side */
    assert(a[w - 1] == BG && b[0] == BG);                 /* far edge, top corner */
    assert(a[(size_t) last * w + w - 1] == BG && b[(size_t) last * w] == BG);
    assert(a[(size_t) last * w] == BG && b[(size_t) last * w + w - 1] == BG); /* below the reflection */
    assert(a[(size_t) mid * w + w - 1] != BG); /* the far edge still holds cover at mid height */
    for (int y = 0; y < t->out_h; y++)
        for (int x = 0; x < w; x++) assert(a[y * w + x] == b[y * w + (w - 1 - x)]);
    free(src); free(a); free(b);
}

/* The artwork is never mirrored: on either side the source column grows from the
 * left edge of the drawn cover to its right edge. Uses an unshaded tilt so the
 * pixel values stay the column index. */
static void check_warp_orientation(void) {
    cover_flow_tilt_params_t flat_shade = params;
    flat_shade.far_shade_pct = 0;
    cover_flow_tilt_t * t = cover_flow_tilt_create(FACE, &flat_shade);
    int step = COVER_FLOW_TILT_STEPS, w = t->steps[step].width;
    size_t bytes = cover_flow_tilt_bytes(t, step);
    uint16_t * src = malloc(FACE * FACE * 2);
    uint16_t * out[2] = { calloc(1, bytes), calloc(1, bytes) };
    assert(src && out[0] && out[1]);
    for (int y = 0; y < FACE; y++)
        for (int x = 0; x < FACE; x++) src[y * FACE + x] = (uint16_t) x;
    for (int side = 0; side < 2; side++) {
        cover_flow_tilt_warp(t, step, side == 0, src, out[side], BG);
        const uint16_t * r = out[side] + (size_t) (FACE / 2) * w;
        assert(r[0] <= 4 && r[w - 1] >= FACE - 5); /* left edge shows the left of the art */
        for (int x = 1; x < w; x++) assert(r[x] >= r[x - 1]);
    }
    free(src); free(out[0]); free(out[1]);
    cover_flow_tilt_destroy(t);
}

/* Soft top edge: the first row of a column sits between the cover and the background. */
static void check_soft_edge(const cover_flow_tilt_t * t) {
    int step = COVER_FLOW_TILT_STEPS, w = t->steps[step].width;
    uint16_t * src = malloc(FACE * FACE * 2);
    uint16_t * dst = calloc(1, cover_flow_tilt_bytes(t, step));
    assert(src && dst);
    for (int i = 0; i < FACE * FACE; i++) src[i] = 0xFFFF;
    cover_flow_tilt_warp(t, step, true, src, dst, BG);
    const cover_flow_tilt_col_t * c = &t->steps[step].cols[w / 2];
    int g = green(dst[(size_t) c->top * w + w / 2]);
    assert(g > green(BG) + 4 && g < green(dst[(size_t) (c->top + 3) * w + w / 2]));
    assert(dst[(size_t) (c->top - 1) * w + w / 2] == BG);
    free(src); free(dst);
}

/* ---- The warp as first written, one pixel at a time, kept as the reference ---- */

static inline uint16_t ref_shade(uint16_t p, uint32_t shade32) {
    uint32_t x = ((uint32_t) p | ((uint32_t) p << 16)) & 0x07E0F81Fu;
    x = ((x * shade32) >> 5) & 0x07E0F81Fu;
    return (uint16_t) (x | (x >> 16));
}

static inline uint16_t ref_mix(uint16_t fg, uint16_t bg, uint32_t alpha) {
    uint32_t f = ((uint32_t) fg | ((uint32_t) fg << 16)) & 0x07E0F81Fu;
    uint32_t b = ((uint32_t) bg | ((uint32_t) bg << 16)) & 0x07E0F81Fu;
    uint32_t a = (alpha + 4) >> 3;
    uint32_t x = (((f - b) * a >> 5) + b) & 0x07E0F81Fu;
    return (uint16_t) (x | (x >> 16));
}

static void ref_warp(const cover_flow_tilt_t * t, int step, bool near_left, const uint16_t * src, uint16_t * dst, uint16_t bg) {
    const cover_flow_tilt_step_t * st = &t->steps[step];
    int w = st->width, face = t->face_px;
    const int dir = near_left ? 1 : -1, sflip = near_left ? 0 : face - 1, start = near_left ? 0 : w - 1;
    for (int y = 0; y < t->out_h; y++) {
        uint16_t * row = dst + (size_t) y * (size_t) w;
        int n = 0, k = 0; /* tops rise and ends fall with x, so a row holds a prefix of the columns */
        while (n < w && y >= st->cols[n].top && y < st->cols[n].top + st->cols[n].height + st->cols[n].refl) n++;
        while (k < n && y < st->cols[k].top + st->cols[k].height) k++;
        uint16_t * empty = near_left ? row + n : row;
        for (int x = 0; x < w - n; x++) empty[x] = bg;
        const cover_flow_tilt_col_t * c = st->cols;
        for (int x = 0; x < n; x++, c++) {
            int u = y - c->top, d = start + dir * x;
            uint32_t alpha = 255;
            if (x >= k) {
                uint32_t depth = (uint32_t) (u - c->height);
                alpha = (t->alpha_fp - depth * c->refl_step - (c->refl_step >> 1)) >> 16;
                u = 2 * c->height - 1 - u;
            }
            uint32_t sy = ((uint32_t) (2 * u + 1) * c->v_step) >> 17;
            uint16_t p = src[(size_t) sy * (size_t) face + (size_t) (sflip + dir * c->src_x)];
            if (c->shade < 256) p = ref_shade(p, c->shade >> 3);
            row[d] = alpha < 255 ? ref_mix(p, bg, alpha) : p;
        }
    }
    if (step)
        for (int x = 0; x < w; x++) {
            uint16_t * px = &dst[st->cols[x].top * w + (near_left ? x : w - 1 - x)];
            *px = ref_mix(*px, bg, 128);
        }
}

/* Every step and both sides come out bit for bit as the reference makes them. */
static void check_matches_reference(const cover_flow_tilt_t * t) {
    int face = t->face_px;
    uint16_t * src = malloc(face * face * 2);
    assert(src);
    for (int i = 0; i < face * face; i++) src[i] = (uint16_t) ((i * 2654435761u) >> 13);
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++)
        for (int side = 0; side < 2; side++) {
            size_t bytes = cover_flow_tilt_bytes(t, s);
            uint16_t * a = malloc(bytes), * b = malloc(bytes);
            assert(a && b);
            memset(a, 0xA5, bytes); /* stale bytes must not survive */
            memset(b, 0x5A, bytes);
            cover_flow_tilt_warp(t, s, side == 0, src, a, BG);
            ref_warp(t, s, side == 0, src, b, BG);
            assert(memcmp(a, b, bytes) == 0);
            memset(a, 0xA5, bytes);
            int w = t->steps[s].width;
            cover_flow_tilt_warp_columns(t, s, side == 0, src, a, BG, -5, -1);
            cover_flow_tilt_warp_columns(t, s, side == 0, src, a, BG, w + 1, w + 5);
            cover_flow_tilt_warp_columns(t, s, side == 0, src, a, BG, 3, 2);
            for (size_t i = 0; i < bytes / 2; i++) assert(a[i] == 0xA5A5);
            for (int x = 0; x < w; x += 11) {
                int end = x + 11 < w ? x + 11 : w;
                cover_flow_tilt_warp_columns(t, s, side == 0, src, a, BG, x, end);
                for (int y = 0; y < t->out_h; y++)
                    for (int col = 0; col < w; col++)
                        assert(a[y * w + col] == (col < end ? b[y * w + col] : 0xA5A5));
            }
            free(a); free(b);
        }
    free(src);
}

int main(void) {
    cover_flow_tilt_t * t = cover_flow_tilt_create(FACE, &params);
    assert(t);
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++) {
        check_monotonic(t, s);
        if (s) assert(t->steps[s].width < t->steps[s - 1].width);
        check_reflection(t, s, true);
        check_reflection(t, s, false);
    }
    check_full_tilt(t);
    check_identity(t);
    check_warp_mirror(t);
    check_warp_orientation();
    check_soft_edge(t);
    check_matches_reference(t);
    cover_flow_tilt_destroy(t);
    const int sizes[] = { 8, 93, 187 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int reflect = 0; reflect <= 35; reflect += 35) {
            cover_flow_tilt_params_t scaled = params;
            scaled.reflect_pct = reflect;
            t = cover_flow_tilt_create(sizes[i], &scaled);
            assert(t);
            check_matches_reference(t);
            cover_flow_tilt_destroy(t);
        }
    }
    puts("cover_flow_tilt: PASS");
    return 0;
}
