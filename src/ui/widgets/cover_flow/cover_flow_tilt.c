#include "cover_flow_tilt.h"
#include <stdlib.h>
#include <string.h>

#define SHADE_FULL 256
#define EDGE_ALPHA 128 /* the soft pixel on the top row of a column */
#define CF_WARP_BAND_ROWS 32 /* rows warped per sweep of the columns; at or above out_h it is one sweep */

static int step_width(int face, int far_width_pct, int step) {
    int far_w = face * far_width_pct / 100;
    return face - (face - far_w) * step / COVER_FLOW_TILT_STEPS;
}

/* Cover and reflection rows of a column, from its height. */
static void set_rows(cover_flow_tilt_col_t * c, const cover_flow_tilt_t * t, int pct, int h) {
    int r = h * pct / 100;
    c->height = (uint16_t) h;
    c->top = (int16_t) ((t->face_px - h) / 2);
    if (c->top + h + r > t->out_h) r = t->out_h - c->top - h;
    c->refl = (uint16_t) r;
    c->refl_step = r ? t->alpha_fp / (uint32_t) r : 0;
}

/* Fills the columns of one step. Plane geometry: a point at distance s (0..1)
 * along the cover is scaled by 1 / (1 + a*s), where the far edge is `ratio`
 * of the near edge. Inverting the projected x gives the source column. */
static void fill_step(cover_flow_tilt_t * t, int step, const cover_flow_tilt_params_t * p) {
    cover_flow_tilt_step_t * st = &t->steps[step];
    int face = t->face_px, w = st->width;
    float near_h = face * (1.0f - (1.0f - p->side_height_pct / 100.0f) * step / COVER_FLOW_TILT_STEPS);
    float ratio = 1.0f - (1.0f - p->far_height_pct / 100.0f) * step / COVER_FLOW_TILT_STEPS;
    float a = 1.0f / ratio - 1.0f;
    float drop = (SHADE_FULL * p->far_shade_pct / 100.0f) * step / COVER_FLOW_TILT_STEPS;
    for (int x = 0; x < w; x++) {
        float proj = (x + 0.5f) / w;
        float s = proj / ((1.0f + a) - a * proj);
        int src_x = (int) (s * face);
        if (src_x > face - 1) src_x = face - 1;
        int h = (int) (near_h / (1.0f + a * s) + 0.5f);
        if (h > face) h = face;
        if (h < 2) h = 2;
        if (x > 0 && h > st->cols[x - 1].height) h = st->cols[x - 1].height; /* never grows inward */
        cover_flow_tilt_col_t * c = &st->cols[x];
        c->src_x = (uint16_t) src_x;
        set_rows(c, t, p->reflect_pct, h);
        c->shade = (uint16_t) (SHADE_FULL - (int) (drop * proj));
        c->v_step = ((uint32_t) face << 16) / (uint32_t) h;
    }
}

static void fill_flat(cover_flow_tilt_t * t, const cover_flow_tilt_params_t * p) {
    cover_flow_tilt_step_t * st = &t->steps[0];
    for (int x = 0; x < t->face_px; x++) {
        st->cols[x] = (cover_flow_tilt_col_t) { .src_x = (uint16_t) x, .shade = SHADE_FULL, .v_step = 1u << 16 };
        set_rows(&st->cols[x], t, p->reflect_pct, t->face_px);
    }
}

cover_flow_tilt_t * cover_flow_tilt_create(int face_px, const cover_flow_tilt_params_t * params) {
    if (face_px < 8 || face_px > 2048 || !params) return NULL;
    cover_flow_tilt_t * t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->face_px = face_px;
    t->out_h = face_px + face_px * params->reflect_pct / 100;
    t->alpha_fp = (uint32_t) (255 * params->reflect_alpha_pct / 100) << 16;
    size_t total = 0;
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++) {
        t->steps[s].width = s ? step_width(face_px, params->far_width_pct, s) : face_px;
        total += (size_t) t->steps[s].width * sizeof(cover_flow_tilt_col_t);
    }
    t->block = calloc(1, total);
    if (!t->block) { free(t); return NULL; }
    uint8_t * at = t->block;
    for (int s = 0; s <= COVER_FLOW_TILT_STEPS; s++) {
        cover_flow_tilt_step_t * st = &t->steps[s];
        st->cols = (cover_flow_tilt_col_t *) at;
        at += (size_t) st->width * sizeof(cover_flow_tilt_col_t);
        if (s) fill_step(t, s, params);
        else fill_flat(t, params);
    }
    return t;
}

