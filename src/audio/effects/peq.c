#include "peq.h"
#include "mseb.h"
#include "../../library/artwork/support/library_endian.h"
#include "../../core/persistence/storage_paths.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PEQ_MAX_CHANNELS 2

/* Direct Form I biquad: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
 *                              - a1*y[n-1] - a2*y[n-2]
 * (a0 already normalized to 1). float, not double -- this is the actual
 * per-sample hot path (peq_process()'s inner loop below, up to
 * PEQ_NUM_BANDS cascaded biquads per sample per channel, continuously for
 * the whole track whenever the user has any band enabled), and single-
 * precision float's ~144dB dynamic range is already far beyond 16-bit
 * audio's own ~96dB -- standard practice for audio biquad EQs (e.g. JUCE's
 * dsp module defaults to float for exactly this filter family), not a real
 * quality tradeoff on this content. compute_band_coeffs() below still does
 * its own setup math in double (pow/cos/sin, once per band per change, not
 * per sample) and only narrows to float in the one assignment into this
 * struct. */
typedef struct {
    float b0, b1, b2, a1, a2;
} biquad_coeffs_t;

typedef struct {
    float x1, x2, y1, y2;
} biquad_state_t;

static peq_band_t bands[PEQ_NUM_BANDS];
static biquad_coeffs_t coeffs[PEQ_NUM_BANDS];
static biquad_state_t state[PEQ_NUM_BANDS][PEQ_MAX_CHANNELS];
static bool last_call_was_s32 = false;
static atomic_bool bypass = false;
static double preamp_db = 0.0;
/* UI/plugin writes are atomic; the smoothed value belongs to the PCM owner. */
static _Atomic float stereo_width_target = 1.0f;
static float stereo_width_current = 1.0f;
#define STEREO_WIDTH_RAMP_SEC 0.020f
/* Crossfeed target is shared with the UI. Mix and low-pass memory stay on
 * the audio thread: writers never touch them. */
static _Atomic float crossfeed_target = 0.0f;
static float crossfeed_mix = 0.0f;
static float crossfeed_lp[PEQ_MAX_CHANNELS];
#define CROSSFEED_DELAY_RING_SIZE 512u
#define CROSSFEED_DELAY_RING_MASK (CROSSFEED_DELAY_RING_SIZE - 1u)
/* Two channels of float low-pass history: 4 KiB total. The ring is logically
 * invalidated on reset and filled from the next input when processing resumes. */
static float crossfeed_delay_ring[PEQ_MAX_CHANNELS][CROSSFEED_DELAY_RING_SIZE];
static unsigned int crossfeed_delay_write = 0;
static bool crossfeed_delay_primed = false;
static _Atomic int crossfeed_angle_target = 0;
/* These tap and blend fields are audio-thread owned. A new angle waits for an
 * in-progress 20 ms blend to finish; the latest requested angle is then used. */
static unsigned int crossfeed_tap_old = 0;
static unsigned int crossfeed_tap_target = 0;
static float crossfeed_tap_blend = 1.0f;
static unsigned int crossfeed_tap_rate = 0;
static float crossfeed_alpha = 0.0f;
static unsigned int crossfeed_coeff_rate = 0;
/* Set when the rate, PCM format, or a noted discontinuity changes. The next
 * active sample copies the current input into the low-pass memory so the
 * mix does not ring out the previous stream and does not step a steady
 * signal. */
static bool crossfeed_prime = false;
/* Audio thread sets this at a transport break. Only peq_process consumes
 * it, so UI/control threads never write the low-pass. */
static atomic_bool crossfeed_discontinuity = false;
static unsigned int coeffs_sample_rate = 0;
static bool coeffs_dirty = true;

/* Cached linear preamp factor (10^(preamp_db / 20.0)), updated whenever
 * preamp_db changes to avoid recomputing pow() during audio processing. */
static float cached_preamp_linear = 1.0f;

static void update_preamp_linear_cache(void) {
    cached_preamp_linear = (float) pow(10.0, preamp_db / 20.0);
}

/* Peak limiter -- stereo-linked (one shared gain-reduction envelope across
 * channels) so a loud transient does not shift the stereo image. Engages only
 * when the preamp/EQ stage's output would exceed full scale, leaving unboosted
 * audio unaffected. */
static float limiter_gain = 1.0f; /* current linear gain reduction, 1.0 = none */

/* One-pole envelope-follower time constants -- attack fast enough to catch
 * a sustained loud passage within a few samples (avoiding an audible "over"
 * before the limiter reacts), release slow enough that gain recovers
 * smoothly after the loud passage ends instead of audibly "pumping" on
 * every transient. Standard limiter defaults; protects manual EQ band
 * boosts too, not just the preamp slider a plugin might drive. Ceiling sits
 * a hair under true full scale (32767) so the envelope's own smoothing lag
 * has a small margin before the belt-and-suspenders clamp would ever need
 * to actually bite in normal operation. */
#define LIMITER_ATTACK_SEC 0.001
#define LIMITER_RELEASE_SEC 0.050
#define LIMITER_CEILING 32760.0f
/* Scaled ceiling for 24-bit range (2^23 = 8388608 full-scale).
 * Maintains exact same headroom ratio (32760/32768 = 8386560/8388608 = 0.999755859375,
 * i.e. 32760 * 256 = 8386560.0f). */
#define LIMITER_CEILING_S32 8386560.0f

