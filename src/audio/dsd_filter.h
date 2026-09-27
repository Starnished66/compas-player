#ifndef DSD_FILTER_H
#define DSD_FILTER_H

#include <stdbool.h>
#include <stdint.h>

/* Single-stage DSD-to-PCM decimator using a byte lookup-table FIR.
 *
 * The lowpass is a Kaiser (beta 8.6) windowed sinc, 448 taps per DSD64
 * multiple, cut off at the base rate (44.1 or 48 kHz) and decimating
 * straight to twice the base rate (88.2 or 96 kHz). Its taps are grouped by
 * 8, so each input byte contributes one precomputed partial sum: 56 table
 * lookups per DSD64 output sample and no multiplies.
 *
 * Measured response (44.1 kHz family; the 48 kHz family scales every
 * frequency by 48/44.1, and DSD128/256 keep the same response because the
 * tap count scales with the rate): flat within 0.0003 dB to 24 kHz, and at
 * least 88 dB down above 64.2 kHz, i.e. everything that could alias into
 * 0-24 kHz. DC gain is exactly 1, so a full-scale DSD signal maps to
 * full-scale PCM and SACD 0 dB (50% modulation) lands at -6 dBFS. */

#define DSD_FILTER_MAX_GROUPS 224 /* DSD256: 1792 taps / 8 */
/* Byte whose bits sum to zero: DSD's conventional idle pattern. */
#define DSD_SILENCE_BYTE 0x69

typedef struct dsd_filter_design dsd_filter_design_t;

/* Shared, reference-counted design for one DSD rate: multiple is the DSD
 * rate over the base rate (64, 128 or 256); base_rate is 44100 or 48000.
 * Decoders at the same rate (for example the current and the gapless next
 * track) share one table. NULL for an unsupported rate or out of memory.
 * Thread-safe. */
const dsd_filter_design_t * dsd_filter_design_acquire(unsigned int multiple, unsigned int base_rate);
void dsd_filter_design_release(const dsd_filter_design_t * design);

/* FIR length in bytes, and input bytes per channel per output sample. */
int dsd_filter_groups(const dsd_filter_design_t * design);
int dsd_filter_bytes_per_output(const dsd_filter_design_t * design);

/* Streaming per-channel state. The history is stored twice so the newest
 * window is always contiguous. */
typedef struct {
    uint8_t bytes[2 * DSD_FILTER_MAX_GROUPS];
    int byte_pos;   /* oldest byte in the window */
    int byte_phase; /* bytes pushed toward the next output */
} dsd_channel_state_t;

/* Fills the history with DSD silence so a fresh or seeked channel starts
 * from a zero signal rather than a full-scale DC step. */
void dsd_channel_reset(dsd_channel_state_t * ch);

/* Feeds one DSD byte, most significant bit first in time. Writes a PCM
 * sample to *out_sample and returns true once per
 * dsd_filter_bytes_per_output() bytes. */
bool dsd_channel_feed_byte(dsd_channel_state_t * ch, const dsd_filter_design_t * design, uint8_t byte,
                           float * out_sample);

#endif /* DSD_FILTER_H */
