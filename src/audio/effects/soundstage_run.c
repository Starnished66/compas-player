#include "soundstage_hot.h"

/* Value of a ramp at the middle of the next n samples; moves it n ahead. */
static float ss_mid(ss_ramp_t * r, unsigned int n) {
    float a = r->cur, b = ss_ramp_advance(r, n);
    return a + 0.5f * (b - a);
}

static void ss_seg_ms(ss_t * s, ss_seg_t * g, unsigned int n) {
    float side = ss_mid(&s->side, n), cap = ss_mid(&s->cap, n);
    g->gs = side * cap;
    g->shelf0 = s->shelf_on[0];
    g->shelf1 = s->shelf_on[1];
}

/* Reflection level, its energy compensation (scales the whole output) and
 * the tap state: crossfade, and whether history reads need the valid check. */
static void ss_seg_refl(ss_t * s, ss_seg_t * g, unsigned int n) {
    float g0 = s->refl.cur, g1 = ss_ramp_advance(&s->refl, n);
    float c1 = g1 != g0 ? ss_comp(g1) : s->comp;
    g->kref = 0.25f * (g0 + g1);
    g->fh = 0.25f * (s->comp + c1);
    s->comp = c1;
    g->refl_on = g0 != 0.0f || g1 != 0.0f;
    if (!g->refl_on) ss_refl_clear(s);
    g->checked = g->refl_on && s->valid < s->maxtap;
    g->blending = g->refl_on && s->blending;
    if (!g->blending) return;
    float b0 = s->blend, b1 = b0 + (1.0f / (SS_BLEND_SEC * (float)s->rate)) * (float)n;
    if (b1 > 1.0f) b1 = 1.0f;
    g->blend = 0.5f * (b0 + b1);
    s->blend = b1;
    if (b1 >= 1.0f) s->blending = false;
}

/* Binaural amount, and the far-path delay: a fixed integer plus fraction
 * unless the angle is still moving. */
static void ss_seg_head(ss_t * s, ss_seg_t * g, unsigned int n) {
    float a0 = s->amount.cur, a1 = ss_ramp_advance(&s->amount, n);
    g->head_on = a0 != 0.0f || a1 != 0.0f;
    g->amt = 0.5f * (a0 + a1);
    if (!g->head_on) {
        if (s->head_live) ss_head_clear(s);
        return;
    }
    if (!s->head_live) s->delay.cur = s->delay.tgt;
    s->head_live = true;
    g->moving = s->delay.cur != s->delay.tgt;
    float d0 = s->delay.cur, d1 = ss_ramp_advance(&s->delay, n);
    g->dly = d0;
    g->dly_inc = (d1 - d0) / (float)n;
    g->dn = (unsigned int)d0;
    g->dfrac = d0 - (float)g->dn;
}

static void ss_seg_begin(ss_t * s, ss_seg_t * g, unsigned int n) {
    memset(g, 0, sizeof(*g));
    g->mix = ss_mid(&s->mix, n);
    g->mix_one = g->mix == 1.0f;
    ss_seg_ms(s, g, n);
    ss_seg_refl(s, g, n);
    ss_seg_head(s, g, n);
}

/* A frame over the ceiling or not finite: the slow guard decides each frame
 * of the segment, with its own input. */
static void ss_guard_segment(ss_t * s, const float * lr, float * out, unsigned int n, float ceiling) {
    s->over = true;
    for (unsigned int i = 0; i < n; i++) {
        if (!ss_guard_frame(s, lr + 2 * i, out + 2 * i, ceiling)) {
            out[2 * i] = lr[2 * i];
            out[2 * i + 1] = lr[2 * i + 1];
        }
    }
}

void ss_run_segment(ss_t * s, float * lr, unsigned int n, float ceiling) {
    ss_seg_t g;
    float m[SS_SEG], d[SS_SEG], out[2 * SS_SEG];
    ss_seg_begin(s, &g, n);
    ss_pass_in(&g, lr, m, d, n);
    if (g.shelf0) ss_pass_shelf(&s->shelf[0], &s->shelf_hist[0], d, n);
    if (g.shelf1) ss_pass_shelf(&s->shelf[1], &s->shelf_hist[1], d, n);
    if (g.refl_on) ss_pass_refl(s, &g, m, d, n);
    if (g.head_on && g.moving) ss_pass_head(s, &g, d, n, true);
    else if (g.head_on) ss_pass_head(s, &g, d, n, false);
    ss_pass_out(&g, lr, m, d, out, n);
    if (ss_top_bits(out, 2 * n) > ss_abs_bits(ceiling)) ss_guard_segment(s, lr, out, n, ceiling);
    memcpy(lr, out, 2 * (size_t)n * sizeof(float));
    ss_flush_denormals(s);
}