void cover_flow_tilt_destroy(cover_flow_tilt_t * tilt) {
    if (!tilt) return;
    free(tilt->block);
    free(tilt);
}

size_t cover_flow_tilt_bytes(const cover_flow_tilt_t * tilt, int step) {
    return (size_t) tilt->steps[step].width * (size_t) tilt->out_h * 2;
}

/* Scales an RGB565 pixel by shade32/32 with two multiplies. */
static inline uint16_t shade_px(uint16_t p, uint32_t shade32) {
    uint32_t x = ((uint32_t) p | ((uint32_t) p << 16)) & 0x07E0F81Fu;
    x = ((x * shade32) >> 5) & 0x07E0F81Fu;
    return (uint16_t) (x | (x >> 16));
}

/* `fg` over `bg` at alpha 0..255, both RGB565. */
static inline uint16_t mix_px(uint16_t fg, uint16_t bg, uint32_t alpha) {
    uint32_t f = ((uint32_t) fg | ((uint32_t) fg << 16)) & 0x07E0F81Fu;
    uint32_t b = ((uint32_t) bg | ((uint32_t) bg << 16)) & 0x07E0F81Fu;
    uint32_t a = (alpha + 4) >> 3; /* 0..32 */
    uint32_t x = (((f - b) * a >> 5) + b) & 0x07E0F81Fu;
    return (uint16_t) (x | (x >> 16));
}

/* One destination column, rows [from, to) only: background above, the cover, the reflection
 * fading into the background, then background again. `s0` is source column 0 as seen from the
 * near edge and `dir` walks away from it, so the artwork keeps its orientation on both sides of
 * the centre. `p` is the column's first pixel and `w` the row pitch. Each run starts at the
 * accumulator values its first row would have reached, so any split gives the same pixels. */
static void warp_column(const cover_flow_tilt_t * t, const cover_flow_tilt_col_t * c, const uint16_t * s0, int dir,
                        int w, int from, int to, bool soft, uint16_t * p, uint16_t bg) {
    const int face = t->face_px, top = c->top, bottom = top + c->height, end = bottom + c->refl;
    const uint32_t shade = c->shade >> 3, v2 = 2 * c->v_step;
    const uint16_t * s = s0 + dir * (int) c->src_x;
    int y = from, stop = to < top ? to : top;
    p += (size_t) from * (size_t) w;
    for (; y < stop; y++, p += w) *p = bg;
    stop = to < bottom ? to : bottom;
    uint16_t * first = p;
    uint32_t acc = (uint32_t) (2 * (y - top) + 1) * c->v_step; /* the source row is its top bits */
    for (; y < stop; y++, p += w, acc += v2) *p = shade_px(s[(acc >> 17) * (uint32_t) face], shade);
    if (soft && first != p && top >= from) *first = mix_px(*first, bg, EDGE_ALPHA); /* soft top pixel */
    stop = to < end ? to : end;
    uint32_t k = (uint32_t) (y - bottom); /* the reflection reads the cover upwards from its bottom */
    acc = (uint32_t) (2 * c->height - 1) * c->v_step - k * v2;
    uint32_t alpha = t->alpha_fp - (c->refl_step >> 1) - k * c->refl_step;
    for (; y < stop; y++, p += w, acc -= v2, alpha -= c->refl_step)
        *p = mix_px(shade_px(s[(acc >> 17) * (uint32_t) face], shade), bg, alpha >> 16);
    stop = to < t->out_h ? to : t->out_h;
    for (; y < stop; y++, p += w) *p = bg;
}

void cover_flow_tilt_warp(const cover_flow_tilt_t * tilt, int step, bool near_left,
                          const uint16_t * src, uint16_t * dst, uint16_t bg) {
    const cover_flow_tilt_step_t * st = &tilt->steps[step];
    int face = tilt->face_px, w = st->width, done = 0;
    if (step == 0) { /* the flat cover is the face: copy it (unless it is already there) and warp only the reflection */
        if (dst != src) memcpy(dst, src, (size_t) face * (size_t) face * 2);
        done = face;
    }
    const int dir = near_left ? 1 : -1;
    const uint16_t * s0 = src + (near_left ? 0 : face - 1);
    /* Bands of rows keep the destination lines in cache across the columns. */
    for (int from = done; from < tilt->out_h; from += CF_WARP_BAND_ROWS) {
        int to = from + CF_WARP_BAND_ROWS < tilt->out_h ? from + CF_WARP_BAND_ROWS : tilt->out_h;
        for (int x = 0; x < w; x++)
            warp_column(tilt, &st->cols[x], s0, dir, w, from, to, step != 0, dst + (near_left ? x : w - 1 - x), bg);
    }
}
