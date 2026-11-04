#include "soundstage.h"
#include "soundstage_internal.h"

#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define SS_DEFAULTS { false, 30.0, 0.0, 0.0, 0.0, 150.0, 0.0, 5000.0, 0.0, 0.5, 0.5 }
#define SS_RAMP_SEC 0.020  /* gain-like parameters */
#define SS_SUB_SEC 0.0025  /* coefficient sub-block */
#define SS_SMOOTH_SEC 0.030 /* coefficient one-pole time constant */

/* Config publication mirrors mseb.c: a plain mutex guards the small struct,
 * and the idle PCM path only reads the atomic flags. */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static soundstage_config_t requested = SS_DEFAULTS;
static atomic_bool active = false;
static atomic_bool discontinuity = false;
static ss_t st; /* PCM thread only */

const soundstage_field_t soundstage_fields[] = {
    { "speaker_angle_deg", offsetof(soundstage_config_t, speaker_angle_deg), 10, 60 },
    { "binaural_amount",   offsetof(soundstage_config_t, binaural_amount),   0, 1 },
    { "side_db",           offsetof(soundstage_config_t, side_db),           -12, 6 },
    { "side_low_db",       offsetof(soundstage_config_t, side_low_db),       -12, 6 },
    { "side_low_hz",       offsetof(soundstage_config_t, side_low_hz),       50, 500 },
    { "side_high_db",      offsetof(soundstage_config_t, side_high_db),      -12, 6 },
    { "side_high_hz",      offsetof(soundstage_config_t, side_high_hz),      2000, 12000 },
    { "reflections",       offsetof(soundstage_config_t, reflections),       0, 1 },
    { "room_size",         offsetof(soundstage_config_t, room_size),         0, 1 },
    { "damping",           offsetof(soundstage_config_t, damping),           0, 1 },
};
const size_t soundstage_field_count = sizeof soundstage_fields / sizeof soundstage_fields[0];

static bool config_valid(const soundstage_config_t * c) {
    for (size_t i = 0; i < soundstage_field_count; i++) {
        double v = *(const double *)((const char *)c + soundstage_fields[i].offset);
        if (!isfinite(v) || v < soundstage_fields[i].min || v > soundstage_fields[i].max) return false;
    }
    return true;
}

/* Nothing audible would change: no binaural, reflections or side EQ. */
static bool config_neutral(const soundstage_config_t * c) {
    return c->binaural_amount == 0 && c->reflections == 0 && c->side_db == 0 &&
           c->side_low_db == 0 && c->side_high_db == 0;
}

void soundstage_defaults(soundstage_config_t * config) {
    if (config) *config = (soundstage_config_t)SS_DEFAULTS;
}

bool soundstage_set_config(const soundstage_config_t * config) {
    if (!config || !config_valid(config)) return false;
    pthread_mutex_lock(&config_mutex);
    requested = *config;
    atomic_store_explicit(&active, config->enabled && !config_neutral(config),
                          memory_order_release);
    pthread_mutex_unlock(&config_mutex);
    return true;
}

void soundstage_get_config(soundstage_config_t * config) {
    if (!config) return;
    pthread_mutex_lock(&config_mutex);
    *config = requested;
    pthread_mutex_unlock(&config_mutex);
}

void soundstage_reset(void) {
    soundstage_config_t neutral;
    soundstage_defaults(&neutral);
    (void)soundstage_set_config(&neutral);
    soundstage_note_pcm_discontinuity();
}

void soundstage_note_pcm_discontinuity(void) {
    atomic_store_explicit(&discontinuity, true, memory_order_relaxed);
}

static void ss_set_targets(ss_t * s, const soundstage_config_t * c) {
    s->cfg = *c;
    s->mix.tgt = c->enabled ? 1.0f : 0.0f;
    s->amount.tgt = (float)c->binaural_amount;
    s->refl.tgt = (float)c->reflections;
    s->side.tgt = (float)pow(10.0, c->side_db / 20.0);
    s->sm_tgt[SS_SM_LOW_DB] = c->side_low_db;
    s->sm_tgt[SS_SM_LOW_HZ] = c->side_low_hz;
    s->sm_tgt[SS_SM_HIGH_DB] = c->side_high_db;
    s->sm_tgt[SS_SM_HIGH_HZ] = c->side_high_hz;
    s->sm_tgt[SS_SM_ANGLE] = c->speaker_angle_deg;
    s->sm_tgt[SS_SM_DAMPING] = c->damping;
}

/* One-pole step of every coefficient parameter toward its target; returns
 * true while any of them moved so coefficients are only rebuilt then. */
