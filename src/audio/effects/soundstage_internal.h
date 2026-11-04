#ifndef SOUNDSTAGE_INTERNAL_H
#define SOUNDSTAGE_INTERNAL_H

/* State shared by soundstage.c (config, lifecycle, ramps) and the DSP
 * stages in soundstage_dsp.c / soundstage_room.c. Every field of ss_t is
 * owned by the PCM thread. */
#include "soundstage.h"

#include <stdbool.h>

/* Reflections run at the stream rate divided by decim, at most 66 kHz. The
 * longest tap is 37.9 ms * 1.6 = 60.6 ms, about 4002 slots at 66 kHz; taps
 * are clamped to SS_REFL_RING - 2 in any case. */
#define SS_REFL_RING 4096u
#define SS_REFL_MASK (SS_REFL_RING - 1u)
#define SS_REFL_TAPS 8
#define SS_HEAD_RING 512u
#define SS_HEAD_MASK (SS_HEAD_RING - 1u)
#define SS_SHELF_Q 0.70710678118654752

/* Shelf: x + v, where v runs the numerator b (that is B - A) over the
 * denominator a1, a2. */
typedef struct { float b0, b1, b2, a1, a2; } ss_biquad_t;
typedef struct { float x1, x2, y1, y2; } ss_hist_t;
typedef struct { float b0, b1, a1; } ss_pole_t;
typedef struct { float x1, y1; } ss_pole_hist_t;
/* Linear ramp toward tgt, at most step per sample. */
typedef struct { float cur, tgt, step; } ss_ramp_t;

/* Parameters that drive coefficients: smoothed by a one-pole per sub-block. */
enum { SS_SM_LOW_DB, SS_SM_LOW_HZ, SS_SM_HIGH_DB, SS_SM_HIGH_HZ,
       SS_SM_ANGLE, SS_SM_DAMPING, SS_SM_COUNT };

typedef struct {
    soundstage_config_t cfg;
    unsigned int rate;
    bool s32, running, dirty, over;
    unsigned int sub_len, sub_left;
    double smooth_k, sm[SS_SM_COUNT], sm_tgt[SS_SM_COUNT];
    ss_ramp_t mix, amount, refl, side, cap, delay;

    ss_biquad_t shelf[2];
    ss_hist_t shelf_hist[2];
    bool shelf_on[2];
    float shelf_gain[2]; /* gain at each shelf's extreme, from the rounded coefficients */
    float shelf_max;     /* largest of them, at least 1 */

    float gain[SS_REFL_TAPS], lp[2], lp_alpha, comp, blend, inv_d, acc[2];
    float sm_cur, sd_cur, sm_next, sd_next, sm_step, sd_step;
    unsigned int decim, phase;
    unsigned int taps[2][2][SS_REFL_TAPS]; /* [old, new][feed][tap], decimated slots */
    unsigned int valid, wpos, maxtap;
    bool blending;
    double room_applied;
    float ring[2][SS_REFL_RING];

    ss_pole_t hn, hf;
    double built_angle, built_damping; /* smoothed values the filters were built for */
    float head_gain; /* scale on hn and hf so |Hn - Hf z^-d| <= 1 */
    ss_pole_hist_t hn_hist, hf_hist;
    float dring[SS_HEAD_RING];
    unsigned int dpos;
    bool head_live;
} ss_t;

/* Value the ramp would hold after n more samples. */
static inline float ss_ramp_peek(const ss_ramp_t * r, unsigned int n) {
    float span = r->step * (float)n;
    if (r->cur < r->tgt) return r->cur + span > r->tgt ? r->tgt : r->cur + span;
    return r->cur - span < r->tgt ? r->tgt : r->cur - span;
}

/* Move a ramp n samples ahead. Returns its value at the end; the caller
 * interpolates linearly from the old value. */
static inline float ss_ramp_advance(ss_ramp_t * r, unsigned int n) {
    r->cur = ss_ramp_peek(r, n);
    return r->cur;
}

/* Aim a ramp at tgt so it arrives in exactly n samples. */
static inline void ss_ramp_aim(ss_ramp_t * r, float tgt, unsigned int n) {
    float d = tgt - r->cur;
    r->tgt = tgt;
    r->step = (d < 0 ? -d : d) / (float)n;
}

/* Longest run of frames with constant coefficients and linear ramps. */
#define SS_SEG 32
#define SS_TINY_BITS 0x26901d7du /* bits of 1e-15f */
#define SS_BLEND_SEC 0.020f

/* soundstage_run.c: one segment of frames, 1 <= n <= SS_SEG, inside a sub-block */
void ss_run_segment(ss_t * s, float * lr, unsigned int n, float ceiling);

/* soundstage_dsp.c: mid/side EQ coefficients and shared helpers */
void ss_ms_eq_update(ss_t * s, bool moved, bool snap);
bool ss_guard_frame(ss_t * s, const float in[2], float out[2], float ceiling);
void ss_clear_state(ss_t * s);
void ss_flush_denormals(ss_t * s);
float ss_comp(float g);

/* soundstage_room.c: reflection and head coefficients */
void ss_room_init(ss_t * s);
void ss_room_update(ss_t * s);
void ss_reflections_update(ss_t * s);
void ss_head_update(ss_t * s);
void ss_room_clear(ss_t * s);
void ss_refl_clear(ss_t * s);
void ss_head_clear(ss_t * s);

#endif
