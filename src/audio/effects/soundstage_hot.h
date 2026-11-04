#ifndef SOUNDSTAGE_HOT_H
#define SOUNDSTAGE_HOT_H

/* Per-frame stage code, included only by soundstage_run.c. All math is single
 * precision, and each stage is a branch-free pass over a segment of frames so
 * independent iterations can overlap on the in-order FPU. Work is done in the
 * unnormalised mid/side domain, m = l + r and d = l - r (twice the usual M
 * and S); the factors of one half are folded into constants. */
#include "soundstage_internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Constants of one segment: stage switches and gains. A gain that ramps
 * holds its value at the middle of the segment, so a segment of at most
 * SS_SEG frames is a short staircase step of the linear ramp. */
typedef struct {
    bool shelf0, shelf1, refl_on, head_on, mix_one, blending, checked, moving;
    float gs, mix, amt, kref, fh, blend, dly, dly_inc, dfrac;
    unsigned int dn;
} ss_seg_t;

/* Magnitude bits of a float: orders like |x|, and NaN or Inf sort above every
 * finite value. Integer compares avoid stalling on the FPU condition flag. */
static inline uint32_t ss_abs_bits(float x) {
    uint32_t u;
    memcpy(&u, &x, sizeof u);
    return u & 0x7fffffffu;
}

/* Pass 1: mid and side (twice the usual M and S), with the broadband side
 * gain. Shelves and this gain are linear, so order is free. */
static inline void ss_pass_in(const ss_seg_t * g, const float * lr, float * m, float * d,
                              unsigned int n) {
    const float gs = g->gs;
    unsigned int i = 0;
    /* Four frames with distinct temporaries so the multiplies overlap. */
    for (; i + 4 <= n; i += 4) {
        float l0 = lr[2 * i], r0 = lr[2 * i + 1], l1 = lr[2 * i + 2], r1 = lr[2 * i + 3];
        float l2 = lr[2 * i + 4], r2 = lr[2 * i + 5], l3 = lr[2 * i + 6], r3 = lr[2 * i + 7];
        float t0 = l0 - r0, t1 = l1 - r1, t2 = l2 - r2, t3 = l3 - r3;
        float d0 = gs * t0, d1 = gs * t1, d2 = gs * t2, d3 = gs * t3;
        m[i] = l0 + r0; m[i + 1] = l1 + r1; m[i + 2] = l2 + r2; m[i + 3] = l3 + r3;
        d[i] = d0; d[i + 1] = d1; d[i + 2] = d2; d[i + 3] = d3;
    }
    for (; i < n; i++) {
        m[i] = lr[2 * i] + lr[2 * i + 1];
        d[i] = gs * (lr[2 * i] - lr[2 * i + 1]);
    }
}

/* Shelf as x + v (see ss_shelf_float): direct form I on v, whose feed-forward
 * and old-output products do not wait for the newest output, so the recurrence
 * is one multiply and one subtract. Coefficients stay in memory: only 16
 * single FPU registers exist, and a spill or integer-register copy costs more
 * than the load. The history holds x and v. */
static inline void ss_pass_shelf(const ss_biquad_t * c, ss_hist_t * h, float * d, unsigned int n) {
    float x1 = h->x1, x2 = h->x2, v1 = h->y1, v2 = h->y2;
    unsigned int i = 0;
    /* Feed-forward sums of four samples first (independent, overlapping),
     * then the four-step recurrence. */
    for (; i + 4 <= n; i += 4) {
        float xa = d[i], xb = d[i + 1], xc = d[i + 2], xd = d[i + 3];
        float fa = (c->b0 * xa + c->b1 * x1) + c->b2 * x2;
        float fb = (c->b0 * xb + c->b1 * xa) + c->b2 * x1;
        float fc = (c->b0 * xc + c->b1 * xb) + c->b2 * xa;
        float fd = (c->b0 * xd + c->b1 * xc) + c->b2 * xb;
        float va = (fa - c->a2 * v2) - c->a1 * v1;
        float vb = (fb - c->a2 * v1) - c->a1 * va;
        float vc = (fc - c->a2 * va) - c->a1 * vb;
        float vd = (fd - c->a2 * vb) - c->a1 * vc;
        d[i] = xa + va; d[i + 1] = xb + vb; d[i + 2] = xc + vc; d[i + 3] = xd + vd;
        x2 = xc; x1 = xd; v2 = vc; v1 = vd;
    }
    for (; i < n; i++) {
        float x = d[i];
        float v = (((c->b0 * x + c->b1 * x1) + c->b2 * x2) - c->a2 * v2) - c->a1 * v1;
        x2 = x1; x1 = x; v2 = v1; v1 = v;
        d[i] = x + v;
    }
    h->x1 = x1; h->x2 = x2; h->y1 = v1; h->y2 = v2;
}

