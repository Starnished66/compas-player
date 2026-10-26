#ifndef PEQ_H
#define PEQ_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Parametric EQ: PEQ_NUM_BANDS independent biquad filters in cascade, each
 * with adjustable center frequency, gain, Q, and filter type (peaking /
 * low shelf / high shelf) -- standard RBJ Audio EQ Cookbook coefficients
 * (shelf filters parameterized by Q the same way the Web Audio API's
 * BiquadFilterNode does, rather than the cookbook's alternate S/slope
 * parameter, so every band exposes the same freq/gain/Q trio regardless of
 * type). Applied to interleaved S16 stereo (or mono) PCM in the audio
 * output path, with independent filter state per channel so left/right
 * don't interfere with each other. Band layout (10 bands, ISO standard
 * center frequencies, first/last as shelves) matches a typical hardware
 * DAP's own PEQ screen. */

#define PEQ_NUM_BANDS 10

typedef enum {
    PEQ_TYPE_PEAKING = 0,
    PEQ_TYPE_LOW_SHELF,
    PEQ_TYPE_HIGH_SHELF,
} peq_band_type_t;

typedef struct {
    double freq_hz;
    double gain_db;
    double q;
    peq_band_type_t type;
    bool enabled;
} peq_band_t;

void peq_init(void);

bool peq_get_bypass(void);
void peq_set_bypass(bool bypass);

double peq_get_preamp_db(void);
void peq_set_preamp_db(double db);

/* Stereo width: 0 folds stereo to mono, 1 is neutral, 2 widens.
 * Smoothed in the PCM thread; mono input is unaffected. Widening applies
 * headroom compensation and reduces center level. Saved with PEQ profiles,
 * reset to 1 by old profiles/defaults, and disabled by PEQ bypass. Native
 * DoP output bypasses the PCM processing stage entirely. */
double peq_get_stereo_width(void);
void peq_set_stereo_width(double width);

/* Headphone crossfeed strength, 0 (off) through 1 (full effect).
 *
 * Filtered-difference headphone blend, not a stereo-width matrix and not a
 * room or HRTF model. Each channel keeps its
 * sample and adds k times the one-pole low-pass difference between the
 * channels (cutoff PEQ_CROSSFEED_FC_HZ): yL = xL + k * (delayedLP_R - lpL),
 * and the mirror for R. Angle 0 uses no explicit delay and keeps steady mono
 * (L == R) at unity. Nonzero angles delay only the opposite channel's
 * filtered contribution, changing mono frequency phase as an inherent ITD
 * tradeoff; DC remains at unity.
 *
 * At strength s the low-frequency cross coefficient is
 * k = s * PEQ_CROSSFEED_MAX_BLEND (direct 1 - k once the low-pass has
 * settled). High frequencies approach direct = 1, cross = 0. k <= 0.30, so
 * a full-scale hard-panned steady tone cannot exceed full scale from the
 * mix itself. A polarity reversal can still reach about 1 + 2k times full
 * scale while the low-pass holds the previous sample. The 1 ms limiter
 * attack does not cover those first samples. While crossfeed is active,
 * that same sample applies a stereo-linked gain so the frame peak does not
 * exceed the limiter ceiling (32760 in 16-bit, 8386560 in 24-bit) unless
 * the pre-crossfeed peak was already above it. In-range audio is not
 * attenuated. Strength 0 does not apply this gain, and the baseline limiter
 * is unchanged.
 *
 * Processing order for an active PCM buffer: per-channel preamp, per-channel
 * EQ, crossfeed (including that immediate gain), stereo width, limiter and
 * clamp. Mono buffers skip crossfeed and width. Strength 0 is an exact
 * no-op (the PCM fast path returns before the sample loop when preamp,
 * bands, width, and crossfeed are all idle). Native DoP never enters
 * peq_process(). PEQ bypass disables preamp, bands, and stereo width while
 * Crossfeed remains independent and can still run through this PCM stage.
 *
 * The stored default is 0 for new installs, reset, and profiles that omit
 * the key. PEQ_CROSSFEED_SUGGESTED is a conservative strength for a caller
 * that is explicitly turning the effect on. The audio thread ramps strength
 * and angle tap changes over PEQ_CROSSFEED_RAMP_SEC. Going idle or a
 * non-stereo buffer clears the low-pass and delay history. A sample-rate
 * or PCM-format change, and a PCM discontinuity noted by the audio thread,
 * prime both from the next input sample so the previous stream is not
 * replayed and steady DC does not step. Continuous gapless PCM and ordinary
 * block boundaries retain history. */
