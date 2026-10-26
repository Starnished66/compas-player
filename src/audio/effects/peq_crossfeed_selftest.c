/* Host checks for headphone crossfeed. The reference below repeats the
 * documented one-pole mix in float so a mismatch fails exactly. */
#include "peq.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void idle(void) {
    int16_t z[2] = {0};
    peq_reset_to_defaults();
    peq_set_bypass(true);
    peq_process(z, 1, 2, 48000);
    peq_set_bypass(false);
    /* An enabled band forces the PCM path so the 16-bit format flag is
     * current and a later 24-bit call is a real format change. Crossfeed
     * stays off, so this does not prime its filter. */
    peq_set_band_enabled(0, true);
    peq_process(z, 1, 2, 48000);
    peq_reset_to_defaults();
    peq_set_bypass(true);
    peq_process(z, 1, 2, 48000);
    peq_set_bypass(false);
}

static size_t ramp_frames(unsigned int rate) {
    if (rate == 0) rate = 48000;
    return (size_t) ((double) rate * PEQ_CROSSFEED_RAMP_SEC) + 8;
}

static void preroll(unsigned int rate) {
    size_t n = ramp_frames(rate);
    int16_t * silence = calloc(n * 2, sizeof(int16_t));
    assert(silence);
    peq_process(silence, n, 2, rate);
    free(silence);
}

static float one_pole_alpha(unsigned int rate) {
    if (rate == 0) rate = 48000;
    return (float) (1.0 - exp(-2.0 * M_PI * PEQ_CROSSFEED_FC_HZ / (double) rate));
}

static int16_t clamp16(float sample) {
    if (sample > 32767.0f) sample = 32767.0f;
    if (sample < -32768.0f) sample = -32768.0f;
    return (int16_t) sample;
}

/* Reference chain: crossfeed, then width. Limiter stays at unity for the
 * levels these checks use. Float operations match peq.c. */
static void reference_s16(const int16_t * in, int16_t * out, size_t frames,
                          unsigned int rate, float cross_target, float width_target) {
    float mix = 0.0f;
    float lp0 = 0.0f, lp1 = 0.0f;
    float width = 1.0f;
    float alpha = one_pole_alpha(rate);
    float cross_step = 1.0f / ((float) PEQ_CROSSFEED_RAMP_SEC * (float) (rate ? rate : 48000));
    float width_step = 2.0f / (0.020f * (float) (rate ? rate : 48000));
    for (size_t i = 0; i < frames; i++) {
        float x0 = in[i * 2];
        float x1 = in[i * 2 + 1];
        if (mix < cross_target) mix = fminf(cross_target, mix + cross_step);
        else if (mix > cross_target) mix = fmaxf(cross_target, mix - cross_step);
        float k = mix * (float) PEQ_CROSSFEED_MAX_BLEND;
        if (k == 0.0f) {
            lp0 = lp1 = 0.0f;
        } else {
            lp0 += alpha * (x0 - lp0);
            lp1 += alpha * (x1 - lp1);
            x0 = x0 + k * (lp1 - lp0);
            x1 = x1 + k * (lp0 - lp1);
        }
        if (width < width_target) width = fminf(width_target, width + width_step);
        else if (width > width_target) width = fmaxf(width_target, width - width_step);
        if (width != 1.0f) {
            float direct, cross;
            if (width > 1.0f) {
                float inverse = 1.0f / width;
                direct = (1.0f + inverse) * 0.5f;
                cross = (inverse - 1.0f) * 0.5f;
            } else {
                direct = (1.0f + width) * 0.5f;
                cross = (1.0f - width) * 0.5f;
            }
            float left = x0, right = x1;
            x0 = left * direct + right * cross;
            x1 = right * direct + left * cross;
        }
        out[i * 2] = clamp16(x0);
        out[i * 2 + 1] = clamp16(x1);
    }
}

static void test_defaults_and_setter(void) {
    idle();
    assert(peq_get_crossfeed() == 0.0);
    assert(peq_get_crossfeed_angle() == 0);
    assert(PEQ_CROSSFEED_SUGGESTED > 0.0 && PEQ_CROSSFEED_SUGGESTED < 1.0);
    peq_set_crossfeed(PEQ_CROSSFEED_SUGGESTED);
    assert(fabs(peq_get_crossfeed() - PEQ_CROSSFEED_SUGGESTED) < 1e-6);
    peq_set_crossfeed(NAN);
    assert(peq_get_crossfeed() == 0.0);
    peq_set_crossfeed(INFINITY);
    assert(peq_get_crossfeed() == 0.0);
    peq_set_crossfeed(-0.2);
    assert(peq_get_crossfeed() == 0.0);
    peq_set_crossfeed(1.4);
    assert(peq_get_crossfeed() == 1.0);
    peq_reset_to_defaults();
    assert(peq_get_crossfeed() == 0.0);
    assert(peq_get_crossfeed_angle() == 0);
    assert(fabs(peq_get_stereo_width() - 1.0) < 1e-6);
}