/* Slot (wpos + 1 - tap) is the newest sample a tap may see; it exists once
 * tap - 2 older slots have been written. */
static inline float ss_tap_read(const ss_t * s, const float * ring, unsigned int tap, bool checked) {
    if (checked && tap > s->valid + 1u) return 0.0f;
    return ring[(s->wpos + 1u - tap) & SS_REFL_MASK];
}

/* Sum of the 8 taps per feed. Taps are integer slots of the decimated ring. */
static inline void ss_taps(const ss_t * s, const unsigned int taps[2][SS_REFL_TAPS],
                           bool checked, float * sl, float * sr) {
    float l[2] = { 0.0f, 0.0f }, r[2] = { 0.0f, 0.0f };
#pragma GCC unroll 8
    for (int k = 0; k < SS_REFL_TAPS; k++) {
        l[k & 1] += s->gain[k] * ss_tap_read(s, s->ring[k & 1], taps[0][k], checked);
        r[k & 1] += s->gain[k] * ss_tap_read(s, s->ring[(k & 1) ^ 1], taps[1][k], checked);
    }
    *sl = l[0] + l[1];
    *sr = r[0] + r[1];
}

/* End of a block of decim frames: store its mean in the ring, then read the
 * taps one slot ahead. Frames of the next block interpolate linearly from the
 * previous sums to these. Mid and side are sum and difference of the feeds. */
static inline void ss_refl_block_end(ss_t * s, const ss_seg_t * g) {
    s->ring[0][s->wpos & SS_REFL_MASK] = s->acc[0] * s->inv_d;
    s->ring[1][s->wpos & SS_REFL_MASK] = s->acc[1] * s->inv_d;
    s->acc[0] = s->acc[1] = 0.0f;
    s->wpos++;
    if (s->valid < SS_REFL_RING) s->valid++;
    float sl, sr;
    ss_taps(s, s->taps[1], g->checked, &sl, &sr);
    if (g->blending) {
        float ol, orr;
        ss_taps(s, s->taps[0], g->checked, &ol, &orr);
        sl = ol + g->blend * (sl - ol);
        sr = orr + g->blend * (sr - orr);
    }
    s->sm_cur = s->sm_next;
    s->sd_cur = s->sd_next;
    s->sm_next = sl + sr;
    s->sd_next = sl - sr;
    s->sm_step = (s->sm_next - s->sm_cur) * s->inv_d;
    s->sd_step = (s->sd_next - s->sd_cur) * s->inv_d;
}

/* Early reflections added to the virtual-speaker feeds. The low-passed input
 * is averaged over decim frames, the taps are summed once per block, and the
 * sums are interpolated back to the full rate. */
static inline void ss_pass_refl(ss_t * s, const ss_seg_t * g, float * m, float * d, unsigned int n) {
    float lp0 = s->lp[0], lp1 = s->lp[1], sm = s->sm_cur, sd = s->sd_cur;
    float acc0 = s->acc[0], acc1 = s->acc[1];
    float stm = s->sm_step, std = s->sd_step;
    const float alpha = s->lp_alpha, kref = g->kref;
    const unsigned int decim = s->decim;
    unsigned int phase = s->phase;
    for (unsigned int i = 0; i < n; i++) {
        float mi = m[i], di = d[i];
        lp0 += alpha * ((mi + di) - lp0);
        lp1 += alpha * ((mi - di) - lp1);
        if (ss_abs_bits(lp0) < SS_TINY_BITS) lp0 = 0.0f;
        if (ss_abs_bits(lp1) < SS_TINY_BITS) lp1 = 0.0f;
        acc0 += lp0;
        acc1 += lp1;
        m[i] = mi + kref * sm;
        d[i] = di + kref * sd;
        sm += stm;
        sd += std;
        if (++phase == decim) {
            phase = 0;
            s->acc[0] = acc0;
            s->acc[1] = acc1;
            ss_refl_block_end(s, g);
            acc0 = acc1 = 0.0f;
            sm = s->sm_cur; sd = s->sd_cur; stm = s->sm_step; std = s->sd_step;
        }
    }
    s->lp[0] = lp0;
    s->lp[1] = lp1;
    s->acc[0] = acc0;
    s->acc[1] = acc1;
    s->phase = phase;
    s->sm_cur = sm;
    s->sd_cur = sd;
}