/* Attack and release coefficients recomputed alongside biquad coefficients in
 * recompute_all_coeffs() whenever sample_rate changes to avoid redundant exp()
 * calls during audio processing. */
static float cached_attack_coeff = 0.0f;
static float cached_release_coeff = 0.0f;

/* ISO-standard 10-band graphic EQ center frequencies -- matches the layout
 * of typical hardware DAP PEQ screens (band 0 a low shelf, band 9 a high
 * shelf, the rest peaking bells). */
static const double default_freqs[PEQ_NUM_BANDS] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

static void set_defaults(bool reset_crossfeed) {
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        bands[i].freq_hz = default_freqs[i];
        bands[i].gain_db = 0.0;
        bands[i].q = (i == 0 || i == PEQ_NUM_BANDS - 1) ? 0.2 : 0.7;
        bands[i].type = (i == 0) ? PEQ_TYPE_LOW_SHELF
                       : (i == PEQ_NUM_BANDS - 1) ? PEQ_TYPE_HIGH_SHELF
                       : PEQ_TYPE_PEAKING;
        bands[i].enabled = false;
    }
    bypass = false;
    preamp_db = 0.0;
    peq_set_stereo_width(1.0);
    if (reset_crossfeed) {
        peq_set_crossfeed(0.0);
        peq_set_crossfeed_angle(0);
    }
    update_preamp_linear_cache();
}

