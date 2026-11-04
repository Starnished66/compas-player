#include "soundstage_internal.h"
#include "biquad_cookbook.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Shelf as x + v, where v is the response of (B - A) / A: the numerator is
 * the small difference of the cookbook B and A, so the float coefficients keep
 * their relative precision. A plain float B has a DC sum near 1e-6 at 384 kHz
 * and 50 Hz, which float rounding alone moves by up to 9%. The low shelf's
 * 1 + a1 + a2 is equally small, so a2 is chosen to make it as exact as float
 * allows and the numerator is rescaled to keep the DC gain exactly nominal.
 * Returns the gain at the shelf's extreme (DC for a low shelf, Nyquist for a
 * high one), evaluated from the rounded coefficients. */
static double ss_shelf_float(ss_biquad_t * out, biquad_cookbook_t k, bool low) {
    float a1 = (float)k.a1, a2 = (float)k.a2;
    double scale = 1.0;
    if (low) {
        double want = 1.0 + k.a1 + k.a2;
        a2 = (float)(want - 1.0 - (double)a1);
        scale = (1.0 + (double)a1 + (double)a2) / want;
    }
    *out = (ss_biquad_t){ (float)((k.b0 - 1.0) * scale), (float)((k.b1 - k.a1) * scale),
                          (float)((k.b2 - k.a2) * scale), a1, a2 };
    double sign = low ? 1.0 : -1.0;
    return 1.0 + ((double)out->b0 + sign * (double)out->b1 + (double)out->b2) /
                 (1.0 + sign * (double)a1 + (double)a2);
}

static void ss_shelf_one(ss_t * s, int i, biquad_cookbook_type_t type, int db, int hz) {
    double gain_db = s->sm[db];
    double freq = fmin(s->sm[hz], 0.45 * (double)s->rate);
    bool on = gain_db != 0.0;
    s->shelf_gain[i] = 1.0f;
    if (on) {
        double w0 = 2.0 * M_PI * freq / (double)s->rate;
        biquad_cookbook_t k = biquad_cookbook(type, w0, gain_db, SS_SHELF_Q);
        s->shelf_gain[i] = (float)ss_shelf_float(&s->shelf[i], k, type == BIQUAD_COOKBOOK_LOW_SHELF);
    } else if (s->shelf_on[i]) {
        memset(&s->shelf_hist[i], 0, sizeof(s->shelf_hist[i]));
    }
    s->shelf_on[i] = on;
}

/* Side low and high shelves from the smoothed parameters. The shelves peak at
 * DC and Nyquist, so those gains of the rounded coefficients bound how far
 * they can raise the side signal. */
static void ss_shelf_coefs(ss_t * s) {
    ss_shelf_one(s, 0, BIQUAD_COOKBOOK_LOW_SHELF, SS_SM_LOW_DB, SS_SM_LOW_HZ);
    ss_shelf_one(s, 1, BIQUAD_COOKBOOK_HIGH_SHELF, SS_SM_HIGH_DB, SS_SM_HIGH_HZ);
    float peak = s->shelf_gain[0] > s->shelf_gain[1] ? s->shelf_gain[0] : s->shelf_gain[1];
    s->shelf_max = peak > 1.0f ? peak : 1.0f;
}

/* The side EQ shapes only the side; the mid is untouched. The combined peak
 * side gain is capped at 2 by reducing the broadband side gain. The peak is
 * taken where the broadband ramp will be at the end of this sub-block, and the
 * cap ramps across it, except after a restart, when it starts at its target.
 * Any resulting peaks are left to the guard and limiter. */
void ss_ms_eq_update(ss_t * s, bool moved, bool snap) {
    if (moved) ss_shelf_coefs(s);
    float peak = ss_ramp_peek(&s->side, s->sub_len) * s->shelf_max;
    float cap = peak > 2.0f ? 2.0f / peak : 1.0f;
    if (snap) s->cap.cur = cap; /* a restart must not pass a boost uncapped */
    ss_ramp_aim(&s->cap, cap, s->sub_len);
}

static float ss_peak(const float v[2]) {
    float a = fabsf(v[0]), b = fabsf(v[1]);
    return a > b ? a : b;
}

/* Slow path of the per-frame guard, reached only when a frame is over the
 * ceiling or not finite. Same rule as crossfeed_frame: a frame
 * over the ceiling is scaled to it unless the stage input was already
 * louder. A non-finite result drops all state and the frame passes dry. */
bool ss_guard_frame(ss_t * s, const float in[2], float out[2], float ceiling) {
    if (!isfinite(out[0]) || !isfinite(out[1])) {
        ss_clear_state(s);
        return false;
    }
    float in_peak = ss_peak(in), out_peak = ss_peak(out);
    if (out_peak > ceiling && out_peak > in_peak) {
        float allowed = in_peak > ceiling ? in_peak : ceiling;
        if (out_peak > allowed) {
            float gain = allowed / out_peak;
            out[0] *= gain;
            out[1] *= gain;
        }
    }
    return true;
}

/* Drop all signal memory; smoothed parameters stay. The reflection rings are
 * cleared lazily through their valid counter. */
void ss_clear_state(ss_t * s) {
    memset(s->shelf_hist, 0, sizeof(s->shelf_hist));
    ss_room_clear(s);
}

static void ss_flush(float * v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &v[i], sizeof u);
        if ((u & 0x7fffffffu) < SS_TINY_BITS) v[i] = 0.0f;
    }
}

/* After every segment: flush decayed IIR state to zero. FCSR flush-to-zero is
 * not relied on. The reflection low-pass is flushed per sample instead. */
void ss_flush_denormals(ss_t * s) {
    ss_flush((float *)s->shelf_hist, sizeof(s->shelf_hist) / (sizeof(float)));
    ss_flush((float *)&s->hn_hist, sizeof(s->hn_hist) / (sizeof(float)));
    ss_flush((float *)&s->hf_hist, sizeof(s->hf_hist) / (sizeof(float)));
}

/* Energy compensation 1 / sqrt(1 + sum g^2) for reflections at level g. */
float ss_comp(float g) {
    return 1.0f / sqrtf(1.0f + 0.25f * g * g);
}
