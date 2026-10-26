#ifndef DSD_FILTER_H
#define DSD_FILTER_H

#include <stdbool.h>
#include <stdint.h>

/* Three-stage DSD-to-PCM decimator.
 *
 * A: a fifth-order moving average (sinc^5) straight from the 1-bit stream
 *    to 8x the base rate (352.8 or 384 kHz), as a byte lookup-table FIR.
 *    Its zeros sit exactly on the frequencies that fold back onto the audio
 *    band, so it needs K x R bits of window for a decimation of R bits: 5,
 *    10 or 20 table lookups per output for DSD64, 128 and 256. The work per
 *    second therefore grows only with the DSD bit rate, and the tables
 *    (5 to 20 KiB) stay in cache.
 * B: a 23-tap half-band FIR to 4x the base rate.
 * C: a 39-tap FIR to 2x the base rate (88.2 or 96 kHz). It also undoes
 *    stage A's gentle passband droop.
 *
 * Designed response (44.1 kHz family; the 48 kHz family scales every
 * frequency by 48/44.1, and DSD128/256 keep the same response): flat within
 * 0.001 dB to 20 kHz and 0.002 dB to 24 kHz, and at least 110 dB down for
 * everything that would fold into 0-24 kHz. DC gain is exactly 1, so a
 * full-scale DSD signal maps to full-scale PCM and SACD 0 dB (50%
 * modulation) lands at -6 dBFS. */

#define DSD_FILTER_A_MAX_GROUPS 20 /* DSD256: 156 taps in 20 bytes */
#define DSD_FILTER_A_RING 32
#define DSD_FILTER_B_TAPS 23
#define DSD_FILTER_B_RING 32
#define DSD_FILTER_C_TAPS 39
#define DSD_FILTER_C_RING 64
/* Byte whose bits sum to zero: DSD's conventional idle pattern. */
#define DSD_SILENCE_BYTE 0x69

typedef struct dsd_filter_design dsd_filter_design_t;

/* Shared, reference-counted design for one DSD rate: multiple is the DSD
 * rate over the base rate (64, 128 or 256); base_rate is 44100 or 48000.
 * Decoders at the same rate (for example the current and the gapless next
 * track) share one design. NULL for an unsupported rate or out of memory.
 * Thread-safe. */
const dsd_filter_design_t * dsd_filter_design_acquire(unsigned int multiple, unsigned int base_rate);
void dsd_filter_design_release(const dsd_filter_design_t * design);

/* Stage A window in bytes, and input bytes per channel per output sample. */
int dsd_filter_groups(const dsd_filter_design_t * design);
int dsd_filter_bytes_per_output(const dsd_filter_design_t * design);

/* Output samples after which every stage holds only real input, so a seek
 * that decodes this many frames first matches continuous playback. */
int dsd_filter_warmup_outputs(const dsd_filter_design_t * design);

/* Streaming per-channel state. Each history is stored twice so the newest
 * window is always contiguous. */
typedef struct {
    uint8_t bytes[2 * DSD_FILTER_A_RING];
    float a_out[2 * DSD_FILTER_B_RING];
    float b_out[2 * DSD_FILTER_C_RING];
    int byte_pos;
    int a_pos;
    int b_pos;
} dsd_channel_state_t;

/* Fills stage A with DSD silence and the later stages with zeros, so a
 * fresh or seeked channel starts from a quiet signal rather than a
 * full-scale DC step. */
void dsd_channel_reset(dsd_channel_state_t * ch);

/* Consumes dsd_filter_bytes_per_output() bytes, oldest first, each with its
 * most significant bit first in time, and returns one PCM sample. */
float dsd_channel_frame(dsd_channel_state_t * ch, const dsd_filter_design_t * design, const uint8_t * bytes);

#endif /* DSD_FILTER_H */