#define PEQ_CROSSFEED_FC_HZ 700.0
#define PEQ_CROSSFEED_MAX_BLEND 0.30
#define PEQ_CROSSFEED_RAMP_SEC 0.020
#define PEQ_CROSSFEED_SUGGESTED 0.65
double peq_get_crossfeed(void);
void peq_set_crossfeed(double strength);

/* Explicit interaural delay angle. Supported values are 0, 30, 45, 60, and
 * 90 degrees (approximately 0, 261, 381, 488, and 655 microseconds). Invalid
 * values select 0. Persisted with PEQ profiles; omitted or invalid values
 * and resets select 0. The PCM thread owns delay history and blends tap
 * changes over PEQ_CROSSFEED_RAMP_SEC. A new angle requested during a blend
 * is coalesced to the latest target and begins after the current blend ends.
 * Angle 0 keeps legacy no-delay mixing. */
int peq_get_crossfeed_angle(void);
void peq_set_crossfeed_angle(int degrees);

/* Audio thread only. Sets a flag; it does not write filter memory. The next
 * peq_process / peq_process_s32 on that thread drops the crossfeed low-pass
 * and restarts it from the following input sample. Call it at a real PCM
 * break (stop/restart, seek, A-B loop jump, A-B source switch, DoP leaving
 * or returning to PCM). Do not call it for continuous gapless PCM or for
 * the next block of the same stream. */
void peq_note_pcm_discontinuity(void);
const peq_band_t * peq_get_band(int index);
/* Recomputes that band's filter coefficients from the new params -- takes
 * effect on the next processed buffer, no audible click beyond the normal
 * filter response change since biquad state (not just coefficients) is
 * preserved. */
void peq_set_band(int index, double freq_hz, double gain_db, double q);
void peq_set_band_type(int index, peq_band_type_t type);
void peq_set_band_enabled(int index, bool enabled);

/* Read the configured EQ magnitude response in dB at each frequency. This
 * uses the supplied nominal sample rate (zero selects 48000; the UI should
 * pass 48000), includes
 * preamp and enabled bands, ignores bypass, and excludes the signal-dependent
 * limiter. Null pointers or a zero count leave the output untouched; invalid individual
 * frequencies produce the preamp-only response. This query does not change
 * audio processing coefficients, filter state, or dirty/sample-rate caches. */
void peq_get_response_db(const double * frequencies_hz, double * response_db,
                         size_t count, unsigned int sample_rate);

/* In-place processing of an interleaved S16 buffer. No-op (fast path) if
 * PEQ is bypassed and Crossfeed is idle, or no bands are enabled, preamp is 0 dB, width is neutral, and
 * crossfeed strength is 0. */
void peq_process(int16_t * buf, size_t frame_count, int channels, unsigned int sample_rate);

/* In-place processing of an interleaved S24_LE/S32 buffer (low 24 bits active).
 * Uses scaled limiter ceiling (~8386560) and 24-bit clamp bounds. */
void peq_process_s32(int32_t * buf, size_t frame_count, int channels, unsigned int sample_rate);

/* Persistence, same temp-file-then-rename pattern as settings.c -- this
 * pair always targets the one fixed, always-current PEQ_FILE_PATH (loaded
 * at startup, saved on every change), distinct from the named-profile
 * pair below. */
void peq_load(void);
void peq_save(void);

/* Named profiles: save/load the exact same file format to/from an
 * arbitrary path (an SD card location the user picks a name for), so a EQ
 * setup can be kept around, shared, or swapped for a different one without
 * losing whatever's currently active in PEQ_FILE_PATH. Both return false on
 * failure (path unwritable/unreadable) rather than silently doing nothing,
 * since these are direct user actions ("Save Profile"/"Load Profile") that
 * need to report success or failure back to the UI, unlike peq_load()/
 * peq_save()'s own fire-and-forget startup/autosave use. */
bool peq_save_to_path(const char * path);
bool peq_load_from_path(const char * path);

/* Every *.peq file directly inside root (not recursive -- see peq.c's own
 * comment on why), sorted alphabetically, for a "Load Profile" file list.
 * Caller owns *out_paths (free each entry, then the array). Returns false
 * if root can't be read or has no *.peq files in it. */
bool peq_scan_profiles(const char * root, char *** out_paths, int * out_count);

/* Restores every band, the preamp, bypass, stereo width, and crossfeed to
 * their original defaults (the same values peq_init() starts from before any
 * saved file is loaded -- see peq.c's own set_defaults()). Crossfeed defaults
 * off. A full factory reset of the current PEQ, distinct from loading a named
 * profile. Caller refreshes UI widgets and calls peq_save() afterward itself,
 * same division of responsibility as peq_load_from_path(). */
void peq_reset_to_defaults(void);
/* Reset PEQ controls while preserving independent Crossfeed strength and angle. */
void peq_reset_equalizer(void);

#endif /* PEQ_H */