static bool ss_smooth(ss_t * s, bool snap) {
    bool moved = false;
    for (int i = 0; i < SS_SM_COUNT; i++) {
        double d = s->sm_tgt[i] - s->sm[i];
        if (d == 0.0) continue;
        moved = true;
        if (snap || fabs(d) < 1e-4 * fmax(1.0, fabs(s->sm_tgt[i]))) s->sm[i] = s->sm_tgt[i];
        else s->sm[i] += d * s->smooth_k;
    }
    return moved;
}

/* Start of every sub-block of about 2.5 ms: move the smoothed parameters and
 * rebuild whatever depends on them. */
static void ss_subblock(ss_t * s) {
    bool moved = ss_smooth(s, false) || s->dirty;
    bool dirty = s->dirty;
    s->dirty = false;
    ss_ms_eq_update(s, moved, dirty);
    /* The head filters and the damping low-pass are the costly rebuilds; redo
     * them only when their own parameter moved. */
    if (dirty || s->sm[SS_SM_DAMPING] != s->built_damping) ss_reflections_update(s);
    if (dirty || s->sm[SS_SM_ANGLE] != s->built_angle) ss_head_update(s);
    ss_room_update(s);
    s->sub_left = s->sub_len;
}

static void ss_set_rate(ss_t * s, unsigned int rate, bool s32) {
    float step = 1.0f / ((float)SS_RAMP_SEC * (float)rate);
    s->rate = rate;
    s->s32 = s32;
    s->sub_len = (unsigned int)(SS_SUB_SEC * (double)rate);
    if (s->sub_len < 16) s->sub_len = 16;
    s->smooth_k = 1.0 - exp(-(double)s->sub_len / (SS_SMOOTH_SEC * (double)rate));
    s->mix.step = s->amount.step = s->refl.step = step;
    s->side.step = 2.0f * step;
}

/* Begin (or restart after a rate or format change): history cleared, every
 * smoothed value snapped to its target. The enable mix is kept on a restart
 * so a format change does not click. */
static void ss_restart(ss_t * s, unsigned int rate, bool s32, bool keep_mix) {
    float mix = keep_mix ? s->mix.cur : 0.0f;
    ss_set_rate(s, rate, s32);
    (void)ss_smooth(s, true);
    s->mix.cur = mix;
    s->amount.cur = s->amount.tgt;
    s->refl.cur = s->refl.tgt;
    s->side.cur = s->side.tgt;
    s->cap.cur = s->cap.tgt = 1.0f;
    s->cap.step = 0.0f;
    ss_clear_state(s);
    ss_room_init(s);
    s->comp = ss_comp(s->refl.cur);
    s->dirty = true;
    s->sub_left = 0;
}

/* Everything the stage could still change is at its neutral value. */
static bool ss_settled_idle(const ss_t * s) {
    if (s->mix.cur == 0.0f && s->mix.tgt == 0.0f) return true;
    return config_neutral(&s->cfg) && s->amount.cur == 0.0f && s->refl.cur == 0.0f &&
           s->side.cur == 1.0f && s->cap.cur == 1.0f && s->sm[SS_SM_LOW_DB] == 0.0 &&
           s->sm[SS_SM_HIGH_DB] == 0.0;
}

bool soundstage_begin_block(unsigned int rate, int channels, bool s32, size_t frames) {
    ss_t * s = &st;
    if (channels != 2 || !frames || !rate) {
        if (s->running) ss_clear_state(s);
        s->running = false;
        return false;
    }
    if (!atomic_load_explicit(&active, memory_order_acquire) && !s->running) {
        atomic_store_explicit(&discontinuity, false, memory_order_relaxed);
        return false;
    }
    /* Never wait for the control thread: if it holds the lock, run on the
     * config already applied and look again at the next block. Before the
     * first block there is nothing applied yet, so the stage stays off. */
    soundstage_config_t config = s->cfg;
    if (pthread_mutex_trylock(&config_mutex) == 0) {
        config = requested;
        pthread_mutex_unlock(&config_mutex);
    } else if (!s->running) {
        return false;
    }
    ss_set_targets(s, &config);
    bool fresh = !s->running;
    if (fresh || s->rate != rate || s->s32 != s32) ss_restart(s, rate, s32, !fresh);
    if (!fresh && ss_settled_idle(s)) {
        ss_clear_state(s);
        s->running = false;
        return false;
    }
    if (atomic_exchange_explicit(&discontinuity, false, memory_order_relaxed))
        ss_clear_state(s);
    s->running = true;
    return true;
}

bool soundstage_frames(float * lr, size_t frames, float ceiling) {
    ss_t * s = &st;
    s->over = false;
    while (frames) {
        if (s->sub_left == 0) ss_subblock(s);
        unsigned int n = frames < SS_SEG ? (unsigned int)frames : SS_SEG;
        if (n > s->sub_left) n = s->sub_left;
        ss_run_segment(s, lr, n, ceiling);
        s->sub_left -= n;
        lr += 2 * (size_t)n;
        frames -= n;
    }
    return s->over;
}