static void compute_band_coeffs_for_band(const peq_band_t * band,
                                         unsigned int sample_rate,
                                         biquad_coeffs_t * output) {
    double freq = band->freq_hz;
    /* Clamp so w0 stays comfortably inside (0, pi) regardless of what the
     * current track's sample rate is (e.g. a 352.8kHz PCM file, where a
     * 16kHz band is nowhere near Nyquist, vs a hypothetical very
     * low sample rate where it could be). */
    double nyquist = (double) sample_rate / 2.0;
    if (freq > nyquist * 0.99) freq = nyquist * 0.99;
    if (freq < 1.0) freq = 1.0;

    double A = pow(10.0, band->gain_db / 40.0);
    double w0 = 2.0 * M_PI * freq / (double) sample_rate;
    double q = band->q > 0.01 ? band->q : 0.01;
    double cos_w0 = cos(w0);
    double sin_w0 = sin(w0);

    double b0, b1, b2, a0, a1, a2;

    if (band->type == PEQ_TYPE_LOW_SHELF || band->type == PEQ_TYPE_HIGH_SHELF) {
        /* Shelf filters parameterized directly by Q (same substitution the
         * Web Audio API's BiquadFilterNode uses for lowshelf/highshelf)
         * rather than the RBJ cookbook's alternate S/slope parameter, so
         * every band type shares the same Q control in the UI. */
        /* Clamp under_sqrt to 0 to prevent complex values and NaN when
         * calculating alpha at high Q settings. */
        double under_sqrt = (A + 1.0 / A) * (1.0 / q - 1.0) + 2.0;
        if (under_sqrt < 0.0) under_sqrt = 0.0;
        double alpha = (sin_w0 / 2.0) * sqrt(under_sqrt);
        double sqrt_A_2alpha = 2.0 * sqrt(A) * alpha;

        if (band->type == PEQ_TYPE_LOW_SHELF) {
            b0 = A * ((A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cos_w0);
            b2 = A * ((A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha);
            a0 = (A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cos_w0);
            a2 = (A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cos_w0);
            b2 = A * ((A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha);
            a0 = (A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cos_w0);
            a2 = (A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha;
        }
    } else {
        /* Peaking (bell) */
        double alpha = sin_w0 / (2.0 * q);
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha / A;
    }

    output->b0 = (float) (b0 / a0);
    output->b1 = (float) (b1 / a0);
    output->b2 = (float) (b2 / a0);
    output->a1 = (float) (a1 / a0);
    output->a2 = (float) (a2 / a0);
}

static void compute_band_coeffs(int index, unsigned int sample_rate) {
    compute_band_coeffs_for_band(&bands[index], sample_rate, &coeffs[index]);
}

void peq_get_response_db(const double * frequencies_hz, double * response_db,
                         size_t count, unsigned int sample_rate) {
    if (!frequencies_hz || !response_db || count == 0) return;
    if (sample_rate == 0) sample_rate = 48000;

    biquad_coeffs_t local_coeffs[PEQ_NUM_BANDS];
    bool active[PEQ_NUM_BANDS];
    for (int band = 0; band < PEQ_NUM_BANDS; band++) {
        active[band] = bands[band].enabled;
        if (active[band]) {
            /* Invalid persisted/user parameters should not poison the whole
             * curve. Treat that band as neutral for this read-only query. */
            const peq_band_t * p = &bands[band];
            if (!isfinite(p->freq_hz) || !isfinite(p->gain_db) || !isfinite(p->q)) {
                active[band] = false;
                continue;
            }
            compute_band_coeffs_for_band(p, sample_rate, &local_coeffs[band]);
            if (!isfinite(local_coeffs[band].b0) || !isfinite(local_coeffs[band].b1) ||
                !isfinite(local_coeffs[band].b2) || !isfinite(local_coeffs[band].a1) ||
                !isfinite(local_coeffs[band].a2)) active[band] = false;
        }
    }

    double preamp = isfinite(preamp_db) ? preamp_db : 0.0;
    if (preamp > 300.0) preamp = 300.0;
    if (preamp < -300.0) preamp = -300.0;
    const double nyquist = (double) sample_rate / 2.0;
    for (size_t i = 0; i < count; i++) {
        double f = frequencies_hz[i];
        if (!isfinite(f) || f <= 0.0) {
            response_db[i] = preamp;
            continue;
        }
        if (nyquist > 0.0 && f > nyquist * 0.99) f = nyquist * 0.99;
        double w = 2.0 * M_PI * f / (double) sample_rate;
        double c1 = cos(w), s1 = -sin(w), c2 = cos(2.0 * w), s2 = -sin(2.0 * w);
        double total = preamp;
        for (int band = 0; band < PEQ_NUM_BANDS; band++) {
            if (!active[band]) continue;
            const biquad_coeffs_t * c = &local_coeffs[band];
            double nr = c->b0 + c->b1 * c1 + c->b2 * c2;
            double ni = c->b1 * s1 + c->b2 * s2;
            double dr = 1.0 + c->a1 * c1 + c->a2 * c2;
            double di = c->a1 * s1 + c->a2 * s2;
            double numerator = nr * nr + ni * ni;
            double denominator = dr * dr + di * di;
            if (!isfinite(numerator) || !isfinite(denominator)) continue;
            double ratio = numerator / fmax(denominator, 1e-30);
            if (!isfinite(ratio) || ratio > 1e30) ratio = 1e30;
            if (ratio < 1e-30) ratio = 1e-30;
            total += 10.0 * log10(ratio);
        }
        if (!isfinite(total)) total = preamp;
        if (total > 300.0) total = 300.0;
        if (total < -300.0) total = -300.0;
        response_db[i] = total;
    }
}

static void recompute_all_coeffs(unsigned int sample_rate) {
    for (int i = 0; i < PEQ_NUM_BANDS; i++) compute_band_coeffs(i, sample_rate);
    cached_attack_coeff = (float) exp(-1.0 / (LIMITER_ATTACK_SEC * (double) sample_rate));
    cached_release_coeff = (float) exp(-1.0 / (LIMITER_RELEASE_SEC * (double) sample_rate));
    coeffs_sample_rate = sample_rate;
    coeffs_dirty = false;
}

void peq_init(void) {
    set_defaults(true);
    memset(state, 0, sizeof(state));
    peq_load();
}

void peq_reset_to_defaults(void) {
    set_defaults(true);
    coeffs_dirty = true;
}

void peq_reset_equalizer(void) {
    set_defaults(false);
    coeffs_dirty = true;
}

bool peq_get_bypass(void) { return bypass; }
void peq_set_bypass(bool b) { bypass = b; }

double peq_get_preamp_db(void) { return preamp_db; }
void peq_set_preamp_db(double db) {
    preamp_db = db;
    update_preamp_linear_cache();
}

double peq_get_stereo_width(void) {
    return atomic_load_explicit(&stereo_width_target, memory_order_relaxed);
}

void peq_set_stereo_width(double width) {
    if (!isfinite(width)) width = 1.0;
    if (width < 0.0) width = 0.0;
    if (width > 2.0) width = 2.0;
    atomic_store_explicit(&stereo_width_target, (float) width, memory_order_relaxed);
}

double peq_get_crossfeed(void) {
    return atomic_load_explicit(&crossfeed_target, memory_order_relaxed);
}

void peq_set_crossfeed(double strength) {
    if (!isfinite(strength)) strength = 0.0;
    if (strength < 0.0) strength = 0.0;
    if (strength > 1.0) strength = 1.0;
    atomic_store_explicit(&crossfeed_target, (float) strength, memory_order_relaxed);
}

int peq_get_crossfeed_angle(void) {
    return atomic_load_explicit(&crossfeed_angle_target, memory_order_relaxed);
}

void peq_set_crossfeed_angle(int degrees) {
    if (degrees != 0 && degrees != 30 && degrees != 45 && degrees != 60 && degrees != 90)
        degrees = 0;
    atomic_store_explicit(&crossfeed_angle_target, degrees, memory_order_relaxed);
}

void peq_note_pcm_discontinuity(void) {
    mseb_note_pcm_discontinuity();
    atomic_store_explicit(&crossfeed_discontinuity, true, memory_order_relaxed);
}

/* Drop crossfeed memory. Safe only on the audio thread (or before playback). */
static void crossfeed_clear_audio_state(void) {
    crossfeed_mix = 0.0f;
    crossfeed_lp[0] = 0.0f;
    crossfeed_lp[1] = 0.0f;
    crossfeed_prime = false;
    crossfeed_delay_primed = false;
    crossfeed_delay_write = 0;
    crossfeed_tap_old = 0;
    crossfeed_tap_target = 0;
    crossfeed_tap_blend = 1.0f;
    crossfeed_tap_rate = 0;
}

/* One-pole coefficient for y += alpha * (x - y), alpha = 1 - exp(-2π fc / sr).
 * exp() runs only when the sample rate changes, not per sample. A rate change
 * drops filter memory so the new coefficient cannot ring out old samples. */
static void crossfeed_update_alpha(unsigned int sample_rate) {
    if (sample_rate == 0) sample_rate = 48000;
    if (crossfeed_coeff_rate == sample_rate && crossfeed_alpha > 0.0f) return;
    if (crossfeed_coeff_rate != 0 && crossfeed_coeff_rate != sample_rate) {
        crossfeed_lp[0] = 0.0f;
        crossfeed_lp[1] = 0.0f;
        crossfeed_prime = true;
        crossfeed_delay_primed = false;
        crossfeed_tap_rate = 0;
    }
    double decay = exp(-2.0 * M_PI * PEQ_CROSSFEED_FC_HZ / (double) sample_rate);
    crossfeed_alpha = (float) (1.0 - decay);
    crossfeed_coeff_rate = sample_rate;
}

static unsigned int crossfeed_angle_delay_us(int angle) {
    switch (angle) {
        case 30: return 261;
        case 45: return 381;
        case 60: return 488;
        case 90: return 655;
        default: return 0;
    }
}

/* Set a nearest-sample delay for the selected ITD. While a blend is underway,
 * keep its endpoints; the latest requested angle is picked up once it ends. */
static void crossfeed_update_tap(unsigned int sample_rate) {
    if (sample_rate == 0) sample_rate = 48000;
    int angle = peq_get_crossfeed_angle();
    double delay = (double) crossfeed_angle_delay_us(angle) * (double) sample_rate / 1000000.0;
    unsigned int wanted = (unsigned int) floor(delay + 0.5);
    if (wanted > CROSSFEED_DELAY_RING_MASK) wanted = CROSSFEED_DELAY_RING_MASK;
    if (crossfeed_tap_rate != sample_rate) {
        /* The history is re-primed on a rate change, so adopt the matching
         * sample-rate tap immediately instead of blending against old units. */
        crossfeed_tap_rate = sample_rate;
        crossfeed_tap_old = wanted;
        crossfeed_tap_target = wanted;
        crossfeed_tap_blend = 1.0f;
        return;
    }
    if (crossfeed_tap_blend < 1.0f || wanted == crossfeed_tap_target) return;
    crossfeed_tap_old = crossfeed_tap_target;
    crossfeed_tap_target = wanted;
    crossfeed_tap_blend = 0.0f;
}

static float crossfeed_read_delay(int channel, unsigned int tap) {
    unsigned int index = (crossfeed_delay_write - tap) & CROSSFEED_DELAY_RING_MASK;
    return crossfeed_delay_ring[channel][index];
}

/* Drop a pending break without arming a prime. Bypass and an idle buffer
 * already zero the low-pass; leaving the flag set would prime later. */
static void crossfeed_drop_discontinuity(void) {
    atomic_store_explicit(&crossfeed_discontinuity, false, memory_order_relaxed);
}

/* Consume a break noted by the audio thread. Armed only while crossfeed
 * will run, so the next sample adopts the new stream instead of the old
 * low-pass. */
static void crossfeed_consume_discontinuity(bool cross_active) {
    if (!atomic_exchange_explicit(&crossfeed_discontinuity, false, memory_order_relaxed))
        return;
    crossfeed_lp[0] = 0.0f;
    crossfeed_lp[1] = 0.0f;
    crossfeed_delay_primed = false;
    if (cross_active) crossfeed_prime = true;
}

/* Strength s maps to k = s * PEQ_CROSSFEED_MAX_BLEND.
 * yL = xL + k * (lpR - lpL), with lp the one-pole low-pass of that channel
 * including the current sample. With a non-zero angle the opposite low-pass
 * is delayed before mixing. No-delay mono remains exact; delayed mono can
 * change phase/response, while steady DC stays at unity. k == 0 skips it.
 *
 * ceiling is the immediate stereo-linked peak limit used only while this
 * mix is active. A reversal can reach about 1+2k times the input before
 * the 1 ms limiter moves. Gain is ceiling/peak, or the pre-crossfeed peak
 * when that peak is already over the ceiling, so preamp headroom still
 * belongs to the existing limiter. Signals that do not exceed the ceiling
 * are left unchanged. */
static void crossfeed_frame(float * samples, float target, float step, float ceiling) {
    if (crossfeed_mix != target) {
        if (crossfeed_mix < target)
            crossfeed_mix = fminf(target, crossfeed_mix + step);
        else
            crossfeed_mix = fmaxf(target, crossfeed_mix - step);
    }
    float k = crossfeed_mix * (float) PEQ_CROSSFEED_MAX_BLEND;
    if (k == 0.0f) {
        crossfeed_lp[0] = 0.0f;
        crossfeed_lp[1] = 0.0f;
        crossfeed_delay_primed = false;
        return;
    }
    float x0 = samples[0];
    float x1 = samples[1];
    if (crossfeed_prime) {
        crossfeed_lp[0] = x0;
        crossfeed_lp[1] = x1;
        crossfeed_prime = false;
    } else {
        crossfeed_lp[0] += crossfeed_alpha * (x0 - crossfeed_lp[0]);
        crossfeed_lp[1] += crossfeed_alpha * (x1 - crossfeed_lp[1]);
    }
    if (!crossfeed_delay_primed) {
        for (unsigned int i = 0; i < CROSSFEED_DELAY_RING_SIZE; i++) {
            crossfeed_delay_ring[0][i] = crossfeed_lp[0];
            crossfeed_delay_ring[1][i] = crossfeed_lp[1];
        }
        crossfeed_delay_write = 0;
        crossfeed_delay_primed = true;
    }
    crossfeed_delay_ring[0][crossfeed_delay_write] = crossfeed_lp[0];
    crossfeed_delay_ring[1][crossfeed_delay_write] = crossfeed_lp[1];
    if (crossfeed_tap_blend < 1.0f) {
        crossfeed_tap_blend = fminf(1.0f, crossfeed_tap_blend + step);
        if (crossfeed_tap_blend >= 1.0f)
            crossfeed_tap_old = crossfeed_tap_target;
    }
    float lp_right = crossfeed_read_delay(1, crossfeed_tap_target);
    float lp_left = crossfeed_read_delay(0, crossfeed_tap_target);
    if (crossfeed_tap_blend < 1.0f) {
        float old_right = crossfeed_read_delay(1, crossfeed_tap_old);
        float old_left = crossfeed_read_delay(0, crossfeed_tap_old);
        lp_right = old_right + crossfeed_tap_blend * (lp_right - old_right);
        lp_left = old_left + crossfeed_tap_blend * (lp_left - old_left);
    }
    float y0 = x0 + k * (lp_right - crossfeed_lp[0]);
    float y1 = x1 + k * (lp_left - crossfeed_lp[1]);
    crossfeed_delay_write = (crossfeed_delay_write + 1u) & CROSSFEED_DELAY_RING_MASK;
    if (!isfinite(y0) || !isfinite(y1)) {
        crossfeed_lp[0] = 0.0f;
        crossfeed_lp[1] = 0.0f;
        crossfeed_delay_primed = false;
        return;
    }
    float in_peak = fabsf(x0);
    float in_other = fabsf(x1);
    if (in_other > in_peak) in_peak = in_other;
    float out_peak = fabsf(y0);
    float out_other = fabsf(y1);
    if (out_other > out_peak) out_peak = out_other;
    if (out_peak > ceiling && out_peak > in_peak) {
        float allowed = in_peak > ceiling ? in_peak : ceiling;
        if (out_peak > allowed) {
            float gain = allowed / out_peak;
            y0 *= gain;
            y1 *= gain;
        }
    }
    samples[0] = y0;
    samples[1] = y1;
}

/* Normalized mid/side matrix. For width > 1 the reciprocal provides
 * headroom: the absolute coefficients sum to one, so widening cannot
 * increase a frame's peak. The center becomes quieter when widened.
 * Unity is an exact no-op; mono buffers never enter this matrix. */
static void stereo_width_coeffs(float width, float * direct, float * cross) {
    if (width > 1.0f) {
        float inverse = 1.0f / width;
        *direct = (1.0f + inverse) * 0.5f;
        *cross = (inverse - 1.0f) * 0.5f;
    } else {
        *direct = (1.0f + width) * 0.5f;
        *cross = (1.0f - width) * 0.5f;
    }
}

static void stereo_width_frame(float * samples, float target, float step,
                                float * direct, float * cross) {
    if (stereo_width_current != target) {
        if (stereo_width_current < target)
            stereo_width_current = fminf(target, stereo_width_current + step);
        else
            stereo_width_current = fmaxf(target, stereo_width_current - step);
        stereo_width_coeffs(stereo_width_current, direct, cross);
    }
    if (stereo_width_current == 1.0f) return;
    float left = samples[0], right = samples[1];
    samples[0] = left * *direct + right * *cross;
    samples[1] = right * *direct + left * *cross;
}

const peq_band_t * peq_get_band(int index) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return NULL;
    return &bands[index];
}

void peq_set_band(int index, double freq_hz, double gain_db, double q) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].freq_hz = freq_hz;
    bands[index].gain_db = gain_db;
    bands[index].q = q;
    coeffs_dirty = true;
}

void peq_set_band_type(int index, peq_band_type_t type) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].type = type;
    coeffs_dirty = true;
}