static unsigned int expected_crossfeed_tap(unsigned int rate, int angle) {
    static const int angles[] = { 0, 30, 45, 60, 90 };
    static const unsigned int delays_us[] = { 0, 261, 381, 488, 655 };
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        if (angles[i] == angle)
            return (unsigned int) floor((double) delays_us[i] * rate / 1000000.0 + 0.5);
    }
    assert(0 && "unsupported test angle");
    return 0;
}

static void test_angle_impulse_delay_s16(unsigned int rate, int angle) {
    size_t pulse = ramp_frames(rate) + 2;
    size_t frames = pulse + expected_crossfeed_tap(rate, angle) + 8;
    int16_t * pcm = calloc(frames * 2, sizeof(*pcm));
    assert(pcm);
    pcm[pulse * 2] = 12000;

    idle();
    peq_set_crossfeed(1.0);
    peq_set_crossfeed_angle(angle);
    peq_process(pcm, frames, 2, rate);

    size_t first = 0;
    while (first < frames && pcm[first * 2 + 1] == 0) first++;
    assert(first == pulse + expected_crossfeed_tap(rate, angle));
    free(pcm);
}

static void test_angle_impulse_delay_s32(unsigned int rate, int angle) {
    size_t pulse = ramp_frames(rate) + 2;
    size_t frames = pulse + expected_crossfeed_tap(rate, angle) + 8;
    int32_t * pcm = calloc(frames * 2, sizeof(*pcm));
    assert(pcm);
    pcm[pulse * 2] = 3000000;

    idle();
    peq_set_crossfeed(1.0);
    peq_set_crossfeed_angle(angle);
    peq_process_s32(pcm, frames, 2, rate);

    size_t first = 0;
    while (first < frames && pcm[first * 2 + 1] == 0) first++;
    assert(first == pulse + expected_crossfeed_tap(rate, angle));
    free(pcm);
}

static void test_angle_impulse_delays(void) {
    static const unsigned int rates[] = { 44100, 48000, 192000 };
    static const int angles[] = { 0, 30, 45, 60, 90 };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        for (size_t a = 0; a < sizeof angles / sizeof angles[0]; a++) {
            test_angle_impulse_delay_s16(rates[r], angles[a]);
            test_angle_impulse_delay_s32(rates[r], angles[a]);
        }
    }
}

static void test_disabled_and_mono_identity(void) {
    idle();
    peq_set_crossfeed_angle(90);
    int16_t stereo[] = {0, 0, 1, -1, 32767, -32768, -32768, 32767, 1234, -5678};
    int16_t copy[10];
    memcpy(copy, stereo, sizeof stereo);
    peq_process(stereo, 5, 2, 48000);
    assert(memcmp(stereo, copy, sizeof stereo) == 0);

    int32_t wide[] = {8388607, -8388608, -8388608, 8388607, 40000, -40000};
    int32_t wide_copy[6];
    memcpy(wide_copy, wide, sizeof wide);
    peq_process_s32(wide, 3, 2, 96000);
    assert(memcmp(wide, wide_copy, sizeof wide) == 0);

    peq_set_crossfeed(1.0);
    int16_t mono[] = {32767, -32768, 0, 1, -1};
    int16_t mono_copy[5];
    memcpy(mono_copy, mono, sizeof mono);
    peq_process(mono, 5, 1, 44100);
    assert(memcmp(mono, mono_copy, sizeof mono) == 0);
    int32_t mono32[] = {8388607, -8388608, 100000};
    int32_t mono32_copy[3];
    memcpy(mono32_copy, mono32, sizeof mono32);
    peq_process_s32(mono32, 3, 1, 48000);
    assert(memcmp(mono32, mono32_copy, sizeof mono32) == 0);

    /* Mono clears stereo filter memory. Silence afterwards stays silent. */
    peq_set_crossfeed(1.0);
    preroll(48000);
    int16_t hot[2] = {20000, -20000};
    peq_process(hot, 1, 2, 48000);
    int16_t one[1] = {0};
    peq_process(one, 1, 1, 48000);
    int16_t silence[8] = {0};
    peq_process(silence, 4, 2, 48000);
    for (int i = 0; i < 8; i++) assert(silence[i] == 0);
}