/* Shuffler: only the side goes through Hn - Hf * delay. The delay is a fixed
 * integer plus fraction unless the angle is still moving. */
static inline void ss_pass_head(ss_t * s, const ss_seg_t * g, float * d, unsigned int n, bool moving) {
    const ss_pole_t hn = s->hn, hf = s->hf;
    float nx = s->hn_hist.x1, ny = s->hn_hist.y1, fx = s->hf_hist.x1, fy = s->hf_hist.y1;
    float dly = g->dly;
    unsigned int pos = s->dpos;
    for (unsigned int i = 0; i < n; i++) {
        float x = d[i];
        float near = (hn.b0 * x + hn.b1 * nx) - hn.a1 * ny;
        float far = (hf.b0 * x + hf.b1 * fx) - hf.a1 * fy;
        nx = x; ny = near; fx = x; fy = far;
        s->dring[pos & SS_HEAD_MASK] = far;
        unsigned int k = g->dn;
        float frac = g->dfrac;
        if (moving) {
            k = (unsigned int)dly;
            frac = dly - (float)k;
            dly += g->dly_inc;
        }
        float a = s->dring[(pos - k) & SS_HEAD_MASK];
        float b = s->dring[(pos - k - 1u) & SS_HEAD_MASK];
        pos++;
        d[i] = x + g->amt * ((near - (a + frac * (b - a))) - x);
    }
    s->hn_hist.x1 = nx; s->hn_hist.y1 = ny; s->hf_hist.x1 = fx; s->hf_hist.y1 = fy;
    s->dpos = pos;
}

/* Output: back to left and right, energy compensation, wet/dry mix. Inputs
 * are still in lr. */
static inline void ss_pass_out(const ss_seg_t * g, const float * lr, const float * m,
                               const float * d, float * out, unsigned int n) {
    const float fh = g->fh, mix = g->mix;
    unsigned int i = 0;
    for (; i + 2 <= n; i += 2) {
        float m0 = m[i], d0 = d[i], m1 = m[i + 1], d1 = d[i + 1];
        float o0 = fh * (m0 + d0), o1 = fh * (m0 - d0), o2 = fh * (m1 + d1), o3 = fh * (m1 - d1);
        if (!g->mix_one) {
            o0 = lr[2 * i] + mix * (o0 - lr[2 * i]);
            o1 = lr[2 * i + 1] + mix * (o1 - lr[2 * i + 1]);
            o2 = lr[2 * i + 2] + mix * (o2 - lr[2 * i + 2]);
            o3 = lr[2 * i + 3] + mix * (o3 - lr[2 * i + 3]);
        }
        out[2 * i] = o0; out[2 * i + 1] = o1; out[2 * i + 2] = o2; out[2 * i + 3] = o3;
    }
    for (; i < n; i++) {
        float o0 = fh * (m[i] + d[i]), o1 = fh * (m[i] - d[i]);
        if (!g->mix_one) {
            o0 = lr[2 * i] + mix * (o0 - lr[2 * i]);
            o1 = lr[2 * i + 1] + mix * (o1 - lr[2 * i + 1]);
        }
        out[2 * i] = o0;
        out[2 * i + 1] = o1;
    }
}

typedef uint32_t __attribute__((may_alias)) ss_word_t;

/* Largest output magnitude as bits, read back as integers: cheaper than
 * moving each float into an integer register. */
static inline uint32_t ss_top_bits(const float * out, unsigned int count) {
    const ss_word_t * w = (const ss_word_t *)out;
    uint32_t top = 0;
#pragma GCC unroll 8
    for (unsigned int i = 0; i < count; i++) {
        uint32_t b = w[i] & 0x7fffffffu;
        top = b > top ? b : top;
    }
    return top;
}

#endif