void peq_set_band_enabled(int index, bool enabled) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].enabled = enabled;
}

/* The manual filters must not resume with histories from before MSEB took
 * ownership of tone processing. Crossfeed remains independent, but primes
 * its history against the new signal at this processing boundary. */
static void sync_mseb_ownership(bool active) {
    static bool previous;
    if (active != previous) {
        memset(state, 0, sizeof(state));
        limiter_gain = 1.0f;
        atomic_store_explicit(&crossfeed_discontinuity, true, memory_order_relaxed);
    }
    previous = active;
}

void peq_process(int16_t * buf, size_t frame_count, int channels, unsigned int sample_rate) {
    const bool mseb_active = mseb_process(buf, frame_count, channels, sample_rate);
    sync_mseb_ownership(mseb_active);
    const bool eq_off = mseb_active || atomic_load_explicit(&bypass, memory_order_relaxed);
    /* Bypass disables the EQ controls and stereo width, while Crossfeed has
     * its own setting and remains available. Keep this early return when no
     * Crossfeed is active so an inactive signal stays bit-perfect. */
    float cross_target = (float) peq_get_crossfeed();
    bool cross_active = channels == 2 && (cross_target != 0.0f || crossfeed_mix != 0.0f);
    if (eq_off && !cross_active) {
        limiter_gain = 1.0f;
        stereo_width_current = 1.0f;
        crossfeed_clear_audio_state();
        crossfeed_drop_discontinuity();
        return;
    }

    float width = eq_off ? 1.0f : (float) peq_get_stereo_width();
    bool width_active = channels == 2 && (width != 1.0f || stereo_width_current != 1.0f);
    if (eq_off) stereo_width_current = 1.0f;
    if (!cross_active) crossfeed_clear_audio_state();
    crossfeed_consume_discontinuity(cross_active);
    bool any_enabled = false;
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        if (!eq_off && bands[i].enabled) { any_enabled = true; break; }
    }
    if (!any_enabled && (eq_off || preamp_db == 0.0) && !width_active && !cross_active) {
        limiter_gain = 1.0f;
        return;
    }

    if (last_call_was_s32) {
        memset(state, 0, sizeof(state));
        crossfeed_lp[0] = 0.0f;
        crossfeed_lp[1] = 0.0f;
        crossfeed_delay_primed = false;
        if (cross_active) crossfeed_prime = true;
        last_call_was_s32 = false;
    }

    if (coeffs_dirty || sample_rate != coeffs_sample_rate) recompute_all_coeffs(sample_rate);

    if (channels < 1) return;
    if (channels > PEQ_MAX_CHANNELS) channels = PEQ_MAX_CHANNELS;

    float preamp_linear = eq_off ? 1.0f : cached_preamp_linear;

    float attack_coeff = cached_attack_coeff;
    float release_coeff = cached_release_coeff;

    float width_direct, width_cross;
    stereo_width_coeffs(stereo_width_current, &width_direct, &width_cross);
    unsigned int rate_for_ramp = sample_rate ? sample_rate : 48000;
    float width_step = 2.0f / (STEREO_WIDTH_RAMP_SEC * (float) rate_for_ramp);
    float cross_step = 1.0f / ((float) PEQ_CROSSFEED_RAMP_SEC * (float) rate_for_ramp);
    if (cross_active) {
        crossfeed_update_alpha(sample_rate);
        crossfeed_update_tap(sample_rate);
    }
    for (size_t i = 0; i < frame_count; i++) {
        float frame_samples[PEQ_MAX_CHANNELS];
        float frame_peak = 0.0f;

        for (int ch = 0; ch < channels; ch++) {
            float sample = (float) buf[i * (size_t) channels + (size_t) ch] * preamp_linear;

            for (int band = 0; band < PEQ_NUM_BANDS; band++) {
                if (eq_off || !bands[band].enabled) continue;

                biquad_coeffs_t * c = &coeffs[band];
                biquad_state_t * s = &state[band][ch];

                float x0 = sample;
                float y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
                         - c->a1 * s->y1 - c->a2 * s->y2;

                s->x2 = s->x1;
                s->x1 = x0;
                s->y2 = s->y1;
                s->y1 = y0;

                sample = y0;
            }

            /* Discard non-finite samples to prevent NaN or Inf from poisoning
             * the limiter gain or cascading through biquad filters. */
            if (!isfinite(sample)) sample = 0.0f;
            frame_samples[ch] = sample;
        }
        /* Crossfeed, then width, then the limiter below. Crossfeed is a
         * frequency-dependent opposite-channel mix; width is a broadband
         * mid/side matrix. The limiter sees both. */
        if (cross_active)
            crossfeed_frame(frame_samples, cross_target, cross_step, LIMITER_CEILING);
        if (width_active)
            stereo_width_frame(frame_samples, width, width_step, &width_direct, &width_cross);
        for (int ch = 0; ch < channels; ch++) {
            float abs_sample = fabsf(frame_samples[ch]);
            if (abs_sample > frame_peak) frame_peak = abs_sample;
        }

        /* Stereo-linked gain reduction: both channels of this frame share
         * one envelope, driven by whichever channel is louder, so a hard-
         * panned transient doesn't tug the gain (and therefore the
         * perceived stereo image) unevenly between L/R. */
        float desired_gain = frame_peak > LIMITER_CEILING ? LIMITER_CEILING / frame_peak : 1.0f;
        float coeff = desired_gain < limiter_gain ? attack_coeff : release_coeff;
        limiter_gain = coeff * limiter_gain + (1.0f - coeff) * desired_gain;

        for (int ch = 0; ch < channels; ch++) {
            float sample = frame_samples[ch] * limiter_gain;
            /* Hard clamp to prevent clipping on single-sample transients that
             * occur before the limiter envelope reacts. */
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            buf[i * (size_t) channels + (size_t) ch] = (int16_t) sample;
        }
    }
}