static void test_center_identity_and_opposite_contribution(void) {
    idle();
    peq_set_crossfeed(1.0);
    preroll(48000);
    int16_t center[64];
    for (int i = 0; i < 32; i++) {
        int16_t s = (int16_t) ((i * 1000) - 15000);
        center[i * 2] = center[i * 2 + 1] = s;
    }
    int16_t center_copy[64];
    memcpy(center_copy, center, sizeof center);
    peq_process(center, 32, 2, 48000);
    assert(memcmp(center, center_copy, sizeof center) == 0);

    idle();
    peq_set_crossfeed(1.0);
    preroll(48000);
    int16_t impulse[2] = {10000, 0};
    peq_process(impulse, 1, 2, 48000);
    assert(impulse[1] > 100 && impulse[1] < 1500);
    assert(impulse[0] < 10000 && impulse[0] > 8000);

    int16_t tail[400];
    memset(tail, 0, sizeof tail);
    peq_process(tail, 200, 2, 48000);
    int32_t energy = 0;
    for (int i = 0; i < 200; i++) energy += abs(tail[i * 2 + 1]);
    assert(energy > impulse[1]);
    assert(abs(tail[199 * 2 + 1]) < abs(tail[0 * 2 + 1]));
}

static double rms_right(const int16_t * stereo, size_t frames) {
    double acc = 0.0;
    for (size_t i = 0; i < frames; i++) {
        double s = stereo[i * 2 + 1];
        acc += s * s;
    }
    return sqrt(acc / (double) frames);
}

static void fill_sine(int16_t * stereo, size_t frames, unsigned int rate, double hz, int16_t amp) {
    for (size_t i = 0; i < frames; i++) {
        double w = 2.0 * M_PI * hz * (double) i / (double) rate;
        stereo[i * 2] = (int16_t) lrint(amp * sin(w));
        stereo[i * 2 + 1] = 0;
    }
}

static void test_frequency_dependence(void) {
    const unsigned int rate = 48000;
    const size_t frames = 4800;
    int16_t * low = calloc(frames * 2, sizeof(int16_t));
    int16_t * high = calloc(frames * 2, sizeof(int16_t));
    assert(low && high);
    idle();
    peq_set_crossfeed(1.0);
    preroll(rate);
    fill_sine(low, frames, rate, 100.0, 16000);
    peq_process(low, frames, 2, rate);
    idle();
    peq_set_crossfeed(1.0);
    preroll(rate);
    fill_sine(high, frames, rate, 8000.0, 16000);
    peq_process(high, frames, 2, rate);
    double low_rms = rms_right(low + 2000, frames - 1000);
    double high_rms = rms_right(high + 2000, frames - 1000);
    assert(low_rms > 2000.0);
    assert(high_rms * 8.0 < low_rms);

    float alpha = one_pole_alpha(rate);
    double w = 2.0 * M_PI * 100.0 / (double) rate;
    double mag2 = (double) alpha * alpha /
        (1.0 - 2.0 * (1.0 - alpha) * cos(w) + (1.0 - alpha) * (1.0 - alpha));
    double expect = 16000.0 / sqrt(2.0) * PEQ_CROSSFEED_MAX_BLEND * sqrt(mag2);
    assert(fabs(low_rms - expect) / expect < 0.08);
    free(low);
    free(high);
}

static void test_reference_order_and_formats(void) {
    idle();
    peq_set_crossfeed(1.0);
    peq_set_stereo_width(1.5);
    enum { N = 180 };
    int16_t in[N * 2], out[N * 2], ref[N * 2];
    uint32_t lcg = 1;
    for (int i = 0; i < N * 2; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        in[i] = (int16_t) ((lcg >> 16) % 20001 - 10000);
    }
    memcpy(out, in, sizeof in);
    peq_process(out, N, 2, 48000);
    reference_s16(in, ref, N, 48000, 1.0f, 1.5f);
    assert(memcmp(out, ref, sizeof out) == 0);

    idle();
    peq_set_crossfeed(1.0);
    /* Reach full strength in 16-bit, then one silent 24-bit frame consumes
     * the format-change prime so the following block is the plain one-pole. */
    preroll(48000);
    int32_t primed[2] = {0, 0};
    peq_process_s32(primed, 1, 2, 48000);
    int32_t block[32];
    for (int i = 0; i < 16; i++) {
        block[i * 2] = 200000;
        block[i * 2 + 1] = -80000;
    }
    peq_process_s32(block, 16, 2, 48000);
    float lp0 = 0, lp1 = 0, alpha = one_pole_alpha(48000);
    /* preroll ended at silence, so memory is ~0; replay the same recurrence. */
    int32_t expect[32];
    for (int i = 0; i < 16; i++) {
        float x0 = 200000.0f, x1 = -80000.0f;
        lp0 += alpha * (x0 - lp0);
        lp1 += alpha * (x1 - lp1);
        float k = (float) PEQ_CROSSFEED_MAX_BLEND;
        expect[i * 2] = (int32_t) (x0 + k * (lp1 - lp0));
        expect[i * 2 + 1] = (int32_t) (x1 + k * (lp0 - lp1));
    }
    assert(memcmp(block, expect, sizeof expect) == 0);
}

