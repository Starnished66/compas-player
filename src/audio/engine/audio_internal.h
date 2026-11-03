#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

/* Declarations shared by the audio engine translation units (audio*.c).
 * Not part of the public API (audio.h). */

#include "audio.h"
#include "audio_dr_config.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "../decoders/aiff/aiff_decoder.h"
#include "../decoders/dsd/dsd_decoder.h"
#include "../decoders/aac/aac_decoder.h"
#include "../decoders/alac/alac_decoder.h"
#include "../decoders/ape/ape_decoder.h"
#include "../decoders/wma/wma_decoder.h"
#include "../decoders/ogg/opus_decoder.h"
#include "../decoders/vorbis/vorbis_decoder.h"
#include "../decoders/wavpack/wavpack_decoder.h"
#include "../decoders/caf/caf_decoder.h"
#include "../../network/http/stream/http_stream.h"
#include "decoder_result.h"
#include "audio_helpers.h"
#ifdef HOST_BUILD
#include <SDL2/SDL.h>
#else
#include "../output/audio_output.h"
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define NORMAL_CHUNK_FRAMES 4096
#define LOW_POWER_CHUNK_FRAMES 8192
#define MAX_CHUNK_FRAMES LOW_POWER_CHUNK_FRAMES
#define MAX_CHANNELS 8

/* --- Decoder dispatch: dr_flac/dr_mp3/dr_wav all expose the same shape of
 * API (open, channels/sampleRate, read_pcm_frames_s16, seek_to_pcm_frame,
 * close) but aren't literally the same type, so this is just a small tagged
 * union picking the right calls rather than a new abstraction layer. */

typedef enum {
    DECODER_FLAC,
    DECODER_MP3,
    DECODER_WAV,
    DECODER_AIFF,
    DECODER_DSD,
    DECODER_AAC,
    DECODER_ALAC,
    DECODER_APE,
    DECODER_WMA,
    DECODER_OPUS,
    DECODER_VORBIS,
    DECODER_WAVPACK,
    DECODER_CAF
} decoder_type_t;

typedef struct {
    decoder_type_t type;
    union {
        drflac * flac;
        drmp3 * mp3;
        drwav * wav;
        aiff_decoder_t * aiff;
        dsd_decoder_t * dsd;
        aac_decoder_t * aac;
        alac_decoder_t * alac;
        ape_decoder_t * ape;
        wma_decoder_t * wma;
        opus_decoder_wrap_t * opus;
        vorbis_decoder_wrap_t * vorbis;
        wavpack_decoder_t * wavpack;
        caf_decoder_t * caf;
    } as;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int source_sample_rate;
    unsigned int source_bit_depth;
    unsigned int bitrate_kbps;
    uint64_t total_frames;
    audio_error_t open_error;
    bool open_io_error; /* the open failed on a read/open error, not on what the file is (probe only) */

    /* Local MP3 only. dr_mp3 never owns a bound seek table, so this tagged
     * decoder owns and frees it alongside the drmp3 instance. */
    drmp3_seek_point * mp3_seek_points;
    drmp3_uint32 mp3_seek_point_count;
    bool mp3_seek_index_attempted;

    /* Network input is owned until decoder_close(). Live inputs remain
     * forward-only; finite Range inputs may seek when both the server and
     * decoder support it. The codec still determines the decode/read API. */
    http_stream_t * net_stream;
    bool seekable;
    /* Network MP3 read context. On the heap because dr_mp3 keeps this
     * pointer while the playback thread copies decoder_t by value when it
     * promotes the next track (cur_dec = nxt_dec). */
    struct net_mp3_reader * net_mp3;

    /* Local FLAC callback stream. dr_flac has no relaxed file convenience
     * wrapper, so keep the FILE alive for the decoder lifetime. */
    FILE * flac_file;

    /* CUE tracks expose a bounded, track-relative timeline over the source
     * decoder. The underlying codec always stays positioned in source frames. */
    bool segmented;
    uint64_t segment_start_frame;
    uint64_t segment_frame_count;
    uint64_t segment_read_cursor;
} decoder_t;

/* --- audio.c: playback state needed by the decoder open policy --- */
extern pthread_mutex_t audio_mutex;
extern pthread_cond_t audio_cond;
extern uint64_t playback_generation;
/* protected by audio_mutex */
extern char * active_path;

/* --- audio_decoder_open.c --- */
audio_codec_t public_codec_for_decoder(const decoder_t * dec);
bool is_stream_url(const char * path);
bool decoder_open(decoder_t * dec, const char * path);
bool decoder_open_for_generation(decoder_t * dec, const char * path, uint64_t generation);
bool decoder_open_next(decoder_t * dec, const char * path);