void peq_process_s32(int32_t * buf, size_t frame_count, int channels, unsigned int sample_rate) {
    const bool mseb_active = mseb_process_s32(buf, frame_count, channels, sample_rate);
    sync_mseb_ownership(mseb_active);
    const bool eq_off = mseb_active || atomic_load_explicit(&bypass, memory_order_relaxed);
    float cross_target = (float) peq_get_crossfeed();
    bool cross_active = channels == 2 && (cross_target != 0.0f || crossfeed_mix != 0.0f);
    if (eq_off && !cross_active) {
        limiter_gain = 1.0f;
        stereo_width_current = 1.0f;
        crossfeed_clear_audio_state();
        crossfeed_drop_discontinuity();
        return;
    }

    float width = eq_off ? 1.0f : (float) peq_get_stereo_width();
    bool width_active = channels == 2 && (width != 1.0f || stereo_width_current != 1.0f);
    if (eq_off) stereo_width_current = 1.0f;
    if (!cross_active) crossfeed_clear_audio_state();
    crossfeed_consume_discontinuity(cross_active);
    bool any_enabled = false;
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        if (!eq_off && bands[i].enabled) { any_enabled = true; break; }
    }
    if (!any_enabled && (eq_off || preamp_db == 0.0) && !width_active && !cross_active) {
        limiter_gain = 1.0f;
        return;
    }

    if (!last_call_was_s32) {
        memset(state, 0, sizeof(state));
        crossfeed_lp[0] = 0.0f;
        crossfeed_lp[1] = 0.0f;
        crossfeed_delay_primed = false;
        if (cross_active) crossfeed_prime = true;
        last_call_was_s32 = true;
    }

    if (coeffs_dirty || sample_rate != coeffs_sample_rate) recompute_all_coeffs(sample_rate);

    if (channels < 1) return;
    if (channels > PEQ_MAX_CHANNELS) channels = PEQ_MAX_CHANNELS;

    float preamp_linear = eq_off ? 1.0f : cached_preamp_linear;
    float attack_coeff = cached_attack_coeff;
    float release_coeff = cached_release_coeff;

    float width_direct, width_cross;
    stereo_width_coeffs(stereo_width_current, &width_direct, &width_cross);
    unsigned int rate_for_ramp = sample_rate ? sample_rate : 48000;
    float width_step = 2.0f / (STEREO_WIDTH_RAMP_SEC * (float) rate_for_ramp);
    float cross_step = 1.0f / ((float) PEQ_CROSSFEED_RAMP_SEC * (float) rate_for_ramp);
    if (cross_active) {
        crossfeed_update_alpha(sample_rate);
        crossfeed_update_tap(sample_rate);
    }
    for (size_t i = 0; i < frame_count; i++) {
        float frame_samples[PEQ_MAX_CHANNELS];
        float frame_peak = 0.0f;

        for (int ch = 0; ch < channels; ch++) {
            float sample = (float) buf[i * (size_t) channels + (size_t) ch] * preamp_linear;

            for (int band = 0; band < PEQ_NUM_BANDS; band++) {
                if (eq_off || !bands[band].enabled) continue;

                biquad_coeffs_t * c = &coeffs[band];
                biquad_state_t * s = &state[band][ch];

                float x0 = sample;
                float y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
                         - c->a1 * s->y1 - c->a2 * s->y2;

                s->x2 = s->x1;
                s->x1 = x0;
                s->y2 = s->y1;
                s->y1 = y0;

                sample = y0;
            }

            if (!isfinite(sample)) sample = 0.0f;
            frame_samples[ch] = sample;
        }
        if (cross_active)
            crossfeed_frame(frame_samples, cross_target, cross_step, LIMITER_CEILING_S32);
        if (width_active)
            stereo_width_frame(frame_samples, width, width_step, &width_direct, &width_cross);
        for (int ch = 0; ch < channels; ch++) {
            float abs_sample = fabsf(frame_samples[ch]);
            if (abs_sample > frame_peak) frame_peak = abs_sample;
        }

        float desired_gain = frame_peak > LIMITER_CEILING_S32 ? LIMITER_CEILING_S32 / frame_peak : 1.0f;
        float coeff = desired_gain < limiter_gain ? attack_coeff : release_coeff;
        limiter_gain = coeff * limiter_gain + (1.0f - coeff) * desired_gain;

        for (int ch = 0; ch < channels; ch++) {
            float sample = frame_samples[ch] * limiter_gain;
            if (sample > 8388607.0f) sample = 8388607.0f;
            if (sample < -8388608.0f) sample = -8388608.0f;
            buf[i * (size_t) channels + (size_t) ch] = (int32_t) sample;
        }
    }
}

