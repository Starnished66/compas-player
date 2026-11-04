#include "soundstage_internal.h"

#include <complex.h>
#include <math.h>
#include <string.h>


/* ---- Early reflections ------------------------------------------------ */

/* Tap times in ms at room_size 1.0 (scale 1.0), per output feed. Feed 0
 * (left) reads L,R,L,R...; feed 1 (right) reads R,L,R,L... */
static const double tap_ms[2][SS_REFL_TAPS] = {
    { 7.1, 11.3, 13.7, 17.9, 21.4, 26.3, 31.7, 37.9 },
    { 8.3, 10.6, 14.9, 16.8, 22.7, 25.1, 33.2, 36.4 },
};
static const double tap_base_gain[SS_REFL_TAPS] = { .42, .36, .32, .28, .24, .20, .17, .14 };

/* Room size 0..1 scales the tap times linearly from 0.3x to 1.6x. Taps are
 * whole slots of the decimated ring; the block mean that fills a slot is
 * centred (decim - 1) / 2 frames late, which the rounding accounts for. */
static void ss_build_taps(const ss_t * s, double room, unsigned int taps[2][SS_REFL_TAPS]) {
    double scale = 0.3 + 1.3 * room, d = (double)s->decim;
    for (int feed = 0; feed < 2; feed++)
        for (int k = 0; k < SS_REFL_TAPS; k++) {
            double frames = tap_ms[feed][k] * scale * 0.001 * (double)s->rate;
            double n = floor((frames + (d - 1.0) / 2.0) / d + 0.5);
            taps[feed][k] = n < 2.0 ? 2u : n > (double)(SS_REFL_RING - 2u) ? SS_REFL_RING - 2u
                                                                          : (unsigned int)n;
        }
}

static void ss_taps_max(ss_t * s) {
    s->maxtap = 0;
    for (int set = 0; set < 2; set++)
        for (int feed = 0; feed < 2; feed++)
            for (int k = 0; k < SS_REFL_TAPS; k++)
                if (s->taps[set][feed][k] > s->maxtap) s->maxtap = s->taps[set][feed][k];
}

/* Normalise the gains so the energy of one feed's reflections at
 * reflections == 1 is 0.25, about -6 dB against the direct sound. */
void ss_room_init(ss_t * s) {
    double energy = 0.0;
    for (int k = 0; k < SS_REFL_TAPS; k++) energy += tap_base_gain[k] * tap_base_gain[k];
    for (int k = 0; k < SS_REFL_TAPS; k++)
        s->gain[k] = (float)(tap_base_gain[k] * sqrt(0.25 / energy));
    s->decim = 1;
    while ((double)s->rate / (double)s->decim > 66000.0) s->decim *= 2;
    s->inv_d = 1.0f / (float)s->decim;
    ss_build_taps(s, s->cfg.room_size, s->taps[1]);
    memcpy(s->taps[0], s->taps[1], sizeof(s->taps[0]));
    s->room_applied = s->cfg.room_size;
    ss_taps_max(s);
    s->blending = false;
    s->blend = 1.0f;
}

/* A new room size crossfades from the old tap set over 20 ms. A change that
 * arrives mid-blend waits for it, then the latest value is used. */
void ss_room_update(ss_t * s) {
    if (s->blending || s->cfg.room_size == s->room_applied) return;
    memcpy(s->taps[0], s->taps[1], sizeof(s->taps[0]));
    ss_build_taps(s, s->cfg.room_size, s->taps[1]);
    s->room_applied = s->cfg.room_size;
    ss_taps_max(s);
    s->blending = true;
    s->blend = 0.0f;
}

/* Reflection input low-pass: 16 kHz at damping 0 down to 2 kHz at 1. */
void ss_reflections_update(ss_t * s) {
    s->built_damping = s->sm[SS_SM_DAMPING];
    double fc = fmin(16000.0 - 14000.0 * s->sm[SS_SM_DAMPING], 0.4 * (double)s->rate);
    s->lp_alpha = (float)(1.0 - exp(-2.0 * M_PI * fc / (double)s->rate));
}

void ss_head_clear(ss_t * s) {
    memset(&s->hn_hist, 0, sizeof(s->hn_hist));
    memset(&s->hf_hist, 0, sizeof(s->hf_hist));
    memset(s->dring, 0, sizeof(s->dring));
    s->head_live = false;
}

void ss_refl_clear(ss_t * s) {
    s->lp[0] = s->lp[1] = s->acc[0] = s->acc[1] = 0.0f;
    s->sm_cur = s->sd_cur = s->sm_next = s->sd_next = s->sm_step = s->sd_step = 0.0f;
    s->phase = 0;
    s->valid = 0;
}