static void test_symmetry_bounds_and_blocks(void) {
    enum { N = 128 };
    int16_t a[N * 2], b[N * 2];
    uint32_t lcg = 99;
    for (int i = 0; i < N; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        a[i * 2] = (int16_t) (lcg >> 17);
        lcg = lcg * 1664525u + 1013904223u;
        a[i * 2 + 1] = (int16_t) (lcg >> 17);
        b[i * 2] = a[i * 2 + 1];
        b[i * 2 + 1] = a[i * 2];
    }
    idle();
    peq_set_crossfeed(1.0);
    preroll(48000);
    peq_process(a, N, 2, 48000);
    idle();
    peq_set_crossfeed(1.0);
    preroll(48000);
    peq_process(b, N, 2, 48000);
    for (int i = 0; i < N; i++) {
        assert(a[i * 2] == b[i * 2 + 1]);
        assert(a[i * 2 + 1] == b[i * 2]);
    }

    idle();
    peq_set_crossfeed(1.0);
    int16_t hot[400];
    int32_t hot32[400];
    for (int i = 0; i < 200; i++) {
        hot[i * 2] = (i & 1) ? 32767 : -32768;
        hot[i * 2 + 1] = (i & 1) ? -32768 : 32767;
        hot32[i * 2] = (i & 1) ? 8388607 : -8388608;
        hot32[i * 2 + 1] = (i & 1) ? -8388608 : 8388607;
    }
    peq_process(hot, 200, 2, 48000);
    for (int i = 0; i < 400; i++) assert(hot[i] >= -32768);
    peq_process_s32(hot32, 200, 2, 96000);
    for (int i = 0; i < 400; i++) {
        assert(hot32[i] <= 8388607);
        assert(hot32[i] >= -8388608);
    }

    int16_t whole[600], split[600];
    for (int i = 0; i < 300; i++) {
        whole[i * 2] = (int16_t) (1000 + (i % 50) * 100);
        whole[i * 2 + 1] = (int16_t) (-4000 + (i % 17) * 200);
    }
    memcpy(split, whole, sizeof whole);
    /* One silent frame absorbs a sample-rate prime so both runs start
     * from the same mix and filter memory. */
    idle();
    peq_set_crossfeed(0.8);
    int16_t absorb[2] = {0, 0};
    peq_process(absorb, 1, 2, 48000);
    peq_process(whole, 300, 2, 48000);
    idle();
    peq_set_crossfeed(0.8);
    peq_process(absorb, 1, 2, 48000);
    peq_process(split, 120, 2, 48000);
    peq_process(split + 240, 180, 2, 48000);
    assert(memcmp(whole, split, sizeof whole) == 0);
}