/* --- audio_decoder_io.c --- */
/* Every local (non-stream) MP3 is a candidate for a seek-point index
 * (see request_mp3_seek_index_locked()'s own `desired < 2` bailout for the
 * self-consistent floor: a file too short to place at least 2 points at
 * MP3_SEEK_INDEX_INTERVAL_SECONDS spacing just never gets an index,
 * brute-force seeking being cheap enough there anyway). Building always
 * happens off the playback thread (see mp3_index_worker()) so it can never
 * block audio output, including at a crossfade/gapless boundary. */
#define MP3_SEEK_INDEX_INTERVAL_SECONDS 2u
/* 24-byte drmp3_seek_point entries; caps a single track's table at ~196 KiB
 * (a ~4-hour recording at the interval above lands just under this, ~169
 * KiB) -- small next to this device's RAM budget, and this codebase's other
 * long-lived caches (album art, metadata DB) are already sized in that same
 * ballpark. */
#define MP3_SEEK_INDEX_MAX_POINTS 8192u

void decoder_close(decoder_t * dec);
bool decoder_seek_raw(decoder_t * dec, uint64_t frame);
decoder_read_result_t decoder_read_s16(decoder_t * dec, uint64_t frames, int16_t * buf);
#ifndef HOST_BUILD
decoder_read_result_t decoder_read_s32(decoder_t * dec, uint64_t frames, int32_t * buf);
#endif
bool mp3_needs_seek_index(const decoder_t * dec);
bool decoder_supports_sample_accurate_loop(const decoder_t * dec);
bool decoder_is_dop(const decoder_t * dec);
#ifndef HOST_BUILD
bool decoder_set_dop(decoder_t *dec, bool dop, uint64_t frame, uint64_t *out_frame);
#endif
bool reopen_decoder_at(decoder_t * dec, const char * path, uint64_t last_confirmed_frame);

/* What identifies one MP3 stream: a seek table built for one decoder is only
 * valid for another decoder of the same file with the same identity. */
typedef struct {
    unsigned int sample_rate;
    unsigned int channels;
    uint64_t stream_length;
    uint64_t stream_start_offset;
    uint32_t delay_frames;
    uint32_t padding_frames;
} mp3_stream_id_t;

static inline mp3_stream_id_t mp3_stream_id_of(const drmp3 * mp3) {
    return (mp3_stream_id_t) {
        .sample_rate = mp3->sampleRate, .channels = mp3->channels,
        .stream_length = mp3->streamLength, .stream_start_offset = mp3->streamStartOffset,
        .delay_frames = mp3->delayInPCMFrames, .padding_frames = mp3->paddingInPCMFrames,
    };
}

static inline bool mp3_stream_matches(const drmp3 * mp3, const mp3_stream_id_t * id) {
    return mp3->sampleRate == id->sample_rate && mp3->channels == id->channels &&
           mp3->streamLength == id->stream_length && mp3->streamStartOffset == id->stream_start_offset &&
           mp3->delayInPCMFrames == id->delay_frames && mp3->paddingInPCMFrames == id->padding_frames;
}

/* main.c installs the real alternate stack. */
void install_thread_crash_altstack(void);

static inline void set_worker_thread_name(const char * name) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, name);
#else
    (void) name;
#endif
}

static inline uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000ULL + (uint64_t) now.tv_nsec / 1000000ULL;
}

/* --- audio_mp3_index.c: state guarded by audio_mutex unless atomic --- */
typedef enum {
    MP3_INDEX_READY,
    MP3_INDEX_TRANSIENT_FAILURE,
    MP3_INDEX_PERMANENT_FAILURE
} mp3_index_outcome_t;

typedef struct mp3_index_job {
    char * path;
    uint64_t generation;
    mp3_stream_id_t stream;
    uint64_t total_pcm_frames; /* already known from the Xing/Info tag (dec->total_frames) -- lets the
                                 * worker place points in one pass instead of re-deriving this via its
                                 * own full-file counting pass, see mp3_build_seek_points_cancellable() */
    drmp3_uint32 count;
    drmp3_seek_point * points;
    mp3_index_outcome_t outcome;
} mp3_index_job_t;

/* Cancellation state of one seek-index build: the playback it was started
 * for, plus (for the A/B switch builder) its independent token. */
typedef struct {
    uint64_t generation;
    const atomic_uint * token; /* NULL when unused */
    unsigned expected_token;
} mp3_index_cancel_t;

extern bool mp3_index_worker_active;
extern mp3_index_job_t * mp3_index_result;
extern bool mp3_seek_deferred;
extern uint64_t mp3_seek_deferred_frame;
extern uint64_t mp3_seek_deferred_generation;
extern unsigned int mp3_seek_deferred_sample_rate;
extern unsigned int mp3_seek_retry_count;
extern uint64_t mp3_seek_retry_after_ms;
extern atomic_uint mp3_index_active_generation;
extern atomic_bool mp3_index_stop_flag;

#define MP3_SEEK_RETRY_MAX 3u
#define MP3_SEEK_RETRY_DELAY_MS 500u