void ss_room_clear(ss_t * s) {
    ss_refl_clear(s);
    ss_head_clear(s);
}

/* ---- Binaural head ---------------------------------------------------- */

#define HEAD_RADIUS 0.0875
#define SOUND_SPEED 343.0
#define SHADOW_AMIN 0.1
#define SHADOW_THETA_MIN 150.0

/* Brown and Duda head shadow gain alpha(theta), theta in degrees. */
static double ss_alpha(double theta_deg) {
    return (1.0 + SHADOW_AMIN / 2.0) +
           (1.0 - SHADOW_AMIN / 2.0) * cos(theta_deg / SHADOW_THETA_MIN * M_PI);
}

/* H(s) = (1 + s alpha / 2 w0) / (1 + s / 2 w0) by matched pole-zero: the
 * pole and zero land exactly at exp(-p T) and exp(-z T), gain 1 at DC. A
 * bilinear transform warps the zero of the deep far-ear shadow (alpha 0.1)
 * by over a dB at 44.1 and 48 kHz; this stays within about 0.2 dB. */
static ss_pole_t ss_shadow(double alpha, unsigned int rate) {
    double pole = 2.0 * SOUND_SPEED / HEAD_RADIUS;
    double pp = exp(-pole / (double)rate);
    double pz = exp(-pole / alpha / (double)rate);
    double gain = (1.0 - pp) / (1.0 - pz);
    return (ss_pole_t){ (float)gain, (float)(-gain * pz), (float)(-pp) };
}

/* Largest |Hn - Hf * delay| on a 96-point grid, linear in frequency from
 * about 20 Hz to the lesser of 20 kHz and 0.45 fs, evaluated from the filters
 * actually run. The delay is the integer plus linearly interpolated fraction
 * the hot path uses. Single precision with a rotating phasor, so there are no
 * trigonometric calls per point; the broad peak (around 1 to 2 kHz) is far
 * wider than a grid step of about 200 Hz. */
static float ss_head_peak(const ss_pole_t * hn, const ss_pole_t * hf, double delay,
                          unsigned int rate) {
    const float w0 = (float)(2.0 * M_PI * 20.0 / (double)rate);
    const float dw = (float)(2.0 * M_PI * (fmin(20000.0, 0.45 * (double)rate) - 20.0) /
                             (95.0 * (double)rate));
    const float n = floorf((float)delay), frac = (float)delay - n;
    float complex z1 = cosf(-w0) + I * sinf(-w0);
    const float complex step = cosf(-dw) + I * sinf(-dw);
    float complex zn = cexpf(-I * w0 * n), stepn = cexpf(-I * dw * n);
    float peak = 0.0f;
    for (int i = 0; i < 96; i++) {
        float complex a = (hn->b0 + hn->b1 * z1) / (1.0f + hn->a1 * z1);
        float complex b = (hf->b0 + hf->b1 * z1) / (1.0f + hf->a1 * z1);
        float complex y = a - b * zn * ((1.0f - frac) + frac * z1);
        float m = crealf(y) * crealf(y) + cimagf(y) * cimagf(y);
        peak = m > peak ? m : peak;
        z1 *= step;
        zn *= stepn;
    }
    return sqrtf(peak);
}

/* Near ear sees the speaker at 90 - angle from its axis, far ear at
 * 90 + angle; the far path is delayed by the Woodworth ITD. The side path Hn - Hf * delay
 * peaks above unity (about +5 dB near 1 to 2 kHz), so both filters are scaled
 * by head_gain to keep its largest gain at 1: binaural never adds level. */
void ss_head_update(ss_t * s) {
    double angle = s->sm[SS_SM_ANGLE];
    s->built_angle = angle;
    double phi = angle * M_PI / 180.0;
    double itd = HEAD_RADIUS / SOUND_SPEED * (phi + sin(phi));
    double delay = fmin(fmax(itd * (double)s->rate, 1.0), (double)(SS_HEAD_RING - 2u));
    s->hn = ss_shadow(ss_alpha(90.0 - angle), s->rate);
    s->hf = ss_shadow(ss_alpha(90.0 + angle), s->rate);
    float peak = ss_head_peak(&s->hn, &s->hf, delay, s->rate);
    s->head_gain = peak > 1.0f ? 1.0f / peak : 1.0f;
    s->hn.b0 *= s->head_gain; s->hn.b1 *= s->head_gain;
    s->hf.b0 *= s->head_gain; s->hf.b1 *= s->head_gain;
    if (!s->head_live) s->delay.cur = (float)delay;
    ss_ramp_aim(&s->delay, (float)delay, s->sub_len);
}
