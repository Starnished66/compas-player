#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

/* Pure sample-level helpers of the playback engine: ReplayGain, gain and
 * the crossfade mix. No state. */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static inline double replaygain_to_linear(bool has_gain, double gain_db, bool has_peak, double peak) {
    double linear = has_gain ? pow(10.0, gain_db / 20.0) : 1.0;
    if (has_peak && peak > 0.0) {
        double max_linear = 1.0 / peak; /* clamp so the loudest sample can't clip */
        if (linear > max_linear) linear = max_linear;
    }
    /* Belt-and-suspenders alongside metadata.c's own parse_replaygain_gain()/
     * _peak() range limits: those already reject an absurd-but-finite
     * gain_db (e.g. a corrupted "1e308" tag) before it ever reaches this
     * function, but this is the one real choke point every gain value from
     * every tag format/call site funnels through before apply_gain()'s
     * lrintf() -- cheap enough to guard here too rather than trust every
     * current and future caller to have validated its own inputs. Falls
     * back to unity (no gain change) rather than propagating a non-finite
     * value into raw sample math. */
    if (!isfinite(linear)) linear = 1.0;
    return linear;
}

/* Applies linear gain scaling to int16 PCM samples. Uses lrintf() to round
 * to nearest integer to minimize quantization error in quiet passages. */
static inline void apply_gain(int16_t * buf, size_t sample_count, float gain) {
    /* Fast path: no scaling needed. A positive ReplayGain adjustment can
     * push this above 1.0f, so this can't just be ">= 0.999f". */
    if (gain > 0.999f && gain < 1.001f) return;
    for (size_t i = 0; i < sample_count; i++) {
        long scaled = lrintf((float) buf[i] * gain);
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        buf[i] = (int16_t) scaled;
    }
}

#ifndef HOST_BUILD
static inline void apply_gain_s32(int32_t * buf, size_t sample_count, float gain) {
    if (gain > 0.999f && gain < 1.001f) return;
    for (size_t i = 0; i < sample_count; i++) {
        float scaled_f = (float) buf[i] * gain;
        if (scaled_f > 8388607.0f) {
            buf[i] = 8388607;
        } else if (scaled_f < -8388608.0f) {
            buf[i] = -8388608;
        } else {
            buf[i] = (int32_t) lrintf(scaled_f);
        }
    }
}
#endif

/* Mixes n frames of buf_cur/buf_next into buf_out with a linear crossfade.
 * fade_start_frame is how many frames into the CROSSFADE_SECONDS window
 * this chunk's first frame falls, so the fade is continuous across chunk
 * boundaries rather than stepped.
 *
 * int16_t crossfade blending path. Used when either the current or next track
 * is 16-bit. When both tracks qualify for S24_LE wide output, mix_crossfade_s32()
 * is used instead to avoid reopening ALSA at S16_LE. */
static inline void mix_crossfade(const int16_t * buf_cur, const int16_t * buf_next, int16_t * buf_out,
                           uint64_t n, unsigned int channels, uint64_t fade_start_frame, uint64_t crossfade_frames) {
    for (uint64_t k = 0; k < n; k++) {
        float fade_next = (float) (fade_start_frame + k) / (float) crossfade_frames;
        if (fade_next > 1.0f) fade_next = 1.0f;
        if (fade_next < 0.0f) fade_next = 0.0f;
        float fade_cur = 1.0f - fade_next;
        for (unsigned int ch = 0; ch < channels; ch++) {
            size_t idx = (size_t) k * channels + ch;
            float mixed = (float) buf_cur[idx] * fade_cur + (float) buf_next[idx] * fade_next;
            if (mixed > 32767.0f) mixed = 32767.0f;
            if (mixed < -32768.0f) mixed = -32768.0f;
            buf_out[idx] = (int16_t) mixed;
        }
    }
}

#ifndef HOST_BUILD
/* Same as mix_crossfade() but for the S24_LE wide path's int32_t buffers
 * (right-justified 24-in-32). Only ever called when BOTH cur_dec and
 * nxt_dec have already been confirmed S24_LE-eligible -- see this
 * function's own caller. */
static inline void mix_crossfade_s32(const int32_t * buf_cur, const int32_t * buf_next, int32_t * buf_out,
                              uint64_t n, unsigned int channels, uint64_t fade_start_frame, uint64_t crossfade_frames) {
    for (uint64_t k = 0; k < n; k++) {
        float fade_next = (float) (fade_start_frame + k) / (float) crossfade_frames;
        if (fade_next > 1.0f) fade_next = 1.0f;
        if (fade_next < 0.0f) fade_next = 0.0f;
        float fade_cur = 1.0f - fade_next;
        for (unsigned int ch = 0; ch < channels; ch++) {
            size_t idx = (size_t) k * channels + ch;
            float mixed = (float) buf_cur[idx] * fade_cur + (float) buf_next[idx] * fade_next;
            if (mixed > 8388607.0f) mixed = 8388607.0f;
            if (mixed < -8388608.0f) mixed = -8388608.0f;
            buf_out[idx] = (int32_t) mixed;
        }
    }
}
#endif

#endif /* AUDIO_DSP_H */