static void test_transitions(void) {
    idle();
    peq_set_crossfeed(1.0);
    preroll(48000);
    int16_t tone[2];
    for (int i = 0; i < 1000; i++) {
        tone[0] = 10000;
        tone[1] = 0;
        peq_process(tone, 1, 2, 48000);
    }
    int16_t before = tone[0];
    peq_set_crossfeed(0.0);
    int16_t step[2] = {10000, 0};
    peq_process(step, 1, 2, 48000);
    assert(abs(step[0] - before) < 30);

    size_t n = ramp_frames(48000) + 64;
    int16_t * down = calloc(n * 2, sizeof(int16_t));
    assert(down);
    for (size_t i = 0; i < n; i++) down[i * 2] = 10000;
    peq_process(down, n, 2, 48000);
    int max_step = 0;
    for (size_t i = 1; i < n; i++) {
        int d = abs(down[i * 2] - down[(i - 1) * 2]);
        if (d > max_step) max_step = d;
    }
    assert(max_step < 40);
    int16_t ident[4] = {10000, 0, -4000, 2500};
    int16_t ident_copy[4];
    memcpy(ident_copy, ident, sizeof ident);
    peq_process(ident, 2, 2, 48000);
    assert(memcmp(ident, ident_copy, sizeof ident) == 0);
    free(down);

    idle();
    peq_set_crossfeed(1.0);
    peq_set_crossfeed_angle(90);
    preroll(48000);
    int16_t loud[2];
    for (int i = 0; i < 500; i++) {
        loud[0] = 20000;
        loud[1] = -20000;
        peq_process(loud, 1, 2, 48000);
    }
    peq_set_bypass(true);
    int16_t bypassed[4] = {20000, -20000, 1, -1};
    int16_t bypassed_copy[4];
    memcpy(bypassed_copy, bypassed, sizeof bypassed);
    peq_process(bypassed, 2, 2, 48000);
    assert(memcmp(bypassed, bypassed_copy, sizeof bypassed) == 0);
    peq_set_bypass(false);
    int16_t after[8] = {0};
    peq_process(after, 4, 2, 48000);
    for (int i = 0; i < 8; i++) assert(after[i] == 0);

    idle();
    peq_set_crossfeed(1.0);
    preroll(44100);
    int16_t held[2];
    for (int i = 0; i < 800; i++) {
        held[0] = 9000;
        held[1] = -3000;
        peq_process(held, 1, 2, 44100);
    }
    int16_t at_new_rate[2] = {9000, -3000};
    peq_process(at_new_rate, 1, 2, 48000);
    assert(abs(at_new_rate[0] - held[0]) <= 1);
    assert(abs(at_new_rate[1] - held[1]) <= 1);

    idle();
    peq_set_crossfeed(1.0);
    preroll(44100);
    for (int i = 0; i < 200; i++) {
        held[0] = 22000;
        held[1] = -22000;
        peq_process(held, 1, 2, 44100);
    }
    int16_t silent_rate[4] = {0};
    peq_process(silent_rate, 2, 2, 48000);
    assert(silent_rate[0] == 0 && silent_rate[1] == 0 && silent_rate[2] == 0 && silent_rate[3] == 0);

    idle();
    peq_set_crossfeed(1.0);
    int32_t prime32[2] = {0, 0};
    peq_process_s32(prime32, 1, 2, 48000); /* enter s32 with memory cleared */
    size_t warm = ramp_frames(48000);
    int32_t * warm_buf = calloc(warm * 2, sizeof(int32_t));
    assert(warm_buf);
    for (size_t i = 0; i < warm; i++) warm_buf[i * 2] = 2560000;
    peq_process_s32(warm_buf, warm, 2, 48000);
    int32_t settled[2];
    for (int i = 0; i < 400; i++) {
        settled[0] = 2560000;
        settled[1] = 0;
        peq_process_s32(settled, 1, 2, 48000);
    }
    int16_t as16[2] = {10000, 0};
    peq_process(as16, 1, 2, 48000);
    assert(abs(as16[0] - settled[0] / 256) <= 2);
    assert(abs(as16[1] - settled[1] / 256) <= 2);
    free(warm_buf);

    idle();
    peq_set_crossfeed(1.0);
    int32_t again[2] = {0, 0};
    peq_process_s32(again, 1, 2, 48000);
    for (int i = 0; i < 300; i++) {
        again[0] = 7000000;
        again[1] = -7000000;
        peq_process_s32(again, 1, 2, 48000);
    }
    int16_t stale[4] = {1, 1, 1, 1};
    memset(stale, 0, sizeof stale);
    peq_process(stale, 2, 2, 48000);
    assert(stale[0] == 0 && stale[1] == 0 && stale[2] == 0 && stale[3] == 0);

    idle();
    peq_set_crossfeed(1.0);
    peq_set_preamp_db(6.0);
    int16_t mid[4] = {1000, 1000, -2000, -2000};
    peq_process(mid, 2, 2, 48000);
    /* +6 dB is approximately 2x, and centered audio is not crossfed. */
    assert(abs(mid[0] - 1995) < 8);
    assert(mid[0] == mid[1]);
    assert(mid[2] == mid[3]);
}