bool peq_load_from_path(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return false;

    double loaded_width = 1.0; /* Older profiles are neutral, never inherit width. */
    double loaded_crossfeed = 0.0; /* Older profiles are off, never inherit crossfeed. */
    int loaded_crossfeed_angle = 0; /* Older profiles keep legacy zero delay. */
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * key = line;
        const char * value = eq + 1;

        if (strcmp(key, "bypass") == 0) {
            bypass = (strcmp(value, "1") == 0);
            continue;
        }
        if (strcmp(key, "preamp") == 0) {
            preamp_db = atof(value);
            update_preamp_linear_cache();
            continue;
        }

        if (strcmp(key, "stereo_width") == 0) {
            char * end;
            double parsed = strtod(value, &end);
            bool parsed_number = end != value;
            while (*end == ' ' || *end == '\t' || *end == '\r') end++;
            if (parsed_number && *end == '\0' && isfinite(parsed)) loaded_width = parsed;
            continue;
        }

        if (strcmp(key, "crossfeed") == 0) {
            char * end;
            double parsed = strtod(value, &end);
            bool parsed_number = end != value;
            while (*end == ' ' || *end == '\t' || *end == '\r') end++;
            if (parsed_number && *end == '\0' && isfinite(parsed)) loaded_crossfeed = parsed;
            continue;
        }

        if (strcmp(key, "crossfeed_angle") == 0) {
            char * end;
            errno = 0;
            long parsed = strtol(value, &end, 10);
            bool parsed_number = end != value && errno != ERANGE;
            while (*end == ' ' || *end == '\t' || *end == '\r') end++;
            if (parsed_number && *end == '\0' &&
                (parsed == 0 || parsed == 30 || parsed == 45 || parsed == 60 || parsed == 90))
                loaded_crossfeed_angle = (int) parsed;
            else
                loaded_crossfeed_angle = 0;
            continue;
        }

        int index;
        char field[16];
        if (sscanf(key, "band%d_%15s", &index, field) == 2 && index >= 0 && index < PEQ_NUM_BANDS) {
            if (strcmp(field, "freq") == 0) bands[index].freq_hz = atof(value);
            else if (strcmp(field, "gain") == 0) bands[index].gain_db = atof(value);
            else if (strcmp(field, "q") == 0) bands[index].q = atof(value);
            else if (strcmp(field, "type") == 0) bands[index].type = (peq_band_type_t) atoi(value);
            else if (strcmp(field, "enabled") == 0) bands[index].enabled = (strcmp(value, "1") == 0);
        }
    }

    fclose(f);
    peq_set_stereo_width(loaded_width);
    peq_set_crossfeed(loaded_crossfeed);
    peq_set_crossfeed_angle(loaded_crossfeed_angle);
    coeffs_dirty = true;
    return true;
}