bool mp3_index_cancelled(void * user);
void defer_mp3_seek_locked(const decoder_t * dec, uint64_t generation, uint64_t frame);
void finish_mp3_deferred_seek_locked(void);
void free_mp3_index_job(mp3_index_job_t * job);
bool request_mp3_seek_index_locked(decoder_t * dec, const char * path, uint64_t generation);

/* --- audio.c: control flags read by the output layer (guarded by audio_mutex) --- */
extern bool stop_requested;
extern bool restart_requested;
extern bool paused;
extern bool seek_pending;
extern atomic_bool local_output_active;

/* --- audio_volume.c: guarded by audio_mutex --- */
extern float volume_gain; /* linear PCM multiplier the playback thread applies */

/* --- audio_output_sink.c --- */
void close_device(void);
bool ensure_device(unsigned int channels, unsigned int sample_rate);
#ifdef HOST_BUILD
extern SDL_AudioDeviceID sdl_dev;
extern unsigned int device_channels;
extern unsigned int device_sample_rate;
void write_device(const int16_t * buf, uint64_t frames, unsigned int channels);
#else
bool ensure_device_format(unsigned int channels, unsigned int sample_rate, bool want_s24);
bool ensure_device_dop(unsigned int channels, unsigned int sample_rate);
write_result_t ensure_device_with_retry(unsigned int channels, unsigned int sample_rate,
                                        bool want_s24, bool dop, const char * path,
                                        bool allow_during_stop_restart);
write_result_t write_device_with_retry_ex(const int16_t * buf, uint64_t frames,
                                          unsigned int channels, unsigned int sample_rate,
                                          const char * path, uint64_t * out_delivered_frames,
                                          bool allow_during_stop_restart);
write_result_t write_device_with_retry_s32_ex(const int32_t * buf, uint64_t frames,
                                              unsigned int channels, unsigned int sample_rate,
                                              const char * path, int16_t * fallback_buf,
                                              uint64_t * out_delivered_frames,
                                              bool allow_during_stop_restart, bool dop);

static inline write_result_t write_device_with_retry(const int16_t * buf, uint64_t frames,
                                                     unsigned int channels,
                                                     unsigned int sample_rate,
                                                     const char * path,
                                                     uint64_t * out_delivered_frames) {
    return write_device_with_retry_ex(buf, frames, channels, sample_rate, path, out_delivered_frames, false);
}

static inline write_result_t write_device_transition_ramp(const int16_t * buf, uint64_t frames,
                                                          unsigned int channels,
                                                          unsigned int sample_rate,
                                                          const char * path,
                                                          uint64_t * out_delivered_frames) {
    return write_device_with_retry_ex(buf, frames, channels, sample_rate, path, out_delivered_frames, true);
}

static inline write_result_t write_device_with_retry_s32(const int32_t * buf, uint64_t frames,
                                                         unsigned int channels,
                                                         unsigned int sample_rate,
                                                         const char * path,
                                                         int16_t * fallback_buf,
                                                         uint64_t * out_delivered_frames) {
    return write_device_with_retry_s32_ex(buf, frames, channels, sample_rate, path, fallback_buf, out_delivered_frames,
                                          false, false);
}

/* DoP frames straight to the DAC; see decoder_is_dop(). */
static inline write_result_t write_device_with_retry_dop(const int32_t * buf, uint64_t frames,
                                                         unsigned int channels,
                                                         unsigned int sample_rate,
                                                         const char * path,
                                                         uint64_t * out_delivered_frames) {
    return write_device_with_retry_s32_ex(buf, frames, channels, sample_rate, path, NULL, out_delivered_frames,
                                          false, true);
}

static inline write_result_t write_device_transition_ramp_s32(const int32_t * buf, uint64_t frames,
                                                              unsigned int channels,
                                                              unsigned int sample_rate,
                                                              const char * path,
                                                              int16_t * fallback_buf,
                                                              uint64_t * out_delivered_frames) {
    return write_device_with_retry_s32_ex(buf, frames, channels, sample_rate, path,
                                          fallback_buf, out_delivered_frames, true, false);
}

#endif

/* --- audio_dr_impl.c --- */
drmp3_uint32 mp3_build_seek_points_cancellable(drmp3 * scan, uint64_t total_pcm_frames,
                                               drmp3_uint32 desired_count,
                                               drmp3_seek_point * points, bool * out_cancelled,
                                               bool (*cancel)(void *), void * cancel_user);

typedef struct {
    /* A CRC-validated lower point, retained only for this extraction's
     * monotonically increasing waveform targets. */
    drflac_uint64 byte;
    drflac_uint64 pcm;
    drflac_uint64 target;
    bool valid;
} waveform_seek_anchor_t;

bool waveform_flac_seeded_seek(decoder_t * dec, uint64_t frame,
                               uint64_t file_size,
                               bool (*cancel)(void *), void * user,
                               waveform_seek_anchor_t * anchor);
bool decoder_seek(decoder_t * dec, uint64_t frame);

#endif /* AUDIO_INTERNAL_H */