static void test_profiles(void) {
    idle();
    char dir[] = "/tmp/peq-crossfeed-XXXXXX";
    assert(mkdtemp(dir));
    char path[128], legacy[128], bad[128];
    snprintf(path, sizeof path, "%s/current.peq", dir);
    snprintf(legacy, sizeof legacy, "%s/legacy.peq", dir);
    snprintf(bad, sizeof bad, "%s/bad.peq", dir);

    peq_set_crossfeed(0.4);
    peq_set_crossfeed_angle(45);
    peq_set_stereo_width(1.25);
    peq_set_preamp_db(-1.5);
    assert(peq_save_to_path(path));
    peq_set_crossfeed(0.0);
    peq_set_crossfeed_angle(0);
    peq_set_stereo_width(1.0);
    peq_set_preamp_db(0.0);
    assert(peq_load_from_path(path));
    assert(fabs(peq_get_crossfeed() - 0.4) < 1e-5);
    assert(peq_get_crossfeed_angle() == 45);
    assert(fabs(peq_get_stereo_width() - 1.25) < 1e-5);
    assert(fabs(peq_get_preamp_db() + 1.5) < 1e-6);

    static const int angles[] = { 0, 30, 45, 60, 90 };
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        peq_set_crossfeed_angle(angles[i]);
        assert(peq_save_to_path(path));
        peq_set_crossfeed_angle(-1);
        assert(peq_get_crossfeed_angle() == 0);
        assert(peq_load_from_path(path));
        assert(peq_get_crossfeed_angle() == angles[i]);
    }

    FILE * f = fopen(legacy, "w");
    assert(f);
    fputs("bypass=0\npreamp=0.00\nstereo_width=1.500000\nband3_enabled=1\nband3_gain=2.50\n", f);
    fclose(f);
    peq_set_crossfeed(0.8);
    peq_set_crossfeed_angle(90);
    assert(peq_load_from_path(legacy));
    assert(peq_get_crossfeed() == 0.0);
    assert(peq_get_crossfeed_angle() == 0);
    assert(fabs(peq_get_stereo_width() - 1.5) < 1e-5);
    assert(peq_get_band(3)->enabled);
    assert(fabs(peq_get_band(3)->gain_db - 2.5) < 1e-6);

    f = fopen(bad, "w");
    assert(f);
    fputs("bypass=0\npreamp=1.00\ncrossfeed_angle=bogus\ncrossfeed_angle=31\n"
          "crossfeed=bogus\ncrossfeed=nan\ncrossfeed=inf\n"
          "crossfeed=1.5\ncrossfeed=\ncrossfeed=0.25xyz\nband1_enabled=1\n", f);
    fclose(f);
    peq_set_crossfeed(0.7);
    peq_set_crossfeed_angle(60);
    assert(peq_load_from_path(bad));
    assert(peq_get_crossfeed() == 1.0); /* 1.5 is the last finite in-range-clamped value */
    assert(peq_get_crossfeed_angle() == 0);
    assert(fabs(peq_get_preamp_db() - 1.0) < 1e-6);
    assert(peq_get_band(1)->enabled);

    f = fopen(bad, "w");
    assert(f);
    fputs("crossfeed=-4\ncrossfeed=abc\n", f);
    fclose(f);
    assert(peq_load_from_path(bad));
    assert(peq_get_crossfeed() == 0.0);
    assert(peq_get_crossfeed_angle() == 0);

    peq_set_crossfeed(0.2);
    assert(!peq_load_from_path("/tmp/peq-crossfeed-missing-file"));
    assert(fabs(peq_get_crossfeed() - 0.2) < 1e-6);

    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
}

/* Same immediate ceiling as peq.c's LIMITER_CEILING / LIMITER_CEILING_S32.
 * The 1 ms limiter attack is not this reference. */
#define CROSSFEED_CEILING_S16 32760.0f
#define CROSSFEED_CEILING_S32 8386560.0f

static void protect_peak(float *y0, float *y1, float x0, float x1, float ceiling) {
    float in_peak = fabsf(x0);
    float in_other = fabsf(x1);
    if (in_other > in_peak) in_peak = in_other;
    float out_peak = fabsf(*y0);
    float out_other = fabsf(*y1);
    if (out_other > out_peak) out_peak = out_other;
    if (!(out_peak > ceiling) || !(out_peak > in_peak)) return;
    float allowed = in_peak > ceiling ? in_peak : ceiling;
    if (!(out_peak > allowed)) return;
    float gain = allowed / out_peak;
    *y0 *= gain;
    *y1 *= gain;
}

static void warm_constant_s16(int16_t left, int16_t right) {
    preroll(48000);
    int16_t frame[2];
    for (int i = 0; i < 2000; i++) {
        frame[0] = left;
        frame[1] = right;
        peq_process(frame, 1, 2, 48000);
    }
}

static void warm_constant_s32(int32_t left, int32_t right) {
    int32_t frame[2] = {0, 0};
    peq_process_s32(frame, 1, 2, 48000);
    size_t n = ramp_frames(48000);
    int32_t * silence = calloc(n * 2, sizeof(int32_t));
    assert(silence);
    peq_process_s32(silence, n, 2, 48000);
    free(silence);
    for (int i = 0; i < 2000; i++) {
        frame[0] = left;
        frame[1] = right;
        peq_process_s32(frame, 1, 2, 48000);
    }
}

/* The audio thread calls peq_note_pcm_discontinuity() at stop/restart, seek,
 * an A-B loop jump, an A-B source switch, and around DoP. These checks are
 * that contract: same rate and format, no note leaves the warmed low-pass,
 * a note makes the next sample a new stream. */