/* rename() alone is not durable against an unclean shutdown/crash -- the
 * directory entry update needs its own fsync, same requirement every other
 * atomic-write path in this codebase already handles (settings.c's
 * fsync_settings_dir(), library_fsync_dir()'s other callers). Generic over
 * the parent directory rather than a fixed path since peq_save_to_path()
 * writes both the always-current autosave file (/usr/data) and named
 * profiles (wherever the caller's directory lives). Best-effort: a failure
 * here does not fail the save the caller already completed and reported. */
static void peq_fsync_parent_dir(const char * path) {
    char dir[520];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) slash[1] = '\0'; /* "/name" -> "/" */
    else *slash = '\0';
    library_fsync_dir(dir);
}

bool peq_save_to_path(const char * path) {
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE * f = fopen(tmp_path, "w");
    if (!f) return false;

    fprintf(f, "bypass=%d\n", bypass ? 1 : 0);
    fprintf(f, "preamp=%.2f\n", preamp_db);
    fprintf(f, "stereo_width=%.6f\n", peq_get_stereo_width());
    fprintf(f, "crossfeed=%.6f\n", peq_get_crossfeed());
    fprintf(f, "crossfeed_angle=%d\n", peq_get_crossfeed_angle());
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        fprintf(f, "band%d_freq=%.2f\n", i, bands[i].freq_hz);
        fprintf(f, "band%d_gain=%.2f\n", i, bands[i].gain_db);
        fprintf(f, "band%d_q=%.3f\n", i, bands[i].q);
        fprintf(f, "band%d_type=%d\n", i, (int) bands[i].type);
        fprintf(f, "band%d_enabled=%d\n", i, bands[i].enabled ? 1 : 0);
    }

    /* Ensure stdio buffers and SD card writes have completed before reporting success. */
    bool write_ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) write_ok = false;
    if (!write_ok) {
        fprintf(stderr, "peq: failed writing '%s': %s\n", tmp_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    /* Rename temp file to target. On filesystems where atomic overwrite via
     * rename() is not supported when target exists, fall back to staging a
     * backup copy and restoring it if installation fails. */
    if (rename(tmp_path, path) == 0) {
        peq_fsync_parent_dir(path);
        return true;
    }
    int direct_rename_errno = errno;
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "peq: rename '%s' -> '%s' failed: %s\n", tmp_path, path, strerror(direct_rename_errno));
        unlink(tmp_path);
        return false;
    }

    char backup_path[520];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", path);
    unlink(backup_path); /* stale backup from an interrupted older attempt */
    if (rename(path, backup_path) != 0) {
        fprintf(stderr, "peq: could not stage existing profile '%s': %s\n", path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        int install_errno = errno;
        if (rename(backup_path, path) != 0)
            fprintf(stderr, "peq: CRITICAL: rollback '%s' failed: %s\n", path, strerror(errno));
        fprintf(stderr, "peq: could not install replacement '%s': %s\n", path, strerror(install_errno));
        unlink(tmp_path);
        return false;
    }
    unlink(backup_path);
    peq_fsync_parent_dir(path);
    return true;
}