static void test_discontinuity_clears_lowpass(void) {
    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    int16_t leaked[2] = {0, 0};
    peq_process(leaked, 1, 2, 48000);
    assert(leaked[0] < -10000);
    assert(leaked[1] > 10000);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    peq_note_pcm_discontinuity();
    peq_note_pcm_discontinuity(); /* sticky until the next PCM buffer */
    int16_t silent[4];
    memset(silent, 0, sizeof silent);
    peq_process(silent, 2, 2, 48000);
    assert(silent[0] == 0 && silent[1] == 0 && silent[2] == 0 && silent[3] == 0);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    peq_note_pcm_discontinuity();
    int16_t fresh[2] = {1000, 0};
    peq_process(fresh, 1, 2, 48000);
    float k = (float) PEQ_CROSSFEED_MAX_BLEND;
    float expect_l = 1000.0f + k * (0.0f - 1000.0f);
    float expect_r = 0.0f + k * (1000.0f - 0.0f);
    assert(fresh[0] == (int16_t) expect_l);
    assert(fresh[1] == (int16_t) expect_r);
    assert(abs(fresh[0]) < 1000);

    /* Omitting the note keeps block continuity, including the silence tail. */
    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    int16_t continued[2] = {0, 0};
    peq_process(continued, 1, 2, 48000);
    assert(continued[0] == leaked[0] && continued[1] == leaked[1]);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s32(6400000, -6400000);
    int32_t leaked32[2] = {0, 0};
    peq_process_s32(leaked32, 1, 2, 48000);
    assert(leaked32[0] < -10000 * 256);
    assert(leaked32[1] > 10000 * 256);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s32(6400000, -6400000);
    peq_note_pcm_discontinuity();
    int32_t silent32[4];
    memset(silent32, 0, sizeof silent32);
    peq_process_s32(silent32, 2, 2, 48000);
    assert(silent32[0] == 0 && silent32[1] == 0 && silent32[2] == 0 && silent32[3] == 0);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s32(6400000, -6400000);
    peq_note_pcm_discontinuity();
    int32_t fresh32[2] = {256000, 0};
    peq_process_s32(fresh32, 1, 2, 48000);
    float expect32_l = 256000.0f + k * (0.0f - 256000.0f);
    float expect32_r = 0.0f + k * (256000.0f - 0.0f);
    assert(fresh32[0] == (int32_t) expect32_l);
    assert(fresh32[1] == (int32_t) expect32_r);

    /* Same-format return after a gap, and a format change after the gap. */
    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    peq_set_bypass(true);
    peq_note_pcm_discontinuity();
    int16_t held[2] = {25000, -25000};
    int16_t held_copy[2];
    memcpy(held_copy, held, sizeof held);
    peq_process(held, 1, 2, 48000);
    assert(memcmp(held, held_copy, sizeof held) == 0);
    peq_set_bypass(false);
    int16_t after_bypass[2] = {0, 0};
    peq_process(after_bypass, 1, 2, 48000);
    assert(after_bypass[0] == 0 && after_bypass[1] == 0);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    peq_note_pcm_discontinuity();
    int32_t crossed[2] = {0, 0};
    peq_process_s32(crossed, 1, 2, 48000);
    assert(crossed[0] == 0 && crossed[1] == 0);

    idle();
    peq_note_pcm_discontinuity();
    int16_t dry[4] = {3, -4, 5, -6};
    int16_t dry_copy[4];
    memcpy(dry_copy, dry, sizeof dry);
    peq_process(dry, 2, 2, 48000);
    assert(memcmp(dry, dry_copy, sizeof dry) == 0);
}

static void reverse_reference(float x0, float x1, float *lp0, float *lp1,
                              float alpha, float k, float ceiling,
                              float *y0, float *y1) {
    *lp0 += alpha * (x0 - *lp0);
    *lp1 += alpha * (x1 - *lp1);
    *y0 = x0 + k * (*lp1 - *lp0);
    *y1 = x1 + k * (*lp0 - *lp1);
    float bare0 = *y0, bare1 = *y1;
    protect_peak(y0, y1, x0, x1, ceiling);
    (void) bare0;
    (void) bare1;
}

static void test_reverse_transient_headroom(void) {
    const float alpha = one_pole_alpha(48000);
    const float k = (float) PEQ_CROSSFEED_MAX_BLEND;
    float lp0 = 25000.0f, lp1 = -25000.0f;
    float bare0, bare1, y0, y1;
    /* One-pole step from a settled opposite pair, before protection. */
    float step0 = 25000.0f + alpha * (-25000.0f - 25000.0f);
    float step1 = -25000.0f + alpha * (25000.0f - (-25000.0f));
    bare0 = -25000.0f + k * (step1 - step0);
    bare1 = 25000.0f + k * (step0 - step1);
    assert(bare0 < -32768.0f);
    assert(bare1 > 32767.0f);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, -25000);
    int16_t rev[4] = {-25000, 25000, -25000, 25000};
    peq_process(rev, 2, 2, 48000);

    lp0 = 25000.0f;
    lp1 = -25000.0f;
    for (int i = 0; i < 2; i++) {
        float in0 = -25000.0f, in1 = 25000.0f;
        reverse_reference(in0, in1, &lp0, &lp1, alpha, k, CROSSFEED_CEILING_S16, &y0, &y1);
        assert(rev[i * 2] == (int16_t) y0);
        assert(rev[i * 2 + 1] == (int16_t) y1);
        assert(rev[i * 2] != -32768);
        assert(rev[i * 2 + 1] != 32767);
        assert(rev[i * 2] > -32760 || rev[i * 2] == (int16_t) -CROSSFEED_CEILING_S16);
        assert(abs(rev[i * 2]) <= 32760);
        assert(abs(rev[i * 2 + 1]) <= 32760);
        assert(rev[i * 2] == -rev[i * 2 + 1]);
    }
    /* First sample is the ceiling, not the hard clamp and not the raw blend. */
    assert(rev[0] == (int16_t) -CROSSFEED_CEILING_S16);
    assert(rev[1] == (int16_t) CROSSFEED_CEILING_S16);

    float s32_lp0 = 6400000.0f + alpha * (-6400000.0f - 6400000.0f);
    float s32_lp1 = -6400000.0f + alpha * (6400000.0f - (-6400000.0f));
    float bare32_0 = -6400000.0f + k * (s32_lp1 - s32_lp0);
    assert(bare32_0 < -8388608.0f);

    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s32(6400000, -6400000);
    int32_t rev32[4] = {-6400000, 6400000, -6400000, 6400000};
    peq_process_s32(rev32, 2, 2, 48000);
    lp0 = 6400000.0f;
    lp1 = -6400000.0f;
    for (int i = 0; i < 2; i++) {
        reverse_reference(-6400000.0f, 6400000.0f, &lp0, &lp1, alpha, k,
                          CROSSFEED_CEILING_S32, &y0, &y1);
        assert(rev32[i * 2] == (int32_t) y0);
        assert(rev32[i * 2 + 1] == (int32_t) y1);
        assert(rev32[i * 2] != -8388608);
        assert(rev32[i * 2 + 1] != 8388607);
        assert(rev32[i * 2] >= -8386560);
        assert(rev32[i * 2 + 1] <= 8386560);
        assert(rev32[i * 2] == -rev32[i * 2 + 1]);
    }
    assert(rev32[0] == (int32_t) -CROSSFEED_CEILING_S32);
    assert(rev32[1] == (int32_t) CROSSFEED_CEILING_S32);

    /* Off is still bit-exact, including this reversal. */
    idle();
    int16_t dry[4] = {25000, -25000, -25000, 25000};
    int16_t dry_copy[4];
    memcpy(dry_copy, dry, sizeof dry);
    peq_process(dry, 2, 2, 48000);
    assert(memcmp(dry, dry_copy, sizeof dry) == 0);

    /* Settled center stays unity. Width after the immediate gain still
     * matches the filtered blend on a signal that never reaches the ceiling. */
    idle();
    peq_set_crossfeed(1.0);
    warm_constant_s16(25000, 25000);
    int16_t center[2] = {25000, 25000};
    peq_process(center, 1, 2, 48000);
    assert(center[0] == 25000 && center[1] == 25000);

    idle();
    peq_set_crossfeed(1.0);
    peq_set_stereo_width(0.5);
    warm_constant_s16(8000, -8000);
    int16_t narrowed[2] = {-8000, 8000};
    peq_process(narrowed, 1, 2, 48000);
    lp0 = 8000.0f;
    lp1 = -8000.0f;
    reverse_reference(-8000.0f, 8000.0f, &lp0, &lp1, alpha, k, CROSSFEED_CEILING_S16, &y0, &y1);
    /* Width 0.5 scales an antisymmetric pair by 1/2 after crossfeed. */
    float direct = 0.75f, cross = 0.25f;
    float left = y0 * direct + y1 * cross;
    float right = y1 * direct + y0 * cross;
    assert(narrowed[0] == (int16_t) left);
    assert(narrowed[1] == (int16_t) right);
    assert(narrowed[0] == -narrowed[1]);
    assert(abs(narrowed[0]) < 8000);

    idle();
    peq_set_crossfeed(1.0);
    peq_set_preamp_db(6.0);
    int16_t mid[2] = {1000, 1000};
    peq_process(mid, 1, 2, 48000);
    assert(mid[0] == mid[1]);
    assert(abs(mid[0] - 1995) < 8);
}

int main(void) {
    test_defaults_and_setter();
    test_disabled_and_mono_identity();
    test_angle_impulse_delays();
    test_center_identity_and_opposite_contribution();
    test_frequency_dependence();
    test_reference_order_and_formats();
    test_symmetry_bounds_and_blocks();
    test_transitions();
    test_profiles();
    test_discontinuity_clears_lowpass();
    test_reverse_transient_headroom();
    puts("peq crossfeed tests passed");
    return 0;
}