void peq_load(void) {
    peq_load_from_path(PEQ_FILE_PATH);
}

void peq_save(void) {
    peq_save_to_path(PEQ_FILE_PATH);
}

static bool is_peq_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcmp(ext, ".peq") == 0;
}

static int compare_paths(const void * a, const void * b) {
    const char * const * pa = (const char * const *) a;
    const char * const * pb = (const char * const *) b;
    return strcmp(*pa, *pb);
}

/* Flat (non-recursive) scan, unlike text_reader.c's recursive one -- named
 * profiles are meant to sit directly in one dedicated folder the user
 * manages, not scattered across the whole SD card. Caller owns *out_paths
 * (free each entry, then the array). Returns false if root can't be read
 * (including "doesn't exist yet" -- see peq_profiles_dir_ensure()) or has
 * no .peq files. */
bool peq_scan_profiles(const char * root, char *** out_paths, int * out_count) {
    DIR * dir = opendir(root);
    if (!dir) return false;

    char ** paths = NULL;
    int count = 0;
    int capacity = 0;

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_peq_file(de->d_name)) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", root, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            paths = realloc(paths, sizeof(char *) * (size_t) capacity);
        }
        paths[count] = strdup(full_path);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free(paths);
        return false;
    }

    qsort(paths, (size_t) count, sizeof(char *), compare_paths);
    *out_paths = paths;
    *out_count = count;
    return true;
}
