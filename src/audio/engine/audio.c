#include "audio.h"
#include "audio_internal.h"
#include "audio_dsp.h"
#include "../../ui/text/i18n/i18n.h"

#include "../decoders/aiff/aiff_decoder.h"
#include "../decoders/dsd/dsd_decoder.h"
#include "../decoders/aac/aac_decoder.h"
#include "../decoders/alac/alac_decoder.h"
#include "../demux/mp4/mp4_demux.h"
#include "../decoders/ape/ape_decoder.h"
#include "../demux/ape/ape_demux.h"
#include "../decoders/wma/wma_decoder.h"
#include "../decoders/ogg/opus_decoder.h"
#include "../decoders/ogg/ogg_probe.h"
#include "../decoders/vorbis/vorbis_decoder.h"
#include "../decoders/wavpack/wavpack_decoder.h"
#include "../decoders/caf/caf_decoder.h"
#include "../effects/peq.h"
#include "../timing/audio_tempo.h"
#include "../../network/http/stream/http_stream.h"
#include "../../network/http/stream/http_stream_file.h"
#include "../../library/remote/remote_track.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../library/parsers/cue/cue_parser.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "audio_helpers.h"
#include "../../core/system/logging/debug_log.h"
#include "../../core/persistence/storage_paths.h"

#ifdef HOST_BUILD
  #include <SDL2/SDL.h>
#else
  #include "../output/audio_output.h"
#endif

/* --- Playback state ---
 *
 * A single playback thread lives for the app's whole lifetime (created once
 * by audio_init(), never joined) rather than one thread per track, so that
 * gapless and crossfade transitions between tracks never have to tear down
 * and reopen the output device or spin up a fresh thread. gui.c still owns
 * the playlist and its current index; this layer just plays "current" and,
 * optionally, knows about "next" (audio_set_next_track()) so it can prefetch
 * and, near current's natural end, either hand off seamlessly (gapless) or
 * blend the two (crossfade) entirely on its own -- no GUI round-trip.
 * Explicit user actions (tapping a different track, prev/next buttons, the
 * initial pick) always go through audio_play_file_at(), which interrupts
 * everything and restarts immediately with no fade, the same distinction
 * most real DAPs make between a manual skip and automatic progression. */

static pthread_t audio_thread;
pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;

static bool thread_started = false;

static bool have_current = false;   /* something loaded (playing or paused) */
bool stop_requested = false;
bool paused = false;
static bool track_finished = false; /* true EOF with no next track queued (or it failed to open) */
static bool track_advanced = false; /* thread moved on to the queued next track on its own */
static audio_error_t last_playback_error = AUDIO_ERROR_NONE;
static uint64_t last_playback_error_generation = 0;

bool restart_requested = false;
static bool startup_pending = false;
static bool startup_paused = false;
atomic_bool local_output_active;
static bool restart_read_replaygain = false;
static int restart_replaygain_mode;
static track_metadata_t startup_metadata;
static uint64_t startup_metadata_generation;
static bool startup_metadata_ready;
static uint64_t stopped_startup_generation;
static bool stopped_startup_generation_valid;
static uint64_t next_track_generation = 0;
static char * restart_path = NULL;  /* owned; consumed by the thread on restart */
static double restart_start_seconds = 0.0;
static float restart_replaygain_linear = 1.0f;
static bool restart_replaygain_applied = false;

static char * next_path = NULL;     /* owned; staged by audio_set_next_track(), NULL = none queued */
static float next_replaygain_linear = 1.0f;
static bool next_replaygain_applied = false;

static bool crossfade_enabled = false;
/* A-B loop request is expressed in seconds at the public boundary, then
 * converted to exact decoder PCM frames by the playback worker. */
static bool ab_loop_enabled = false;
static bool ab_switch_requested = false;
static double ab_loop_start_seconds = 0.0;
static double ab_loop_end_seconds = 0.0;
static uint64_t ab_loop_generation = 0;
static char speed_directory[1024] = "";
static double requested_playback_speed = 1.0;
static double applied_playback_speed = 1.0;
static unsigned speed_setting_serial = 0;

static const char * speed_normalize_path(const char * path) {
    if (path && strncmp(path, "/usr/data/mnt/sd_0/", sizeof("/usr/data/mnt/sd_0/") - 1) == 0) return path + 4;
    if (path && strcmp(path, "/usr/data/mnt/sd_0") == 0) return path + 4;
    return path;
}
/* Caller owns audio_mutex. Match a directory boundary, never a raw prefix. */
static double speed_for_path_locked(const char * path) {
    path = speed_normalize_path(path);
    size_t n = strlen(speed_directory);
    return path && n && strncmp(path, speed_directory, n) == 0 && path[n] == '/'
        ? requested_playback_speed : 1.0;
}
static double speed_for_decoder_locked(const char * path, const decoder_t * dec) {
    if (dec->type == DECODER_DSD || dec->net_stream || dec->channels > 2 ||
        dec->sample_rate < 8000 || dec->sample_rate > 96000) return 1.0;
    return speed_for_path_locked(path);
}
#define CROSSFADE_SECONDS 3.0

static bool low_power_mode = false;

/* Every explicit track start advances this generation. Pending seeks are
 * accepted only if they still belong to this exact track. */
uint64_t playback_generation = 0;
char * active_path = NULL; /* protected by audio_mutex */

/* Seeks are coalesced commands on the playback-owned decoder, so rapid taps
 * cannot stack decoder instances. */
bool seek_pending = false;
static uint64_t seek_pending_frame = 0;
static uint64_t seek_pending_playback_generation = 0;
static bool seek_pending_is_percent = false;
static double seek_pending_percent = 0.0;

static uint64_t frames_played = 0;
static uint64_t current_total_frames = 0;
static unsigned int current_sample_rate = 0;
/* The scalar counters above are legacy UI state. This generation and state
 * make them a trusted tuple for one playback request, including after a
 * natural EOF or explicit stop closes the live decoder. audio_mutex protects
 * all five fields together. */
static uint64_t counter_generation = 0;
static audio_playback_progress_state_t playback_progress_state = AUDIO_PLAYBACK_PROGRESS_NOT_READY;
static audio_current_format_info_t current_format_info;
static uint64_t current_format_generation = 0;

/* A/B MP3 seek-table scans also stop when their comparison epoch is cleared;
 * this independent token must not affect the primary playback index worker. */
static atomic_uint ab_switch_cancel_token = 0;

#ifndef HOST_BUILD
/* Wide-path scratch buffers, allocated once in audio_thread_func(). The
 * primary buffer is sufficient for ordinary S24 playback; the other two are
 * optional and needed only when two wide tracks are crossfaded. Keeping those
 * availability checks separate prevents a failed optional allocation from
 * disabling otherwise-valid 24-bit playback. */
static int32_t * buf_cur_s32 = NULL;
static int32_t * buf_next_s32 = NULL;
static int32_t * buf_out_s32 = NULL;

static bool can_use_wide_path(const decoder_t * dec) {
    if (!dec) return false;
    if (dec->net_stream != NULL) return false;
    /* DSD reports a 1-bit source but decimates to far more than 16 bits of
     * in-band resolution, so it always qualifies. */
    if (dec->type != DECODER_DSD && dec->source_bit_depth <= 16) return false;
    if (dec->type != DECODER_FLAC &&
        dec->type != DECODER_WAV &&
        dec->type != DECODER_ALAC &&
        dec->type != DECODER_APE &&
        dec->type != DECODER_WAVPACK &&
        dec->type != DECODER_CAF &&
        dec->type != DECODER_AIFF &&
        dec->type != DECODER_DSD) return false;
    if (!audio_output_supports_wide_path()) return false;
    if (!buf_cur_s32) return false;
    return true;
}

static bool can_use_wide_crossfade(const decoder_t * cur, const decoder_t * next) {
    return can_use_wide_path(cur) && can_use_wide_path(next) &&
           buf_next_s32 != NULL && buf_out_s32 != NULL;
}
#endif

#ifndef HOST_BUILD
/* Switches a freshly opened DSD decoder (still at frame 0) to native DoP
 * when the requested route permits a bit-exact DSD attempt. */
static void maybe_enable_dop(decoder_t * dec) {
    if (dec->type != DECODER_DSD || dec->net_stream ||
        dsd_is_dop(dec->as.dsd) || !buf_cur_s32) return;
    unsigned int rate = dsd_get_dop_sample_rate(dec->as.dsd);
    if (dec->channels != 2 || !audio_output_dsd_passthrough_requested(rate)) return;
    if (!decoder_set_dop(dec, true, 0, NULL)) (void) decoder_set_dop(dec, false, 0, NULL);
}

static void publish_current_format_locked(const decoder_t * dec, const char * path,
                                          float replaygain_linear, bool replaygain_applied);

/* Continues a DoP decoder as PCM from *frame_io (DoP frames in, PCM frames
 * out), for when the DAC refuses DoP or the route leaves the local DAC.
 * With publish_path, the current track's position, rate and format are
 * republished in the same locked step that converts a pending seek, so no
 * seek can be computed against the old timeline in between. */
static bool dop_fall_back_to_pcm(decoder_t * dec, uint64_t * frame_io, const char * publish_path,
                                 float replaygain_linear, bool replaygain_applied) {
    /* PCM after DoP must not reuse the low-pass from the PCM that preceded it. */
    peq_note_pcm_discontinuity();
    uint64_t pcm_frame = 0;
    unsigned int dop_rate = dec->sample_rate;
    bool ok = decoder_set_dop(dec, false, *frame_io, &pcm_frame);
    *frame_io = pcm_frame;
    pthread_mutex_lock(&audio_mutex);
    if (seek_pending && !seek_pending_is_percent && dop_rate)
        seek_pending_frame = seek_pending_frame * dec->sample_rate / dop_rate;
    if (publish_path && ok) {
        frames_played = pcm_frame;
        current_total_frames = dec->total_frames;
        current_sample_rate = dec->sample_rate;
        publish_current_format_locked(dec, publish_path, replaygain_linear, replaygain_applied);
    }
    pthread_mutex_unlock(&audio_mutex);
    DBG_LOG("audio: native DSD not available, converting to PCM from frame %" PRIu64 "\n", pcm_frame);
    return ok;
}
#endif

/* audio_mutex must be held. Publishing a copied scalar snapshot here keeps
 * the UI completely independent from the playback thread's stack-local
 * decoder_t and its short-lived gapless/crossfade prefetched decoder. */
static void publish_current_format_locked(const decoder_t * dec, const char * path,
                                          float replaygain_linear, bool replaygain_applied) {
    memset(&current_format_info, 0, sizeof(current_format_info));
    current_format_info.valid = true;
    if (path) snprintf(current_format_info.path, sizeof(current_format_info.path), "%s", path);
    current_format_info.codec = public_codec_for_decoder(dec);
    current_format_info.source_sample_rate = dec->source_sample_rate ? dec->source_sample_rate : dec->sample_rate;
    current_format_info.source_bit_depth = dec->source_bit_depth;
    current_format_info.output_sample_rate = dec->sample_rate;
#ifndef HOST_BUILD
    /* can_use_wide_path() alone is a prediction (source eligibility + what
     * output route is currently REQUESTED) -- audio_output_is_s24_active()
     * is the ground truth for what's actually open right now. They can
     * disagree: an S24_LE hw_params
     * negotiation failure that fell back to S16_LE (see audio_output.c's
     * open_device()), can leave can_use_wide_path() true while the device
     * is actually running at S16_LE. Requiring both here means the UI never
     * claims 24-bit output that isn't really happening (or, for piped output, that the
     * app didn't successfully request and spawn as a 24-bit stream). At a crossfade
     * promotion (cur_dec = nxt_dec) this can under-report by one chunk's
     * worth of time, since the device's format for the newly-promoted track
     * isn't renegotiated until the next chunk's own ensure_device_format()
     * call -- self-corrects immediately once that runs, not worth special-
     * casing for a display value. */
    current_format_info.output_bit_depth = (can_use_wide_path(dec) && audio_output_is_s24_active()) ? 24 : 16;
#else
    current_format_info.output_bit_depth = 16; /* host simulator SDL playback is always 16-bit */
#endif
    current_format_info.channels = dec->channels;
    current_format_info.bitrate_kbps = dec->bitrate_kbps;
    current_format_info.duration_seconds = dec->sample_rate > 0 && dec->total_frames > 0
        ? (double) dec->total_frames / (double) dec->sample_rate : 0.0;
    current_format_info.is_stream = dec->net_stream != NULL;
    current_format_info.seekable = dec->seekable && dec->total_frames > 0;
    current_format_info.is_dsd = dec->type == DECODER_DSD;
    current_format_info.dsd_native = decoder_is_dop(dec);
    current_format_info.replaygain_applied = replaygain_applied;
    current_format_info.replaygain_applied_db = replaygain_applied && replaygain_linear > 0.0f
        ? 20.0 * log10((double) replaygain_linear) : 0.0;
    current_format_info.generation = ++current_format_generation;
}

static void clear_current_format_locked(void) {
    memset(&current_format_info, 0, sizeof(current_format_info));
    current_format_generation++;
    playback_progress_state = AUDIO_PLAYBACK_PROGRESS_NOT_READY;
}

#ifndef HOST_BUILD
/* The gapless handoff's single open attempt, with the same DoP fallback. */
static bool ensure_device_for_decoder(decoder_t * dec, uint64_t * frame_io) {
    if (decoder_is_dop(dec)) {
        if (ensure_device_dop(dec->channels, dec->sample_rate)) return true;
        if (!dop_fall_back_to_pcm(dec, frame_io, NULL, 1.0f, false)) return false;
    }
    return ensure_device_format(dec->channels, dec->sample_rate, can_use_wide_path(dec));
}

/* Opens the output for dec. A DoP decoder the DAC refuses continues as
 * PCM from *frame_io (converted to PCM frames) and opens again. */
static write_result_t ensure_output_for_decoder(decoder_t * dec, uint64_t * frame_io, const char * path,
                                                bool allow_during_stop_restart) {
    if (decoder_is_dop(dec)) {
        write_result_t result = ensure_device_with_retry(dec->channels, dec->sample_rate, true, true,
                                                         path, allow_during_stop_restart);
        if (result != WRITE_RESULT_FAILED) return result;
        if (!dop_fall_back_to_pcm(dec, frame_io, NULL, 1.0f, false)) return WRITE_RESULT_FAILED;
    }
    return ensure_device_with_retry(dec->channels, dec->sample_rate, can_use_wide_path(dec), false,
                                    path, allow_during_stop_restart);
}

/* Decode/process/write one transition fade without narrowing an eligible S24
 * track. The active output check is intentional: local S24 negotiation
 * fallback can make the request differ from the actual format.
 * The s32 writer retains its S16 fallback and exact delivered-frame count. */
static uint64_t write_transition_fade(decoder_t * dec, uint64_t fade_frames,
                                      int16_t * buf_s16, int32_t * buf_s32,
                                      float replaygain_linear, float volume,
                                      const char * path) {
    /* A DoP stream cannot be faded; it simply stops. */
    if (decoder_is_dop(dec)) return 0;
    if (can_use_wide_path(dec) && audio_output_is_s24_active()) {
        decoder_read_result_t r = decoder_read_s32(dec, fade_frames, buf_s32);
        if (r.frames == 0) return 0;
        apply_gain_s32(buf_s32, (size_t) r.frames * dec->channels, replaygain_linear);
        peq_process_s32(buf_s32, (size_t) r.frames, (int) dec->channels, dec->sample_rate);
        apply_gain_s32(buf_s32, (size_t) r.frames * dec->channels, volume);
        apply_ramp_s32(buf_s32, r.frames, dec->channels, 1.0f, 0.0f);
        uint64_t delivered = 0;
        write_device_transition_ramp_s32(buf_s32, r.frames, dec->channels,
                                         dec->sample_rate, path, buf_s16, &delivered);
        return delivered;
    }

    decoder_read_result_t r = decoder_read_s16(dec, fade_frames, buf_s16);
    if (r.frames == 0) return 0;
    apply_gain(buf_s16, (size_t) r.frames * dec->channels, replaygain_linear);
    peq_process(buf_s16, (size_t) r.frames, (int) dec->channels, dec->sample_rate);
    apply_gain(buf_s16, (size_t) r.frames * dec->channels, volume);
    apply_ramp(buf_s16, r.frames, dec->channels, 1.0f, 0.0f);
    uint64_t delivered = 0;
    write_device_transition_ramp(buf_s16, r.frames, dec->channels,
                                 dec->sample_rate, path, &delivered);
    return delivered;
}
#endif /* !HOST_BUILD */

static void close_decoder_if_open(decoder_t * dec, bool * is_open) {
    if (*is_open) { decoder_close(dec); *is_open = false; }
}

/* Only the audio worker acknowledges a stop, after releasing its output.
 * A newer queued play keeps the device for reuse and owns its own state. */
static void finish_startup_without_restart(void) {
    pthread_mutex_lock(&audio_mutex);
    uint64_t observed_generation = playback_generation;
    bool restarting = restart_requested;
    pthread_mutex_unlock(&audio_mutex);
    if (restarting) return;
    close_device();
    pthread_mutex_lock(&audio_mutex);
    if (!restart_requested && playback_generation == observed_generation) {
        startup_pending = false;
        have_current = false;
        paused = false;
        stop_requested = false;
        clear_current_format_locked();
        pthread_cond_broadcast(&audio_cond);
    }
    pthread_mutex_unlock(&audio_mutex);
}

/* Loop clip (audio_loop_start()). The playback thread copies each chunk out
 * under audio_mutex, so start/stop may free a clip at any time. */
#define LOOP_CLIP_MAX_BYTES (4u * 1024u * 1024u)
#define LOOP_CLIP_CHUNK_FRAMES 1024u
#define LOOP_CLIP_RETRY_US 100000

typedef struct {
    int16_t * pcm;
    uint64_t frames;
    unsigned channels;
    unsigned rate;
} loop_clip_t;

static loop_clip_t * loop_clip = NULL;
static uint64_t loop_clip_serial = 0;

static void loop_clip_free(loop_clip_t * clip) {
    if (!clip) return;
    free(clip->pcm);
    free(clip);
}

static void loop_clip_clear_locked(void) {
    loop_clip_free(loop_clip);
    loop_clip = NULL;
    loop_clip_serial++;
    pthread_cond_broadcast(&audio_cond);
}

/* From a pause the loop yields to anything the paused wait must handle; the
 * caller's wait loop re-enters it afterwards. */
static bool loop_clip_may_run_locked(bool from_pause, unsigned tempo_seen) {
    if (!loop_clip || restart_requested || stop_requested) return false;
    if (!from_pause) return true;
    return paused && !seek_pending && !mp3_index_result &&
           tempo_seen == speed_setting_serial;
}

static bool loop_clip_write(const int16_t * buf, uint64_t frames, unsigned channels) {
#ifdef HOST_BUILD
    if (sdl_dev == 0) return false;
    SDL_QueueAudio(sdl_dev, buf, (Uint32) (frames * channels * sizeof(int16_t)));
    Uint32 max_queued_bytes = device_sample_rate * channels * sizeof(int16_t) / 4;
    while (SDL_GetQueuedAudioSize(sdl_dev) > max_queued_bytes) usleep(5000);
    return true;
#else
    uint64_t written = 0;
    return audio_output_write(buf, frames, channels, &written) && written == frames;
#endif
}

/* Playback thread only, from its idle and paused waits: plays the loop clip
 * until it is stopped or the wait has other work. Called and returns with
 * audio_mutex held. A track restart or resume ends the loop. */
static void run_loop_clip_locked(bool from_pause, unsigned tempo_seen) {
    int16_t * chunk = malloc((size_t) LOOP_CLIP_CHUNK_FRAMES * 2 * sizeof(int16_t));
    if (!chunk) {
        loop_clip_clear_locked();
        return;
    }
    uint64_t serial = loop_clip_serial - 1;
    uint64_t pos = 0;
    bool device_ready = false;
    while (loop_clip_may_run_locked(from_pause, tempo_seen)) {
        if (serial != loop_clip_serial) {
            serial = loop_clip_serial;
            pos = 0;
            device_ready = false;
        }
        unsigned channels = loop_clip->channels;
        unsigned rate = loop_clip->rate;
        uint64_t frames = 0;
        while (frames < LOOP_CLIP_CHUNK_FRAMES) {
            uint64_t n = loop_clip->frames - pos;
            if (n > LOOP_CLIP_CHUNK_FRAMES - frames) n = LOOP_CLIP_CHUNK_FRAMES - frames;
            memcpy(chunk + frames * channels, loop_clip->pcm + pos * channels,
                   (size_t) n * channels * sizeof(int16_t));
            frames += n;
            pos += n;
            if (pos == loop_clip->frames) pos = 0;
        }
        float gain = volume_gain;
        pthread_mutex_unlock(&audio_mutex);

        apply_gain(chunk, (size_t) frames * channels, gain);
        if (!device_ready) device_ready = ensure_device(channels, rate);
        if (!device_ready || !loop_clip_write(chunk, frames, channels)) {
            /* Another producer owns the output, or it failed: retry later. */
            close_device();
            device_ready = false;
            usleep(LOOP_CLIP_RETRY_US);
        }
        pthread_mutex_lock(&audio_mutex);
    }
    pthread_mutex_unlock(&audio_mutex);
    close_device();
    free(chunk);
    pthread_mutex_lock(&audio_mutex);
    if (restart_requested || (from_pause && !paused && !stop_requested))
        loop_clip_clear_locked();
}

/* A comparison keeps the primary track identity and DSP/gain. Preparation
 * owns a second decoder off-thread; only the playback thread adopts/reads it.
 * Selection changes the raw buffer, never the output device or decoder cursor.
 */
typedef struct {
    decoder_t decoder;
    char * path;
    uint64_t epoch, generation;
    unsigned cancel_token;
    unsigned rate, channels;
    uint64_t frames;
    bool opened;
} ab_switch_job_t;
static uint64_t ab_switch_epoch;
static uint64_t ab_switch_generation;
static bool ab_switch_worker_active;
static bool ab_switch_ready;
static bool ab_switch_select_b;
static ab_switch_job_t * ab_switch_result;

static void ab_switch_clear_locked(void) {
    ab_switch_epoch++;
    atomic_fetch_add_explicit(&ab_switch_cancel_token, 1u, memory_order_relaxed);
    ab_switch_requested = false;
    ab_switch_ready = false;
    ab_switch_select_b = false;
}
static bool ab_switch_decoder_supported(const decoder_t * d) {
    bool supported_pcm = d && d->source_bit_depth == 16 &&
           (d->type == DECODER_FLAC || d->type == DECODER_WAV ||
            d->type == DECODER_AIFF || d->type == DECODER_CAF || d->type == DECODER_ALAC);
    /* MP3 has no meaningful encoded sample bit depth. Keep it eligible by
     * codec and its actual finite local PCM frame count instead of assigning
     * a fictitious 16-bit source depth. */
    bool supported_mp3 = d && d->type == DECODER_MP3 && d->source_bit_depth == 0;
    return d && !d->net_stream && d->total_frames && d->sample_rate &&
           d->channels > 0 && d->channels <= MAX_CHANNELS &&
           (supported_pcm || supported_mp3);
}
static void ab_switch_free_job(ab_switch_job_t * job) {
    if (!job)
        return;
    if (job->opened)
        decoder_close(&job->decoder);
    free(job->path);
    free(job);
}
static void * ab_switch_prepare_worker(void * opaque) {
    ab_switch_job_t * job = opaque;
    set_worker_thread_name("audio-ab-prepare");
    install_thread_crash_altstack();
    job->opened = decoder_open_for_generation(&job->decoder, job->path, job->generation);
    bool index_ready = true;
    /* A long MP3 needs its reservoir-aware seek table before adoption so
     * the playback thread never has to build an index or brute-force seek. */
    if (job->opened && job->decoder.type == DECODER_MP3 &&
        mp3_needs_seek_index(&job->decoder)) {
        uint64_t interval = (uint64_t) job->decoder.sample_rate * MP3_SEEK_INDEX_INTERVAL_SECONDS;
        uint64_t desired = job->decoder.total_frames / interval;
        if (desired > MP3_SEEK_INDEX_MAX_POINTS) desired = MP3_SEEK_INDEX_MAX_POINTS;
        drmp3_seek_point * points = desired ? calloc((size_t) desired, sizeof(*points)) : NULL;
        drmp3 * scan = malloc(sizeof(*scan));
        bool indexed = false;
        if (points && scan && drmp3_init_file(scan, job->path, NULL)) {
            bool cancelled = false;
            uint64_t index_generation = atomic_load_explicit(&mp3_index_active_generation, memory_order_relaxed);
            mp3_index_cancel_t cancel = { .generation = index_generation,
                                          .token = &ab_switch_cancel_token,
                                          .expected_token = job->cancel_token };
            drmp3_uint32 count = mp3_build_seek_points_cancellable(scan, job->decoder.total_frames,
                                        (drmp3_uint32) desired, points, &cancelled,
                                        mp3_index_cancelled, &cancel);
            if (count && !cancelled && drmp3_bind_seek_table(job->decoder.as.mp3, count, points)) {
                job->decoder.mp3_seek_points = points;
                job->decoder.mp3_seek_point_count = count;
                job->decoder.mp3_seek_index_attempted = true;
                points = NULL;
                indexed = true;
            }
            drmp3_uninit(scan);
        }
        free(scan);
        free(points);
        if (!indexed) index_ready = false;
    }
    bool valid = job->opened && index_ready && ab_switch_decoder_supported(&job->decoder) &&
                 job->decoder.sample_rate == job->rate &&
                 job->decoder.channels == job->channels &&
                 job->decoder.total_frames == job->frames;
    pthread_mutex_lock(&audio_mutex);
    bool current = ab_switch_requested && ab_switch_epoch == job->epoch &&
                   playback_generation == job->generation && !stop_requested &&
                   !restart_requested;
    if (current && valid) {
        ab_switch_result = job;
        job = NULL;
    } else if (current) {
        ab_switch_clear_locked();
    }
    ab_switch_worker_active = false;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    ab_switch_free_job(job);
    return NULL;
}

bool audio_prepare_ab_switch(const char * path) {
    if (!path || path[0] != '/' || strlen(path) >= 2048)
        return false;
    pthread_mutex_lock(&audio_mutex);
    bool allowed =
        have_current && current_format_info.valid &&
        !current_format_info.is_stream && !current_format_info.is_dsd &&
        ((current_format_info.source_bit_depth == 16 &&
          (current_format_info.codec == AUDIO_CODEC_PCM ||
           current_format_info.codec == AUDIO_CODEC_FLAC)) ||
         (current_format_info.codec == AUDIO_CODEC_MP3 &&
          current_format_info.source_bit_depth == 0)) &&
        current_total_frames && current_sample_rate && !crossfade_enabled &&
        !ab_loop_enabled && !paused && applied_playback_speed == 1.0 &&
        speed_for_path_locked(active_path) == 1.0 && !ab_switch_worker_active &&
        !ab_switch_result;
    if (!allowed) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }
    ab_switch_job_t * job = calloc(1, sizeof(*job));
    if (!job) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }
    job->path = strdup(path);
    if (!job->path) {
        free(job);
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }
    ab_switch_clear_locked();
    job->epoch = ab_switch_epoch;
    job->cancel_token = atomic_load_explicit(&ab_switch_cancel_token, memory_order_relaxed);
    job->generation = playback_generation;
    job->rate = current_sample_rate;
    job->channels = current_format_info.channels;
    job->frames = current_total_frames;
    ab_switch_generation = playback_generation;
    ab_switch_requested = true;
    ab_switch_worker_active = true;
    pthread_t worker;
    int rc = pthread_create(&worker, NULL, ab_switch_prepare_worker, job);
    if (rc != 0) {
        ab_switch_worker_active = false;
        ab_switch_clear_locked();
        pthread_mutex_unlock(&audio_mutex);
        ab_switch_free_job(job);
        return false;
    }
    pthread_detach(worker);
    pthread_mutex_unlock(&audio_mutex);
    return true;
}
/* audio_mutex must be held. Active, unpaused, no seek or stop in flight, and
 * neither crossfade nor tempo changes the output. */
static bool playback_plain_1x_locked(void) {
    return !stop_requested && !restart_requested && !seek_pending && !paused &&
           !crossfade_enabled && applied_playback_speed == 1.0 &&
           speed_for_path_locked(active_path) == 1.0;
}

bool audio_select_ab_source(bool source_b) {
    pthread_mutex_lock(&audio_mutex);
    bool ready = ab_switch_requested && ab_switch_ready &&
                 ab_switch_generation == playback_generation && playback_plain_1x_locked();
    if (ready)
        ab_switch_select_b = source_b;
    pthread_mutex_unlock(&audio_mutex);
    return ready;
}
void audio_clear_ab_switch(void) {
    pthread_mutex_lock(&audio_mutex);
    ab_switch_clear_locked();
    ab_switch_job_t * pending = ab_switch_result;
    ab_switch_result = NULL;
    pthread_mutex_unlock(&audio_mutex);
    ab_switch_free_job(pending);
}
void audio_get_ab_switch(bool * preparing, bool * ready, bool * source_b) {
    pthread_mutex_lock(&audio_mutex);
    bool current = ab_switch_requested && ab_switch_generation == playback_generation &&
                   playback_plain_1x_locked();
    if (preparing)
        *preparing = current && !ab_switch_ready;
    if (ready)
        *ready = current && ab_switch_ready;
    if (source_b)
        *source_b = current && ab_switch_ready && ab_switch_select_b;
    pthread_mutex_unlock(&audio_mutex);
}

/* Called only before decoding a plain S16 chunk. The alignment seek happens
 * once during adoption, not during selection. A failed/stale candidate has
 * no effect on the primary decoder or its output. */
static void ab_switch_adopt(ab_switch_job_t ** active, decoder_t * primary,
                            uint64_t frame, uint64_t generation) {
    pthread_mutex_lock(&audio_mutex);
    ab_switch_job_t * candidate = ab_switch_result;
    ab_switch_result = NULL;
    bool eligible = ab_switch_requested && ab_switch_generation == generation &&
                    !paused && !stop_requested && !restart_requested &&
                    !seek_pending && !crossfade_enabled && !ab_loop_enabled &&
                    applied_playback_speed == 1.0 &&
                    ab_switch_decoder_supported(primary);
    uint64_t epoch = ab_switch_epoch;
    pthread_mutex_unlock(&audio_mutex);
    if (*active && (!eligible || (*active)->epoch != epoch)) {
        ab_switch_free_job(*active);
        *active = NULL;
    }
    if (!candidate)
        return;
    bool aligned = eligible && candidate->epoch == epoch &&
                   candidate->generation == generation &&
                   decoder_seek(&candidate->decoder, frame);
    pthread_mutex_lock(&audio_mutex);
    aligned = aligned && ab_switch_requested && ab_switch_epoch == epoch &&
              playback_generation == generation && !seek_pending && !paused &&
              !stop_requested && !restart_requested && !crossfade_enabled;
    if (aligned)
        ab_switch_ready = true;
    else if (ab_switch_epoch == candidate->epoch)
        ab_switch_clear_locked();
    pthread_mutex_unlock(&audio_mutex);
    if (aligned) {
        ab_switch_free_job(*active);
        *active = candidate;
    } else
        ab_switch_free_job(candidate);
}
/* Edge of an A-B source change. Continuous blocks of the same source keep
 * crossfeed memory; only the switch (or dropping back to the primary) notes
 * a break. The playback thread is the only writer. */
static bool ab_pcm_alternate = false;

static void ab_pcm_use_primary(void) {
    if (!ab_pcm_alternate) return;
    ab_pcm_alternate = false;
    peq_note_pcm_discontinuity();
}

static void ab_pcm_use_alternate(void) {
    if (ab_pcm_alternate) return;
    ab_pcm_alternate = true;
    peq_note_pcm_discontinuity();
}

static void ab_switch_mix_source(ab_switch_job_t ** active, int16_t * primary,
                                 int16_t * alternate, uint64_t frames,
                                 unsigned channels) {
    if (!*active) {
        ab_pcm_use_primary();
        return;
    }
    if (!frames)
        return;
    decoder_read_result_t r =
        decoder_read_s16(&(*active)->decoder, frames, alternate);
    pthread_mutex_lock(&audio_mutex);
    bool current = ab_switch_requested && ab_switch_ready &&
                   ab_switch_epoch == (*active)->epoch &&
                   playback_generation == (*active)->generation;
    bool valid = current && r.frames == frames && r.status == DECODER_READ_OK;
    bool select_b = valid && ab_switch_select_b;
    if (!valid && current)
        ab_switch_clear_locked();
    pthread_mutex_unlock(&audio_mutex);
    if (!valid) {
        ab_switch_free_job(*active);
        *active = NULL;
        ab_pcm_use_primary();
        return;
    }
    if (select_b) {
        ab_pcm_use_alternate();
        memcpy(primary, alternate, (size_t)frames * channels * sizeof(*primary));
    } else {
        ab_pcm_use_primary();
    }
}

/* Ends playback of the current track with `err`, as the failure paths of
 * the playback loop do. The caller then leaves the loop. */
static void abort_playback_locked(audio_error_t err, uint64_t generation) {
    last_playback_error = err;
    last_playback_error_generation = generation;
    have_current = false;
    clear_current_format_locked();
    paused = false;
}

static void abort_playback(audio_error_t err, uint64_t generation) {
    pthread_mutex_lock(&audio_mutex);
    abort_playback_locked(err, generation);
    pthread_mutex_unlock(&audio_mutex);
}

/* Records `err` for a startup that failed, unless the request was already
 * superseded or cancelled, and ends the startup. */
static void fail_startup(uint64_t generation, audio_error_t err) {
    pthread_mutex_lock(&audio_mutex);
    if (playback_generation == generation && !restart_requested && !stop_requested) {
        startup_pending = false;
        have_current = false;
        clear_current_format_locked();
        last_playback_error = err;
        last_playback_error_generation = generation;
    }
    pthread_mutex_unlock(&audio_mutex);
    finish_startup_without_restart();
}

/* True while the next track the playback loop snapshotted is still the one
 * queued, and the current request is still active. */
static bool staged_next_valid_locked(uint64_t staged_generation, const char * staged_path,
                                     uint64_t generation) {
    return staged_generation == next_track_generation &&
           playback_generation == generation &&
           !stop_requested && !restart_requested &&
           staged_path != NULL && next_path != NULL &&
           strcmp(next_path, staged_path) == 0;
}

/* Plays the controlled ramp-down before a manual stop, restart or seek. */
static void fade_out_for_transition(decoder_t * dec, int16_t * buf, float replaygain_linear,
                                    float vol, const char * path) {
    uint64_t rf = calculate_ramp_frames(dec->sample_rate);
#ifndef HOST_BUILD
    (void) write_transition_fade(dec, rf, buf, buf_cur_s32, replaygain_linear, vol, path);
#else
    (void) path;
    decoder_read_result_t r_fade = decoder_read_s16(dec, rf, buf);
    if (r_fade.frames > 0) {
        apply_gain(buf, (size_t) r_fade.frames * dec->channels, replaygain_linear);
        peq_process(buf, (size_t) r_fade.frames, (int) dec->channels, dec->sample_rate);
        apply_gain(buf, (size_t) r_fade.frames * dec->channels, vol);
        apply_ramp(buf, r_fade.frames, dec->channels, 1.0f, 0.0f);
        write_device(buf, r_fade.frames, dec->channels);
    }
#endif
}

/* Fades in the first frames of a buffer after a start, seek or resume. */
static void fade_in_s16(int16_t * buf, uint64_t frames, unsigned int channels, unsigned int sample_rate) {
    uint64_t rf = calculate_ramp_frames(sample_rate);
    apply_ramp(buf, frames < rf ? frames : rf, channels, 0.0f, 1.0f);
}

#ifndef HOST_BUILD
static void fade_in_s32(int32_t * buf, uint64_t frames, unsigned int channels, unsigned int sample_rate) {
    uint64_t rf = calculate_ramp_frames(sample_rate);
    apply_ramp_s32(buf, frames < rf ? frames : rf, channels, 0.0f, 1.0f);
}
#endif

/* Discards the time-stretcher and its buffered input. */
static void tempo_drop(audio_tempo_t ** tempo, size_t * pending_frames) {
    audio_tempo_destroy(*tempo);
    *tempo = NULL;
    *pending_frames = 0;
}

/* State of the audio thread that outlives a single chunk. The first group
 * lives for the whole thread; the second group is reset at the start of each
 * track (see the "per-track" initialisation after the output is open). */
typedef struct {
    /* Thread lifetime. */
    audio_tempo_t * tempo;
    double tempo_rate;
    uint64_t tempo_generation;
    size_t tempo_pending_frames;
    unsigned tempo_eof_retries;
    bool tempo_recovery_pending;
    audio_error_t tempo_error;
    unsigned tempo_setting_seen;
    bool tempo_finished;
    ab_switch_job_t * ab_active;
    decoder_t cur_dec;
    bool cur_open;
    char * cur_path_local;
    uint64_t cur_frames_played_local;
    float cur_replaygain_linear;
    bool cur_replaygain_applied;
    decoder_t nxt_dec;
    bool nxt_open;
    bool nxt_format_matches;
    float nxt_replaygain_linear_local;
    bool nxt_replaygain_applied_local;
    uint64_t nxt_frames_consumed;
    int16_t * buf_cur;
    int16_t * buf_next;
    int16_t * buf_out;
    /* Per start (assigned where the request is taken or the output opened). */
    uint64_t cur_generation;
#ifndef HOST_BUILD
    bool startup_pause_no_fade;
#endif
    /* Per track (reset after the start is published). */
    bool should_restart;
    bool was_stopped;
    bool ended_with_no_next;
    bool natural_eof;
    bool need_fade_in;
    bool mp3_seek_output_held;
    unsigned int consecutive_decoder_errors;
    unsigned int consecutive_nxt_decoder_errors;
} playback_session_t;

/* One chunk's commands, snapshotted under audio_mutex at the top of the
 * playback loop. Never copied: the staged path is inline (4 KiB) and only the
 * scalars are initialised, see chunk_staged_path(). */
typedef struct {
    bool do_stop;
    bool do_restart;
    bool do_seek;
    uint64_t seek_frame;
    double wanted_speed;
    mp3_index_job_t * completed_index;
    bool hold_for_mp3_seek;
    bool xfade_on;
    float vol;
    bool remain_paused;
    uint64_t chunk_frames;
    next_track_snapshot_t staged_next;
} chunk_cmd_t;

/* The queued next track's path, or NULL when none was staged. */
static inline const char * chunk_staged_path(const chunk_cmd_t * c) {
    return c->staged_next.valid ? c->staged_next.path : NULL;
}

/* Where the current track is inside the crossfade window. */
typedef struct {
    uint64_t frames_remaining;
    uint64_t crossfade_frames;
    bool in_blend_window;
} blend_window_t;

/* Publishes the audible position. Enters and returns unlocked. */
static void publish_frames_played(uint64_t frames) {
    pthread_mutex_lock(&audio_mutex);
    frames_played = frames;
    pthread_mutex_unlock(&audio_mutex);
}

/* Reads the current track's ReplayGain tags and hands the metadata to a
 * waiting startup request when it is still current. Enters and returns
 * unlocked. */
static void session_read_replaygain_tags(playback_session_t * s, int replaygain_mode) {
    track_metadata_t tags;
    metadata_read_without_artwork(s->cur_path_local, &tags);
    bool album = replaygain_mode == 2 && tags.has_replaygain_album;
    bool has_gain = replaygain_mode != 0 && (album || tags.has_replaygain);
    double gain = album ? tags.replaygain_album_gain_db : tags.replaygain_gain_db;
    bool album_peak = album && tags.has_replaygain_album_peak;
    bool has_peak = replaygain_mode != 0 && (album_peak || tags.has_replaygain_peak);
    double peak = album_peak ? tags.replaygain_album_peak : tags.replaygain_peak;
    s->cur_replaygain_linear = (float) replaygain_to_linear(has_gain, gain, has_peak, peak);
    s->cur_replaygain_applied = has_gain;
    free(tags.picture_data);
    free(tags.lyrics);
    tags.picture_data = NULL;
    tags.picture_size = 0;
    tags.lyrics = NULL;
    pthread_mutex_lock(&audio_mutex);
    if (playback_generation == s->cur_generation && !stop_requested && !restart_requested) {
        startup_metadata = tags;
        startup_metadata_generation = s->cur_generation;
        startup_metadata_ready = true;
    }
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

/* Restarts the time-stretcher state when the track generation moved.
 * Enters and returns unlocked. */
static void tempo_sync_generation(playback_session_t * s) {
    if (s->tempo_generation != s->cur_generation) {
        tempo_drop(&s->tempo, &s->tempo_pending_frames);
        s->tempo_finished = false;
        s->tempo_generation = s->cur_generation;
        s->tempo_eof_retries = 0;
        s->tempo_recovery_pending = false;
        s->tempo_error = AUDIO_ERROR_DECODER_FAILED;
        pthread_mutex_lock(&audio_mutex);
        s->tempo_rate = speed_for_decoder_locked(s->cur_path_local, &s->cur_dec);
        s->tempo_setting_seen = speed_setting_serial;
        applied_playback_speed = 1.0;
        pthread_mutex_unlock(&audio_mutex);
    }
}

/* Drops a stale or finished A/B switch job; the frees happen between two
 * short locked spans. Enters and returns unlocked. */
static void ab_switch_housekeeping(playback_session_t * s) {
    pthread_mutex_lock(&audio_mutex);
    if (ab_switch_requested &&
        (ab_switch_generation != s->cur_generation || paused || stop_requested ||
         restart_requested || seek_pending || crossfade_enabled ||
         speed_for_path_locked(s->cur_path_local) != 1.0))
        ab_switch_clear_locked();
    bool ab_keep = ab_switch_requested && s->ab_active &&
                   s->ab_active->epoch == ab_switch_epoch;
    ab_switch_job_t * ab_discard =
        !ab_switch_requested ? ab_switch_result : NULL;
    if (ab_discard)
        ab_switch_result = NULL;
    pthread_mutex_unlock(&audio_mutex);
    if (s->ab_active && !ab_keep) {
        ab_switch_free_job(s->ab_active);
        s->ab_active = NULL;
        ab_pcm_use_primary();
    }
    ab_switch_free_job(ab_discard);
}

/* The request that started the current track. */
typedef struct {
    double start_seconds;
    bool read_replaygain;
    int replaygain_mode;
} startup_request_t;

/* Idles until a track is requested (serving stop and loop-clip commands on
 * the way), then takes the request. Enters and returns unlocked. */
static void session_wait_for_request(playback_session_t * s, startup_request_t * req) {
    /* Idle until there's a track to (re)start. */
    pthread_mutex_lock(&audio_mutex);
    while (!restart_requested) {
        if (stop_requested) {
            pthread_mutex_unlock(&audio_mutex);
            finish_startup_without_restart();
            pthread_mutex_lock(&audio_mutex);
        } else if (loop_clip) {
            run_loop_clip_locked(false, 0);
        } else pthread_cond_wait(&audio_cond, &audio_mutex);
    }
    restart_requested = false;
    /* New playback is not a continuation of the low-pass left by stop. */
    ab_pcm_alternate = false;
    peq_note_pcm_discontinuity();
    free(s->cur_path_local);
    s->cur_path_local = restart_path; restart_path = NULL; /* ownership transferred */
    req->start_seconds = restart_start_seconds;
    s->cur_replaygain_linear = restart_replaygain_linear;
    s->cur_replaygain_applied = restart_replaygain_applied;
    s->cur_generation = playback_generation;
    req->read_replaygain = restart_read_replaygain;
    req->replaygain_mode = restart_replaygain_mode;
    pthread_mutex_unlock(&audio_mutex);
}

/* Closes what the previous track left open, reads the tags and opens the
 * decoder. False means the start was abandoned (already reported); the caller
 * takes the next request. Enters and returns unlocked. */
static bool session_open_current(playback_session_t * s, const startup_request_t * req) {
    close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
    s->nxt_format_matches = false;
    if (s->cur_open) { decoder_close(&s->cur_dec); s->cur_open = false; }

    if (req->read_replaygain && s->cur_path_local) session_read_replaygain_tags(s, req->replaygain_mode);
    /* Reading a cold file may take seconds. Never open/output a request
     * that Stop or another track has superseded during that read. */
    pthread_mutex_lock(&audio_mutex);
    bool startup_cancelled = playback_generation != s->cur_generation || stop_requested || restart_requested;
    pthread_mutex_unlock(&audio_mutex);
    if (startup_cancelled) { finish_startup_without_restart(); return false; }

    if (!s->cur_path_local || !decoder_open_for_generation(&s->cur_dec, s->cur_path_local, s->cur_generation)) {
        if (s->cur_path_local)
            fprintf(stderr, "audio: playback open rejected (%s): %s\n",
                    safe_path_tail(s->cur_path_local), audio_error_description(s->cur_dec.open_error));
        fail_startup(s->cur_generation, s->cur_path_local ? s->cur_dec.open_error : AUDIO_ERROR_DECODER_FAILED);
        return false;
    }
    s->cur_open = true;
    s->cur_frames_played_local = 0;
#ifndef HOST_BUILD
    maybe_enable_dop(&s->cur_dec);
#endif

    /* Proactively request an index for every eligible MP3 right at open,
     * not just when resuming at a non-zero position -- a cache hit (see
     * mp3_seek_cache_load()) resolves instantly, and even a fresh scan is
     * usually done well before the user taps the progress bar, since it
     * runs on its own thread and never blocks playback starting at frame
     * 0 here. No defer/hold for this fire-and-forget request -- only an
     * actual seek target (below, or a later interactive seek) holds
     * output waiting for it. */
    if (mp3_needs_seek_index(&s->cur_dec)) {
        pthread_mutex_lock(&audio_mutex);
        request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
        pthread_mutex_unlock(&audio_mutex);
    }

    return true;
}

/* Applies the resume position, or defers it for an MP3 still being indexed.
 * False means the start was abandoned. Enters and returns unlocked. */
static bool session_apply_initial_seek(playback_session_t * s, const startup_request_t * req,
                                       bool * initial_mp3_seek_deferred) {
    if (s->cur_dec.seekable && isfinite(req->start_seconds) && req->start_seconds > 0.0) {
        double bounded_seconds = req->start_seconds;
        double duration_seconds = (double) s->cur_dec.total_frames / (double) s->cur_dec.sample_rate;
        if (bounded_seconds > duration_seconds) bounded_seconds = duration_seconds;
        uint64_t start_frame = (uint64_t) (bounded_seconds * (double) s->cur_dec.sample_rate);
        if (mp3_needs_seek_index(&s->cur_dec) && !s->cur_dec.mp3_seek_points && !s->cur_dec.mp3_seek_index_attempted) {
            pthread_mutex_lock(&audio_mutex);
            defer_mp3_seek_locked(&s->cur_dec, s->cur_generation, start_frame);
            pthread_mutex_unlock(&audio_mutex);
            *initial_mp3_seek_deferred = true;
        }
        if (*initial_mp3_seek_deferred) {
            DBG_LOG("audio: initial MP3 seek deferred until index is ready (%s)\n",
                    safe_path_tail(s->cur_path_local));
        } else if (decoder_seek(&s->cur_dec, start_frame)) {
            /* Once an index attempt has resolved either way,
             * decoder_seek() proceeds via the table if one was bound, or
             * brute force as a last resort if indexing permanently
             * failed -- never silently skipped (see its own comment). */
            s->cur_frames_played_local = start_frame;
        } else {
            DBG_LOG("audio: initial seek to frame %" PRIu64 " failed (%s), playing from start\n",
                    start_frame, safe_path_tail(s->cur_path_local));
            /* Seeking can mutate a decoder before reporting failure
             * (notably AAC and Opus). A clean reopen makes the promised
             * start-from-zero fallback real and prevents a bad persisted
             * resume position from crashing again on every boot. */
            if (!reopen_decoder_at(&s->cur_dec, s->cur_path_local, 0)) {
                s->cur_open = false;
                fail_startup(s->cur_generation, AUDIO_ERROR_DECODER_FAILED);
                return false;
            }
            s->cur_frames_played_local = 0;
        }
    }

    return true;
}

/* Opens the output for the new track unless it starts paused. False means
 * the start was abandoned. Enters and returns unlocked. */
static bool session_open_initial_output(playback_session_t * s, bool initial_mp3_seek_deferred) {
    pthread_mutex_lock(&audio_mutex);
    bool pause_before_output = startup_paused;
    pthread_mutex_unlock(&audio_mutex);
#ifndef HOST_BUILD
    /* Reset S24 probe cache before ensuring format so each new track
     * can negotiate S24_LE output. */
    audio_output_reset_s24_probe();
    /* A new DoP decoder starts its markers at 0x05; a DoP stream left
     * open by the previous track may expect 0xFA next. */
    if (decoder_is_dop(&s->cur_dec) && audio_output_is_dop_active()) close_device();
    write_result_t initial_output = pause_before_output ? WRITE_RESULT_OK :
        ensure_output_for_decoder(&s->cur_dec, &s->cur_frames_played_local, s->cur_path_local, false);
#else
    (void) pause_before_output;
    (void) initial_mp3_seek_deferred;
    bool initial_output_ok = ensure_device(s->cur_dec.channels, s->cur_dec.sample_rate);
#endif
#ifndef HOST_BUILD
    if (initial_output != WRITE_RESULT_OK) {
        bool stale_or_cancelled = initial_output == WRITE_RESULT_ABORTED;
        decoder_close(&s->cur_dec);
        s->cur_open = false;
        pthread_mutex_lock(&audio_mutex);
        bool stale_generation = playback_generation != s->cur_generation;
        bool cancelled = stop_requested || restart_requested;
        if (!stale_or_cancelled && !stale_generation && !cancelled) {
            fprintf(stderr, "audio: output open failed (%s): %u channels, %u Hz\n",
                    safe_path_tail(s->cur_path_local), s->cur_dec.channels, s->cur_dec.sample_rate);
            startup_pending = false;
            have_current = false;
            clear_current_format_locked();
            last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
            last_playback_error_generation = s->cur_generation;
        }
        pthread_mutex_unlock(&audio_mutex);
        finish_startup_without_restart();
        return false;
    }
#else
    if (!initial_output_ok) {
        decoder_close(&s->cur_dec);
        s->cur_open = false;
        fail_startup(s->cur_generation, AUDIO_ERROR_OUTPUT_FAILED);
        return false;
    }
#endif
#ifndef HOST_BUILD
    if (initial_mp3_seek_deferred) close_device();
#endif

    return true;
}

/* Publishes the new track as current and resets the per-track state. False
 * means a stop or newer request superseded it. Enters and returns unlocked. */
static bool session_publish_start(playback_session_t * s, bool initial_mp3_seek_deferred) {
    pthread_mutex_lock(&audio_mutex);
    bool initial_request_stale = playback_generation != s->cur_generation ||
                                 stop_requested || restart_requested;
    if (initial_request_stale) {
        pthread_mutex_unlock(&audio_mutex);
        decoder_close(&s->cur_dec);
        s->cur_open = false;
        finish_startup_without_restart();
        return false;
    }
    if (startup_pending) paused = startup_paused;
#ifndef HOST_BUILD
    s->startup_pause_no_fade = paused;
#endif
    startup_pending = false;
    have_current = true;
    frames_played = s->cur_frames_played_local;
    current_total_frames = s->cur_dec.total_frames;
    current_sample_rate = s->cur_dec.sample_rate;
    counter_generation = s->cur_generation;
    playback_progress_state = current_sample_rate > 0
        ? AUDIO_PLAYBACK_PROGRESS_ACTIVE : AUDIO_PLAYBACK_PROGRESS_NOT_READY;
    publish_current_format_locked(&s->cur_dec, s->cur_path_local,
                                  s->cur_replaygain_linear, s->cur_replaygain_applied);
    pthread_mutex_unlock(&audio_mutex);

    s->should_restart = false;
    s->was_stopped = false;
    s->ended_with_no_next = false;
    s->natural_eof = false;
    s->need_fade_in = true;
    s->mp3_seek_output_held = initial_mp3_seek_deferred;
    s->consecutive_decoder_errors = 0;
    s->consecutive_nxt_decoder_errors = 0;
    return true;
}

/* Brings the requested track from the request to playing. False means the
 * start was abandoned (already reported). */
static bool session_start_track(playback_session_t * s, const startup_request_t * req) {
    if (!session_open_current(s, req)) return false;
    bool initial_mp3_seek_deferred = false;
    if (!session_apply_initial_seek(s, req, &initial_mp3_seek_deferred)) return false;
    if (!session_open_initial_output(s, initial_mp3_seek_deferred)) return false;
    return session_publish_start(s, initial_mp3_seek_deferred);
}

/* Releases the per-track playback helpers once the playback loop is left. */
static void session_after_track_loop(playback_session_t * s) {
    audio_clear_ab_switch();
    ab_switch_free_job(s->ab_active);
    s->ab_active = NULL;
    tempo_drop(&s->tempo, &s->tempo_pending_frames);
    s->tempo_finished = false;
    pthread_mutex_lock(&audio_mutex);
    applied_playback_speed = 1.0;
    pthread_mutex_unlock(&audio_mutex);
}

/* Closes the finished track and publishes the terminal state. Enters and
 * returns unlocked. */
static void session_end_track(playback_session_t * s) {
    close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
    if (s->cur_open) { decoder_close(&s->cur_dec); s->cur_open = false; }
    close_device();
    free(s->cur_path_local);
    s->cur_path_local = NULL;

    pthread_mutex_lock(&audio_mutex);
    /* have_current was already cleared in the error gotos above; for
     * normal break paths (stop, natural EOF) it still needs clearing. */
    audio_playback_progress_state_t terminal_progress = AUDIO_PLAYBACK_PROGRESS_NOT_READY;
    if (s->was_stopped) terminal_progress = AUDIO_PLAYBACK_PROGRESS_MANUAL_STOP;
    else if (s->natural_eof) terminal_progress = AUDIO_PLAYBACK_PROGRESS_NATURAL_EOF;
    bool preserve_terminal_progress = terminal_progress != AUDIO_PLAYBACK_PROGRESS_NOT_READY &&
        playback_generation == s->cur_generation && counter_generation == s->cur_generation &&
        playback_progress_state == AUDIO_PLAYBACK_PROGRESS_ACTIVE &&
        !(mp3_seek_deferred && mp3_seek_deferred_generation == s->cur_generation) &&
        last_playback_error_generation != s->cur_generation;
    have_current = false;
    clear_current_format_locked();
    paused = false;
    if (preserve_terminal_progress) playback_progress_state = terminal_progress;
    if (s->ended_with_no_next) track_finished = true;
    if (s->was_stopped) stop_requested = false;
    pthread_mutex_unlock(&audio_mutex);
}

/* Steps out of an extracted piece of the playback loop body: carry on with
 * the rest of the chunk, restart the loop for the next chunk, or leave the
 * loop and end the track. */
typedef enum { PB_PROCEED, PB_NEXT_CHUNK, PB_END_TRACK } pb_step_t;

/* While paused, fades out, closes the output device and waits for a command.
 * Enters and returns locked, but drops audio_mutex around the fade and the
 * device close, and the condition wait and run_loop_clip_locked release it while
 * waiting. */
static void chunk_pause_fade_and_wait_locked(playback_session_t * s) {
#ifndef HOST_BUILD
    /* Close the audio output device while paused so the analog amplifier
     * is de-energized, preventing audible pops on jack insertion/removal.
     * Output is automatically reopened via ensure_device() upon resume. */
    if (paused && !stop_requested && !restart_requested) {
        float vol = volume_gain;
        pthread_mutex_unlock(&audio_mutex);
        if (!s->startup_pause_no_fade && s->tempo_rate == 1.0 && !s->mp3_seek_output_held && s->cur_open && s->cur_dec.sample_rate > 0) {
            uint64_t rf = calculate_ramp_frames(s->cur_dec.sample_rate);
            s->cur_frames_played_local += write_transition_fade(&s->cur_dec, rf, s->buf_cur,
                                                              buf_cur_s32, s->cur_replaygain_linear,
                                                              vol, s->cur_path_local);
        }
        close_device();
        s->need_fade_in = true;
        s->startup_pause_no_fade = false;
        pthread_mutex_lock(&audio_mutex);
    }
#endif
    while (paused && !stop_requested && !restart_requested && !seek_pending &&
           !mp3_index_result && s->tempo_setting_seen == speed_setting_serial) {
        if (loop_clip) run_loop_clip_locked(true, s->tempo_setting_seen);
        else pthread_cond_wait(&audio_cond, &audio_mutex);
    }
}

/* Takes a finished MP3 seek-index job: adopts its table, or books the retry
 * or give-up, and arms the deferred seek where one was waiting on it. Enters
 * and returns locked; the job stays in c->completed_index for the caller to
 * free after unlocking. */
static void chunk_adopt_mp3_index_locked(playback_session_t * s, chunk_cmd_t * c) {
    c->completed_index = mp3_index_result;
    mp3_index_result = NULL;
    if (c->completed_index && c->completed_index->generation == s->cur_generation &&
        s->cur_dec.type == DECODER_MP3) {
        bool adopted = c->completed_index->outcome == MP3_INDEX_READY &&
            drmp3_bind_seek_table(s->cur_dec.as.mp3, c->completed_index->count,
                                  c->completed_index->points);
        if (adopted) {
            s->cur_dec.mp3_seek_points = c->completed_index->points;
            s->cur_dec.mp3_seek_point_count = c->completed_index->count;
            c->completed_index->points = NULL;
            s->cur_dec.mp3_seek_index_attempted = true;
            if (mp3_seek_deferred &&
                mp3_seek_deferred_generation == s->cur_generation) {
                c->do_seek = true;
                c->seek_frame = mp3_seek_deferred_frame;
            }
        } else {
            /* Retry/exhaustion bookkeeping applies to every worker
             * failure, not only ones with a seek waiting on them --
             * an eager (non-deferred) scan that fails the same way
             * must still count toward MP3_SEEK_RETRY_MAX, or nothing
             * ever sets mp3_seek_index_attempted and the eager-retry
             * branch below re-spawns a fresh worker every chunk. */
            bool exhausted = !(c->completed_index->outcome == MP3_INDEX_TRANSIENT_FAILURE &&
                               ++mp3_seek_retry_count < MP3_SEEK_RETRY_MAX);
            if (!exhausted) {
                mp3_seek_retry_after_ms = monotonic_ms() + MP3_SEEK_RETRY_DELAY_MS;
            } else {
                s->cur_dec.mp3_seek_index_attempted = true;
                /* Giving up on indexing must not also give up on
                 * the seek itself -- decoder_seek() now permits a
                 * brute-force fallback once attempted is true
                 * (see its own comment), so apply the deferred
                 * target through the normal do_seek path below
                 * instead of silently abandoning it. */
                if (mp3_seek_deferred && mp3_seek_deferred_generation == s->cur_generation) {
                    c->do_seek = true;
                    c->seek_frame = mp3_seek_deferred_frame;
                }
                finish_mp3_deferred_seek_locked();
            }
        }
    }
}

/* Decides this chunk's seek (speed change, pending or deferred seek) and
 * requests an MP3 seek index where one is still needed. Enters and returns
 * locked. */
static void chunk_resolve_seek_locked(playback_session_t * s, chunk_cmd_t * c) {
    /* A speed change uses the normal seek machinery. Explicit or
     * deferred user seeks retain priority over this rewind-to-audible
     * position, including MP3 seeks waiting for the index. */
    if (c->wanted_speed != s->tempo_rate && !seek_pending && !mp3_seek_deferred && !c->do_seek) {
        c->do_seek = true;
        c->seek_frame = s->cur_frames_played_local;
    }
    if (seek_pending && seek_pending_playback_generation == s->cur_generation) {
        c->do_seek = true;
        c->seek_frame = seek_pending_is_percent
            ? (uint64_t) ((double) s->cur_dec.total_frames * (seek_pending_percent / 100.0))
            : seek_pending_frame;
    }
    if (seek_pending) seek_pending = false;

    if (c->do_seek && c->seek_frame > 0 && mp3_needs_seek_index(&s->cur_dec) &&
        !s->cur_dec.mp3_seek_points && !s->cur_dec.mp3_seek_index_attempted) {
        /* Index attempt still outstanding -- hold this seek until it
         * resolves instead of racing it with a brute-force seek.
         * Once mp3_seek_index_attempted is true (below, or a prior
         * permanent failure), decoder_seek() itself now falls back to
         * brute force rather than this ever silently dropping a tap
         * (see decoder_seek()'s own comment). */
        defer_mp3_seek_locked(&s->cur_dec, s->cur_generation, c->seek_frame);
        request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
        /* A hard setup failure (allocation or thread-create) inside
         * the call above can immediately exhaust the retry budget
         * and flip mp3_seek_index_attempted true right here -- when
         * that happens, apply this seek via brute force now instead
         * of dropping it (do_seek/seek_frame already hold the right
         * target); otherwise hold as usual while the worker runs. */
        c->do_seek = s->cur_dec.mp3_seek_index_attempted;
    } else if (!c->do_seek && mp3_seek_deferred &&
               mp3_seek_deferred_generation == s->cur_generation &&
               !mp3_index_worker_active && !s->cur_dec.mp3_seek_index_attempted &&
               mp3_needs_seek_index(&s->cur_dec)) {
        uint64_t retry_deferred_frame = mp3_seek_deferred_frame;
        request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
        if (s->cur_dec.mp3_seek_index_attempted) {
            /* Same give-up-must-not-drop-the-seek fix as above, for
             * the ongoing-retry path rather than the just-deferred
             * one. */
            c->do_seek = true;
            c->seek_frame = retry_deferred_frame;
        }
    } else if (!c->do_seek && !mp3_seek_deferred && !s->cur_dec.mp3_seek_index_attempted &&
               !s->cur_dec.mp3_seek_points && mp3_needs_seek_index(&s->cur_dec)) {
        /* Eager (non-blocking) retry for the proactive request made
         * at track-open/promotion time: that single attempt no-ops
         * rather than queuing a second job if the previous track's
         * worker or unconsumed result was still occupying the one
         * scan slot (request_mp3_seek_index_locked()'s own busy
         * guard). Retrying here every chunk until it actually
         * queues a job, or attempted/points become set, costs only
         * a few field checks under a mutex already held. */
        request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
    }
}

/* Snapshots the rest of the chunk's inputs and the queued next track. Enters
 * and returns locked. */
static void chunk_snapshot_next_locked(playback_session_t * s, chunk_cmd_t * c) {
    c->hold_for_mp3_seek = mp3_seek_deferred &&
                             mp3_seek_deferred_generation == s->cur_generation &&
                             !c->do_seek;
    c->xfade_on = crossfade_enabled && s->tempo_rate == 1.0 && c->wanted_speed == 1.0 &&
                    speed_for_path_locked(next_path) == 1.0;
    c->vol = volume_gain;
    c->remain_paused = paused;
    c->chunk_frames = low_power_mode ? LOW_POWER_CHUNK_FRAMES : NORMAL_CHUNK_FRAMES;

    /* Phase 3: Stack-buffered owned snapshot of next track metadata.
     * Completely eliminates steady-state heap allocations during playback. */
    /* Only the scalars are initialised: zero-filling the path buffer
     * would cost a PATH_MAX memset per chunk, and path is read only
     * when valid is set. */
    c->staged_next.valid = false;
    c->staged_next.replaygain_linear = 0.0f;
    c->staged_next.replaygain_applied = false;
    c->staged_next.generation = 0;
    if (next_path != NULL) {
        size_t len = strlen(next_path);
        if (len < sizeof(c->staged_next.path)) {
            memcpy(c->staged_next.path, next_path, len + 1);
            c->staged_next.valid = true;
            c->staged_next.replaygain_linear = next_replaygain_linear;
            c->staged_next.replaygain_applied = next_replaygain_applied;
            c->staged_next.generation = next_track_generation;
        }
    }
}

/* Reads this chunk's commands in one locked span, then frees the finished
 * index job after the unlock. Enters and returns unlocked. */
static void chunk_read_commands(playback_session_t * s, chunk_cmd_t * c) {
    pthread_mutex_lock(&audio_mutex);
    chunk_pause_fade_and_wait_locked(s);
    c->do_stop = stop_requested;
    c->do_restart = restart_requested;
    c->do_seek = false;
    c->seek_frame = 0;
    c->wanted_speed = speed_for_decoder_locked(s->cur_path_local, &s->cur_dec);
    s->tempo_setting_seen = speed_setting_serial;
    chunk_adopt_mp3_index_locked(s, c);
    chunk_resolve_seek_locked(s, c);
    chunk_snapshot_next_locked(s, c);
    pthread_mutex_unlock(&audio_mutex);
    free_mp3_index_job(c->completed_index);
}

#ifndef HOST_BUILD
/* Checks the device format and output target every chunk so mid-track route
 * changes (such as Bluetooth connection/disconnection) take effect
 * immediately, and falls DoP back to PCM when the route refuses it. Enters
 * and returns unlocked. PB_END_TRACK when the fallback fails. */
static pb_step_t chunk_check_output_route(playback_session_t * s, chunk_cmd_t * c) {
    if (!c->hold_for_mp3_seek) {
        bool is_blending = (c->xfade_on && chunk_staged_path(c) != NULL && s->nxt_open && s->nxt_format_matches);
        /* Request S24_LE if both sides of a crossfade are S24_LE-eligible,
         * avoiding unnecessary close+reopen cycles. */
        bool blend_can_be_wide = is_blending && can_use_wide_crossfade(&s->cur_dec, &s->nxt_dec);
        bool use_wide = s->tempo_rate == 1.0 && (blend_can_be_wide || (!is_blending && can_use_wide_path(&s->cur_dec)));
        if (decoder_is_dop(&s->cur_dec) && !ensure_device_dop(s->cur_dec.channels, s->cur_dec.sample_rate)) {
            /* The route left the local DAC, or it refused DoP: carry on
             * converted, from the last frame actually delivered. */
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            unsigned int dop_rate = s->cur_dec.sample_rate;
            if (!dop_fall_back_to_pcm(&s->cur_dec, &s->cur_frames_played_local, s->cur_path_local,
                                      s->cur_replaygain_linear, s->cur_replaygain_applied)) {
                abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
            if (c->do_seek && dop_rate) c->seek_frame = c->seek_frame * s->cur_dec.sample_rate / dop_rate;
            use_wide = can_use_wide_path(&s->cur_dec);
            s->need_fade_in = true;
        }
        if (!decoder_is_dop(&s->cur_dec)) ensure_device_format(s->cur_dec.channels, s->cur_dec.sample_rate, use_wide);
        /* use_wide is only the REQUEST; audio_output_is_s24_active() (checked
         * right after ensure_device_format() actually ran) is the ground truth
         * -- local hw_params negotiation can fall back to S16_LE.
         * The decode/process/write branch decision below re-derives this same combined check independently
         * rather than trusting this one, since output can be reopened between
         * here and there; this copy only drives the display
         * value. */
        pthread_mutex_lock(&audio_mutex);
        if (current_format_info.valid) {
            current_format_info.output_bit_depth = (use_wide && audio_output_is_s24_active()) ? 24 : 16;
            current_format_info.dsd_native = decoder_is_dop(&s->cur_dec) && audio_output_is_dop_active();
        }
        pthread_mutex_unlock(&audio_mutex);
    }
    return PB_PROCEED;
}
#endif

/* Parks the output while an MP3 seek waits for its index. Enters and returns
 * unlocked. */
static void chunk_hold_for_mp3_index(playback_session_t * s) {
    if (!s->mp3_seek_output_held) {
        close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
        s->nxt_format_matches = false;
#ifndef HOST_BUILD
        close_device();
#endif
        s->mp3_seek_output_held = true;
        s->need_fade_in = true;
    }
    /* Worker completion and transport commands signal audio_cond.
     * The timeout only services a delayed transient retry. */
    struct timespec wake;
    clock_gettime(CLOCK_REALTIME, &wake);
    wake.tv_nsec += 100000000L;
    if (wake.tv_nsec >= 1000000000L) {
        wake.tv_sec++;
        wake.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&audio_mutex);
    if (mp3_seek_deferred &&
        mp3_seek_deferred_generation == s->cur_generation &&
        !stop_requested && !restart_requested && !seek_pending &&
        !mp3_index_result)
        pthread_cond_timedwait(&audio_cond, &audio_mutex, &wake);
    pthread_mutex_unlock(&audio_mutex);
}

/* Applies the chunk's seek, or recovers the decoder when it fails. Enters and
 * returns unlocked, with a separate locked span per outcome. PB_END_TRACK when
 * the recovery fails; otherwise PB_NEXT_CHUNK. */
static pb_step_t chunk_apply_seek(playback_session_t * s, chunk_cmd_t * c) {
    /* Check actual decoder seekability at the owner as well as
     * the public API: commands can arrive before the new source
     * has published its format snapshot. */
    if (!s->cur_dec.seekable) {
        DBG_LOG("audio: ignoring seek on nonseekable source (%s)\n",
                safe_path_tail(s->cur_path_local));
        return PB_NEXT_CHUNK;
    }

    /* Smooth seek on the playback-owned decoder. Indexed MP3s
     * reach this point only after their bounded table is ready. */
    if (!c->remain_paused && s->tempo_rate == 1.0 && !s->mp3_seek_output_held && s->cur_open && s->cur_dec.sample_rate > 0) {
        fade_out_for_transition(&s->cur_dec, s->buf_cur, s->cur_replaygain_linear, c->vol, s->cur_path_local);
    }

    close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
    s->nxt_format_matches = false;
    if (c->seek_frame > s->cur_dec.total_frames) c->seek_frame = s->cur_dec.total_frames;
    if (decoder_seek(&s->cur_dec, c->seek_frame)) {
        s->cur_frames_played_local = c->seek_frame;
        ab_pcm_alternate = false;
        peq_note_pcm_discontinuity();
        tempo_drop(&s->tempo, &s->tempo_pending_frames);
        s->tempo_finished = false;
        s->tempo_rate = c->wanted_speed;
        s->tempo_eof_retries = 0;
        s->tempo_recovery_pending = false;
        pthread_mutex_lock(&audio_mutex);
        frames_played = s->cur_frames_played_local;
        applied_playback_speed = s->tempo_rate;
        publish_current_format_locked(&s->cur_dec, s->cur_path_local,
            s->cur_replaygain_linear, s->cur_replaygain_applied);
        if (s->tempo_rate != 1.0) current_format_info.output_bit_depth = 16;
        if (mp3_seek_deferred_generation == s->cur_generation)
            finish_mp3_deferred_seek_locked();
        pthread_mutex_unlock(&audio_mutex);
        s->mp3_seek_output_held = c->remain_paused;
        s->need_fade_in = true;
        s->consecutive_decoder_errors = 0;
    } else {
        DBG_LOG("audio: seek to frame %" PRIu64 " failed (%s)\n",
                c->seek_frame, safe_path_tail(s->cur_path_local));
        pthread_mutex_lock(&audio_mutex);
        if (mp3_seek_deferred_generation == s->cur_generation)
            finish_mp3_deferred_seek_locked();
        pthread_mutex_unlock(&audio_mutex);
        /* Some codecs reset internal state before they can know
         * the seek will fail (AAC closes/recreates its handle,
         * Opus moves the demux cursor before discarding). Restore
         * the last confirmed position sequentially so playback
         * never continues through a half-mutated decoder. */
        if (!reopen_decoder_at(&s->cur_dec, s->cur_path_local,
                               s->cur_frames_played_local)) {
            s->cur_open = false;
            abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
            return PB_END_TRACK;
        }
        s->cur_open = true;
        tempo_drop(&s->tempo, &s->tempo_pending_frames);
        s->tempo_finished = false;
        /* The decoder was rebuilt, so the next PCM is not the
         * sample that followed the pre-seek low-pass. */
        ab_pcm_alternate = false;
        peq_note_pcm_discontinuity();
        /* The reopen restored the last delivered source position,
         * so it can commit this tempo without retrying a failed
         * seek or overwriting a newer scoped preference. */
        s->tempo_rate = c->wanted_speed;
        s->tempo_eof_retries = 0;
        s->tempo_recovery_pending = false;
        pthread_mutex_lock(&audio_mutex);
        applied_playback_speed = s->tempo_rate;
        publish_current_format_locked(&s->cur_dec, s->cur_path_local,
            s->cur_replaygain_linear, s->cur_replaygain_applied);
        if (s->tempo_rate != 1.0) current_format_info.output_bit_depth = 16;
        pthread_mutex_unlock(&audio_mutex);
        s->mp3_seek_output_held = c->remain_paused;
        s->need_fade_in = true;
        s->consecutive_decoder_errors = 0;
    }
    return PB_NEXT_CHUNK; /* re-check pause/restart state before decoding */
}

/* Gives up on a tempo-stretched chunk: reports the recorded error and ends
 * the track. Enters and returns unlocked. */
static pb_step_t tempo_fail(playback_session_t * s) {
    abort_playback(s->tempo_error, s->cur_generation);
    return PB_END_TRACK;
}

/* Produces and writes one chunk through the tempo stretcher when a playback
 * speed other than 1x is active. Enters and returns unlocked. PB_PROCEED when
 * the track is not stretched or the stretcher is drained (the EOF is then
 * handled by the normal path), PB_NEXT_CHUNK after a chunk or a retry, and
 * PB_END_TRACK on failure. */
static pb_step_t chunk_tempo(playback_session_t * s, chunk_cmd_t * c) {
    if (s->tempo_rate == 1.0 || s->tempo_finished) return PB_PROCEED;
    if (!s->tempo) {
        s->tempo = audio_tempo_create(s->cur_dec.sample_rate, s->cur_dec.channels,
                                   s->tempo_rate, s->cur_frames_played_local);
        if (!s->tempo) return tempo_fail(s);
        pthread_mutex_lock(&audio_mutex);
        applied_playback_speed = (float) s->tempo_rate;
        current_format_info.output_bit_depth = 16;
        pthread_mutex_unlock(&audio_mutex);
    }
    bool prepared = s->tempo_pending_frames != 0;
    size_t output_frames = prepared ? s->tempo_pending_frames :
        audio_tempo_read(s->tempo, s->buf_cur, (size_t) c->chunk_frames);
    if (!output_frames && !audio_tempo_drained(s->tempo)) {
        decoder_read_result_t input = decoder_read_s16(&s->cur_dec, c->chunk_frames, s->buf_next);
        if (input.status == DECODER_READ_FATAL_ERROR) return tempo_fail(s);
        if (input.frames) {
            if (!audio_tempo_feed(s->tempo, s->buf_next, (size_t) input.frames)) return tempo_fail(s);
            s->consecutive_decoder_errors = 0;
        } else if (input.status != DECODER_READ_EOF) {
            if (++s->consecutive_decoder_errors >= 12) return tempo_fail(s);
            usleep(1000);
            return PB_NEXT_CHUNK;
        }
        if (input.status == DECODER_READ_EOF && !audio_tempo_flush(s->tempo)) return tempo_fail(s);
        output_frames = audio_tempo_read(s->tempo, s->buf_cur, (size_t) c->chunk_frames);
    }
    if (!output_frames && audio_tempo_drained(s->tempo)) {
        uint64_t fed = audio_tempo_source_fed(s->tempo);
        if (is_premature_eof(fed, s->cur_dec.total_frames, false)) {
            /* Recover from the delivered source position. Never
             * emit recovered decoder frames through the 1x path. */
            if (++s->tempo_eof_retries >= 3) return tempo_fail(s);
            tempo_drop(&s->tempo, &s->tempo_pending_frames);
            if (!reopen_decoder_at(&s->cur_dec, s->cur_path_local, s->cur_frames_played_local)) {
                s->cur_open = false;
                return tempo_fail(s);
            }
            s->tempo_recovery_pending = true;
            return PB_NEXT_CHUNK;
        }
        s->cur_frames_played_local = fed;
        tempo_drop(&s->tempo, &s->tempo_pending_frames);
        s->tempo_finished = true;
        publish_frames_played(s->cur_frames_played_local);
        /* Inject EOF below; decoder EOF must not be read again or
         * trigger a second un-stretched recovery path. */
    } else if (output_frames) {
        if (!prepared) {
            apply_gain(s->buf_cur, output_frames * s->cur_dec.channels, s->cur_replaygain_linear);
            peq_process(s->buf_cur, output_frames, (int) s->cur_dec.channels, s->cur_dec.sample_rate);
            apply_gain(s->buf_cur, output_frames * s->cur_dec.channels, c->vol);
            if (s->need_fade_in) {
                fade_in_s16(s->buf_cur, output_frames, s->cur_dec.channels, s->cur_dec.sample_rate);
                s->need_fade_in = false;
            }
        }
        uint64_t delivered = output_frames;
#ifndef HOST_BUILD
        write_result_t result = write_device_with_retry(s->buf_cur, output_frames,
            s->cur_dec.channels, s->cur_dec.sample_rate, s->cur_path_local, &delivered);
#else
        write_device(s->buf_cur, output_frames, s->cur_dec.channels);
#endif
        s->cur_frames_played_local = audio_tempo_advance(s->tempo, (size_t) delivered,
                                                     delivered == output_frames);
        publish_frames_played(s->cur_frames_played_local);
#ifndef HOST_BUILD
        if (result == WRITE_RESULT_FAILED) {
            s->tempo_error = AUDIO_ERROR_OUTPUT_FAILED;
            return tempo_fail(s);
        }
#endif
        /* Bound retries for one stalled EOF, but let a long track
         * recover from later independent read hiccups. Only real
         * output after a recovery advances the retry budget. */
        if (s->tempo_recovery_pending && delivered > 0) {
            s->tempo_eof_retries = 0;
            s->tempo_recovery_pending = false;
        }
        s->tempo_pending_frames = output_frames - (size_t) delivered;
        if (s->tempo_pending_frames)
            memmove(s->buf_cur, s->buf_cur + delivered * s->cur_dec.channels,
                s->tempo_pending_frames * s->cur_dec.channels * sizeof(*s->buf_cur));
        return PB_NEXT_CHUNK;
    } else return PB_NEXT_CHUNK;
    return PB_PROCEED;
}

/* Bounded reopen+seek recovery after a premature EOF on the current decoder.
 * log_prefix is "" for plain playback or "crossfade " inside the blend window;
 * wide reads S32 into buf, otherwise S16. Returns true when a read produced
 * frames (*r holds it); the caller owns the exhaustion handling. */
static bool reread_after_premature_eof(playback_session_t * s, const char * log_prefix,
                                       bool wide, uint64_t request_frames, void * buf,
                                       decoder_read_result_t * r) {
    for (int dr = 0; dr < 3; dr++) {
        pthread_mutex_lock(&audio_mutex);
        bool abort = stop_requested || restart_requested;
        pthread_mutex_unlock(&audio_mutex);
        if (abort) break;

        DBG_LOG("audio: %spremature EOF at frame %" PRIu64 "/%" PRIu64
                ", reopen attempt %d (%s)\n",
                log_prefix, s->cur_frames_played_local, s->cur_dec.total_frames, dr + 1,
                safe_path_tail(s->cur_path_local));
        usleep(50000); /* 50 ms backoff */

        if (!reopen_decoder_at(&s->cur_dec, s->cur_path_local, s->cur_frames_played_local)) {
            DBG_LOG("audio: %sdecoder reopen failed on attempt %d\n", log_prefix, dr + 1);
            s->cur_open = false; /* dec is now closed */
            continue;
        }
        s->cur_open = true;
#ifndef HOST_BUILD
        *r = wide ? decoder_read_s32(&s->cur_dec, request_frames, (int32_t *) buf)
                  : decoder_read_s16(&s->cur_dec, request_frames, (int16_t *) buf);
#else
        (void) wide;
        *r = decoder_read_s16(&s->cur_dec, request_frames, (int16_t *) buf);
#endif
        if (r->frames > 0) return true;
    }
    return false;
}

/* Failure check for the next decoder's read inside the blend window. */
static bool crossfade_next_read_failed(playback_session_t * s, const chunk_cmd_t * c,
                                       const decoder_read_result_t * r_next) {
    bool nxt_failed = false;

    if (r_next->status == DECODER_READ_FATAL_ERROR || (r_next->status == DECODER_READ_EOF && r_next->frames == 0)) {
        nxt_failed = true;
    } else if (r_next->status == DECODER_READ_RECOVERABLE_ERROR) {
        s->consecutive_nxt_decoder_errors++;
        DBG_LOG("audio: next track recoverable decode error (%u/10) (%s)\n",
                s->consecutive_nxt_decoder_errors, safe_path_tail(chunk_staged_path(c)));
        if (s->consecutive_nxt_decoder_errors >= 10) {
            DBG_LOG("audio: next track consecutive recoverable errors "
                    "exceeded limit, cancelling blend (%s)\n",
                    safe_path_tail(chunk_staged_path(c)));
            nxt_failed = true;
        }
    } else if (r_next->status == DECODER_READ_OK) {
        s->consecutive_nxt_decoder_errors = 0;
    }
    return nxt_failed;
}

/* Status handling for the current decoder's read inside the blend window.
 * PB_END_TRACK: playback was aborted (leave the inner loop); PB_NEXT_CHUNK:
 * retry on the next iteration; PB_PROCEED: *r_cur holds frames to blend. */
static pb_step_t crossfade_cur_status(playback_session_t * s, bool wide, uint64_t want,
                                      uint64_t frames_remaining, void * buf,
                                      decoder_read_result_t * r_cur) {
    if (r_cur->status == DECODER_READ_FATAL_ERROR) {
        DBG_LOG("audio: crossfade fatal decode error (%s)\n", safe_path_tail(s->cur_path_local));
        close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
        s->nxt_format_matches = false;
        abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
        return PB_END_TRACK;
    }

    /* Handle recoverable decode errors using consecutive error counts,
     * matching the plain-playback retry path rather than prematurely
     * ending the crossfade window. */
    if (r_cur->status == DECODER_READ_RECOVERABLE_ERROR) {
        s->consecutive_decoder_errors++;
        DBG_LOG("audio: crossfade recoverable decode error #%u (%s)\n",
                s->consecutive_decoder_errors, safe_path_tail(s->cur_path_local));
        if (s->consecutive_decoder_errors >= 10) {
            DBG_LOG("audio: crossfade consecutive recoverable errors "
                    "exceeded limit (%s)\n",
                    safe_path_tail(s->cur_path_local));
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
            return PB_END_TRACK;
        }
        return PB_NEXT_CHUNK;
    } else if (r_cur->status == DECODER_READ_OK) {
        s->consecutive_decoder_errors = 0;
    }

    if (r_cur->frames == 0 && frames_remaining > 0 && r_cur->status == DECODER_READ_EOF) {
        bool is_nonfinite_stream = s->cur_dec.net_stream != NULL && !s->cur_dec.seekable;
        if (is_premature_eof(s->cur_frames_played_local, s->cur_dec.total_frames,
                             is_nonfinite_stream)) {
            if (!reread_after_premature_eof(s, "crossfade ", wide, want, buf, r_cur)) {
                DBG_LOG("audio: crossfade decoder recovery exhausted (%s)\n",
                        safe_path_tail(s->cur_path_local));
                close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
                s->nxt_format_matches = false;
                abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
        }
    }
    return PB_PROCEED;
}

/* A zero-frame EOF on the current decoder (premature-EOF recovery already
 * done): hand off to the queued next track, or end the track. PB_NEXT_CHUNK
 * continues the inner loop (handoff done or stale prefetch discarded),
 * PB_END_TRACK leaves it (output failure or nothing queued). Enters and
 * returns unlocked. The decoder can become DoP during the handoff
 * (maybe_enable_dop), so it is not assumed non-DoP on return. */
static pb_step_t track_end_or_handoff(playback_session_t * s, const chunk_cmd_t * c) {
    /* True EOF (or live stream end). If a next track is queued, hand
     * off to it -- seamlessly if the format matches (device stays open),
     * or with a brief reopen if it doesn't. */
    if (!s->nxt_open && chunk_staged_path(c) != NULL) {
        if (decoder_open_next(&s->nxt_dec, chunk_staged_path(c))) {
            s->nxt_open = true;
            s->nxt_frames_consumed = 0;
            s->nxt_replaygain_linear_local = c->staged_next.replaygain_linear;
            s->nxt_replaygain_applied_local = c->staged_next.replaygain_applied;
        } else {
            DBG_LOG("audio: failed to open next track (%s)\n",
                    safe_path_tail(chunk_staged_path(c)));
        }
    }

    if (s->nxt_open) {
        pthread_mutex_lock(&audio_mutex);
        bool snap_still_valid = staged_next_valid_locked(c->staged_next.generation, chunk_staged_path(c), s->cur_generation);
        if (!snap_still_valid) {
            /* The current queue or playback request changed while
             * opening the prefetched decoder. Discard it and let
             * the loop handle the current request. */
            pthread_mutex_unlock(&audio_mutex);
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            DBG_LOG("audio: stale gapless prefetch discarded (%s)\n",
                    safe_path_tail(chunk_staged_path(c)));
            return PB_NEXT_CHUNK;
        }
        pthread_mutex_unlock(&audio_mutex);
#ifndef HOST_BUILD
        /* Gapless handoff to a different decoder -- see the
         * initial-open call site's identical comment on why
         * this must run before ensure_device_format(). */
        audio_output_reset_s24_probe();
        if (s->nxt_frames_consumed == 0) maybe_enable_dop(&s->nxt_dec);
        /* DoP to DoP at one rate keeps the stream open. */
        if (decoder_is_dop(&s->cur_dec) && decoder_is_dop(&s->nxt_dec))
            dsd_continue_dop_markers(s->nxt_dec.as.dsd, s->cur_dec.as.dsd);
        bool device_ok = ensure_device_for_decoder(&s->nxt_dec, &s->nxt_frames_consumed);
#else
        bool device_ok = ensure_device(s->nxt_dec.channels, s->nxt_dec.sample_rate);
#endif
        pthread_mutex_lock(&audio_mutex);
        snap_still_valid = staged_next_valid_locked(c->staged_next.generation, chunk_staged_path(c), s->cur_generation);
        if (!snap_still_valid) {
            pthread_mutex_unlock(&audio_mutex);
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            DBG_LOG("audio: gapless handoff invalidated during device "
                    "setup (%s)\n",
                    safe_path_tail(chunk_staged_path(c)));
            return PB_NEXT_CHUNK;
        }

        decoder_t previous_dec = s->cur_dec;
        char *previous_path = s->cur_path_local;
        s->cur_dec = s->nxt_dec;
        memset(&s->nxt_dec, 0, sizeof(s->nxt_dec));
        s->cur_open = true;
        s->nxt_open = false;
        s->cur_frames_played_local = s->nxt_frames_consumed;
        s->cur_replaygain_linear = s->nxt_replaygain_linear_local;
        s->cur_replaygain_applied = s->nxt_replaygain_applied_local;
        s->cur_path_local = next_path; next_path = NULL;
        free(active_path);
        active_path = s->cur_path_local ? strdup(s->cur_path_local) : NULL;
        playback_generation++;
        atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
        atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
        finish_mp3_deferred_seek_locked();
        seek_pending = false;
        s->cur_generation = playback_generation;
        remote_track_stream_begin_playback(s->cur_path_local, s->cur_generation);
        current_total_frames = s->cur_dec.total_frames;
        current_sample_rate = s->cur_dec.sample_rate;
        frames_played = s->cur_frames_played_local;
        counter_generation = s->cur_generation;
        if (device_ok) {
            playback_progress_state = current_sample_rate > 0
                ? AUDIO_PLAYBACK_PROGRESS_ACTIVE : AUDIO_PLAYBACK_PROGRESS_NOT_READY;
            publish_current_format_locked(&s->cur_dec, s->cur_path_local,
                                          s->cur_replaygain_linear, s->cur_replaygain_applied);
        } else {
            abort_playback_locked(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
        }
        track_advanced = true;
        if (device_ok && mp3_needs_seek_index(&s->cur_dec))
            request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
        pthread_mutex_unlock(&audio_mutex);

        decoder_close(&previous_dec);
        free(previous_path);

        return device_ok ? PB_NEXT_CHUNK : PB_END_TRACK;
    }

    s->ended_with_no_next = true;
    s->natural_eof = chunk_staged_path(c) == NULL &&
                     (!s->cur_dec.net_stream ||
                      (s->cur_dec.seekable && s->cur_dec.total_frames > 0));
    return PB_END_TRACK;
}

/* Where the current track is inside the crossfade window, and prefetch of
 * the queued next track the first time the window is entered. */
static void blend_prepare(playback_session_t * s, const chunk_cmd_t * c, blend_window_t * b) {
    b->frames_remaining = (s->cur_dec.total_frames > s->cur_frames_played_local)
        ? s->cur_dec.total_frames - s->cur_frames_played_local : 0;
    b->crossfade_frames = (uint64_t) (CROSSFADE_SECONDS * (double) s->cur_dec.sample_rate);
    b->in_blend_window = c->xfade_on && chunk_staged_path(c) != NULL && !decoder_is_dop(&s->cur_dec) &&
                           b->frames_remaining > 0 && b->frames_remaining <= b->crossfade_frames;

    if (b->in_blend_window && !s->nxt_open) {
        if (decoder_open_next(&s->nxt_dec, chunk_staged_path(c))) {
            s->nxt_open = true;
            s->nxt_frames_consumed = 0;
            s->nxt_replaygain_linear_local = c->staged_next.replaygain_linear;
            s->nxt_replaygain_applied_local = c->staged_next.replaygain_applied;
            s->nxt_format_matches = (s->nxt_dec.channels == s->cur_dec.channels && s->nxt_dec.sample_rate == s->cur_dec.sample_rate);
        } else {
            DBG_LOG("audio: failed to prefetch next track (%s)\n",
                    safe_path_tail(chunk_staged_path(c)));
        }
    }
}

#ifndef HOST_BUILD
/* One blend-window chunk in the wide (int32_t) domain. PB_PROCEED: the window
 * is finished, promote the next track. PB_NEXT_CHUNK: continue the inner loop.
 * PB_END_TRACK: playback was aborted. Enters and returns unlocked. */
static pb_step_t crossfade_chunk_wide(playback_session_t * s, const chunk_cmd_t * c, blend_window_t * b) {
    unsigned int channels = s->cur_dec.channels;
    uint64_t want = (b->frames_remaining < c->chunk_frames) ? b->frames_remaining : c->chunk_frames;

    decoder_read_result_t r_cur = decoder_read_s32(&s->cur_dec, want, buf_cur_s32);
    uint64_t n_cur = r_cur.frames;

    {
        pb_step_t cs = crossfade_cur_status(s, true, want, b->frames_remaining,
                                            buf_cur_s32, &r_cur);
        if (cs == PB_END_TRACK) return PB_END_TRACK;
        if (cs == PB_NEXT_CHUNK) return PB_NEXT_CHUNK;
        n_cur = r_cur.frames;
    }

    if (n_cur > 0) {
        decoder_read_result_t r_next = decoder_read_s32(&s->nxt_dec, n_cur, buf_next_s32);
        bool nxt_failed = crossfade_next_read_failed(s, c, &r_next);

        if (nxt_failed) {
            DBG_LOG("audio: next track crossfade decode failed (status=%d), "
                    "cancelling crossfade (%s)\n",
                    (int) r_next.status, safe_path_tail(chunk_staged_path(c)));
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            s->consecutive_nxt_decoder_errors = 0;

            /* IMPORTANT: Do NOT discard buf_cur_s32! Output the decoded frames of
             * cur_dec as unblended audio and advance cur_frames_played_local to
             * prevent skips and maintain accurate timeline sync. buf_cur (s16) is
             * idle in this wide branch -- reused as write_device_with_retry_s32()'s
             * narrow-fallback scratch buffer, see that function's own comment. */
            apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, s->cur_replaygain_linear);
            peq_process_s32(buf_cur_s32, (size_t) n_cur, (int) channels, s->cur_dec.sample_rate);
            apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, c->vol);

            if (s->need_fade_in) {
                fade_in_s32(buf_cur_s32, n_cur, channels, s->cur_dec.sample_rate);
                s->need_fade_in = false;
            }

            uint64_t delivered = 0;
            write_result_t wr = write_device_with_retry_s32(buf_cur_s32, n_cur, channels,
                                                             s->cur_dec.sample_rate, s->cur_path_local,
                                                             s->buf_cur, &delivered);
            s->cur_frames_played_local += delivered;
            b->frames_remaining -= (delivered < b->frames_remaining) ? delivered : b->frames_remaining;
            publish_frames_played(s->cur_frames_played_local);

            if (wr == WRITE_RESULT_ABORTED) {
                return PB_NEXT_CHUNK;
            } else if (wr == WRITE_RESULT_FAILED) {
                DBG_LOG("audio: crossfade fallback output failure (%s)\n", safe_path_tail(s->cur_path_local));
                abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
            return PB_NEXT_CHUNK;
        }

        uint64_t n_next = r_next.frames;
        s->nxt_frames_consumed += n_next;
        if (n_next < n_cur) {
            memset(buf_next_s32 + (size_t) n_next * channels, 0, (size_t) (n_cur - n_next) * channels * sizeof(int32_t));
        }

        apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, s->cur_replaygain_linear);
        apply_gain_s32(buf_next_s32, (size_t) n_cur * channels, s->nxt_replaygain_linear_local);

        uint64_t fade_start_frame = b->crossfade_frames - b->frames_remaining;
        mix_crossfade_s32(buf_cur_s32, buf_next_s32, buf_out_s32, n_cur, channels, fade_start_frame, b->crossfade_frames);

        peq_process_s32(buf_out_s32, (size_t) n_cur, (int) channels, s->cur_dec.sample_rate);
        apply_gain_s32(buf_out_s32, (size_t) n_cur * channels, c->vol);

        if (s->need_fade_in) {
            fade_in_s32(buf_out_s32, n_cur, channels, s->cur_dec.sample_rate);
            s->need_fade_in = false;
        }

        uint64_t delivered = 0;
        write_result_t wr = write_device_with_retry_s32(buf_out_s32, n_cur, channels,
                                                         s->cur_dec.sample_rate, s->cur_path_local,
                                                         s->buf_out, &delivered);
        s->cur_frames_played_local += delivered;
        b->frames_remaining -= (delivered < b->frames_remaining) ? delivered : b->frames_remaining;
        publish_frames_played(s->cur_frames_played_local);

        if (wr == WRITE_RESULT_ABORTED) {
            return PB_NEXT_CHUNK;
        } else if (wr == WRITE_RESULT_FAILED) {
            DBG_LOG("audio: crossfade output failure, abandoning blend (%s)\n",
                    safe_path_tail(s->cur_path_local));
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
            return PB_END_TRACK;
        }

        if (b->frames_remaining > 0) {
            return PB_NEXT_CHUNK; /* more of cur left in the window, keep blending */
        }
    }
    return PB_PROCEED;
}
#endif

/* The same for the narrow (int16_t) domain. */
static pb_step_t crossfade_chunk_s16(playback_session_t * s, const chunk_cmd_t * c, blend_window_t * b) {
    unsigned int channels = s->cur_dec.channels;
    uint64_t want = (b->frames_remaining < c->chunk_frames) ? b->frames_remaining : c->chunk_frames;

    decoder_read_result_t r_cur = decoder_read_s16(&s->cur_dec, want, s->buf_cur);
    uint64_t n_cur = r_cur.frames;

    {
        pb_step_t cs = crossfade_cur_status(s, false, want, b->frames_remaining,
                                            s->buf_cur, &r_cur);
        if (cs == PB_END_TRACK) return PB_END_TRACK;
        if (cs == PB_NEXT_CHUNK) return PB_NEXT_CHUNK;
        n_cur = r_cur.frames;
    }

    if (n_cur > 0) {
        decoder_read_result_t r_next = decoder_read_s16(&s->nxt_dec, n_cur, s->buf_next);
        bool nxt_failed = crossfade_next_read_failed(s, c, &r_next);

        if (nxt_failed) {
            DBG_LOG("audio: next track crossfade decode failed (status=%d), "
                        "cancelling crossfade (%s)\n",
                    (int) r_next.status, safe_path_tail(chunk_staged_path(c)));
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            s->consecutive_nxt_decoder_errors = 0;

            /* IMPORTANT: Do NOT discard buf_cur! Output the decoded frames of cur_dec
             * as unblended audio and advance cur_frames_played_local to prevent skips
             * and maintain accurate timeline sync. */
            apply_gain(s->buf_cur, (size_t) n_cur * channels, s->cur_replaygain_linear);
            peq_process(s->buf_cur, (size_t) n_cur, (int) channels, s->cur_dec.sample_rate);
            apply_gain(s->buf_cur, (size_t) n_cur * channels, c->vol);

            if (s->need_fade_in) {
                fade_in_s16(s->buf_cur, n_cur, channels, s->cur_dec.sample_rate);
                s->need_fade_in = false;
            }

#ifndef HOST_BUILD
            uint64_t delivered = 0;
            write_result_t wr = write_device_with_retry(s->buf_cur, n_cur, channels,
                                                         s->cur_dec.sample_rate, s->cur_path_local,
                                                         &delivered);
            s->cur_frames_played_local += delivered;
            b->frames_remaining -= (delivered < b->frames_remaining) ? delivered : b->frames_remaining;
            publish_frames_played(s->cur_frames_played_local);

            if (wr == WRITE_RESULT_ABORTED) {
                return PB_NEXT_CHUNK;
            } else if (wr == WRITE_RESULT_FAILED) {
                DBG_LOG("audio: crossfade fallback output failure (%s)\n", safe_path_tail(s->cur_path_local));
                abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
#else
            write_device(s->buf_cur, n_cur, channels);
            s->cur_frames_played_local += n_cur;
            b->frames_remaining -= (n_cur < b->frames_remaining) ? n_cur : b->frames_remaining;
            publish_frames_played(s->cur_frames_played_local);
#endif
            return PB_NEXT_CHUNK;
        }

        uint64_t n_next = r_next.frames;
        s->nxt_frames_consumed += n_next;
        if (n_next < n_cur) {
            memset(s->buf_next + (size_t) n_next * channels, 0, (size_t) (n_cur - n_next) * channels * sizeof(int16_t));
        }

        apply_gain(s->buf_cur, (size_t) n_cur * channels, s->cur_replaygain_linear);
        apply_gain(s->buf_next, (size_t) n_cur * channels, s->nxt_replaygain_linear_local);

        uint64_t fade_start_frame = b->crossfade_frames - b->frames_remaining;
        mix_crossfade(s->buf_cur, s->buf_next, s->buf_out, n_cur, channels, fade_start_frame, b->crossfade_frames);

        peq_process(s->buf_out, (size_t) n_cur, (int) channels, s->cur_dec.sample_rate);
        apply_gain(s->buf_out, (size_t) n_cur * channels, c->vol);

        if (s->need_fade_in) {
            fade_in_s16(s->buf_out, n_cur, channels, s->cur_dec.sample_rate);
            s->need_fade_in = false;
        }

#ifndef HOST_BUILD
        uint64_t delivered = 0;
        write_result_t wr = write_device_with_retry(s->buf_out, n_cur, channels,
                                                     s->cur_dec.sample_rate, s->cur_path_local,
                                                     &delivered);
        s->cur_frames_played_local += delivered;
        b->frames_remaining -= (delivered < b->frames_remaining) ? delivered : b->frames_remaining;
        publish_frames_played(s->cur_frames_played_local);

        if (wr == WRITE_RESULT_ABORTED) {
            return PB_NEXT_CHUNK;
        } else if (wr == WRITE_RESULT_FAILED) {
            DBG_LOG("audio: crossfade output failure, abandoning blend (%s)\n",
                    safe_path_tail(s->cur_path_local));
            close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
            s->nxt_format_matches = false;
            abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
            return PB_END_TRACK;
        }
#else
        write_device(s->buf_out, n_cur, channels);
        s->cur_frames_played_local += n_cur;
        b->frames_remaining -= n_cur;
        publish_frames_played(s->cur_frames_played_local);
#endif

        if (b->frames_remaining > 0) {
            return PB_NEXT_CHUNK; /* more of cur left in the window, keep blending */
        }
    }
    return PB_PROCEED;
}

/* Blend window finished (cur fully consumed): promote next to current, or
 * discard a stale prefetch. Enters and returns unlocked. */
static void crossfade_promote_next(playback_session_t * s, const chunk_cmd_t * c) {
    /* Blend window finished (cur fully consumed) -- promote next to current.
     * Verify the snapshot's generation still matches what was armed --
     * if not, another audio_set_next_track() replaced the queued track
     * while we were in the crossfade window; discard the stale prefetch. */
    pthread_mutex_lock(&audio_mutex);
    bool snap_still_valid = staged_next_valid_locked(c->staged_next.generation, chunk_staged_path(c), s->cur_generation);
    if (snap_still_valid) {
        decoder_close(&s->cur_dec);
        s->cur_dec = s->nxt_dec;
#ifndef HOST_BUILD
        /* Reset S24 probe cache on track promotion so the new track
         * can negotiate S24_LE output. */
        audio_output_reset_s24_probe();
#endif
        s->nxt_open = false;
        s->nxt_format_matches = false;
        s->cur_frames_played_local = s->nxt_frames_consumed;
        s->cur_replaygain_linear = s->nxt_replaygain_linear_local;
        s->cur_replaygain_applied = s->nxt_replaygain_applied_local;
        free(s->cur_path_local);
        s->cur_path_local = next_path; next_path = NULL;
        free(active_path);
        active_path = s->cur_path_local ? strdup(s->cur_path_local) : NULL;
        playback_generation++;
        atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
        atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
        /* A deferred seek belongs to the track that just ended --
         * leaving it set would block the new track's own eager
         * index request (its "not deferred" retry branch) and
         * misdirect the stale-generation deferred-retry branch. */
        finish_mp3_deferred_seek_locked();
        seek_pending = false;
        s->cur_generation = playback_generation;
        remote_track_stream_begin_playback(s->cur_path_local, s->cur_generation);
        current_total_frames = s->cur_dec.total_frames;
        current_sample_rate = s->cur_dec.sample_rate;
        frames_played = s->cur_frames_played_local;
        counter_generation = s->cur_generation;
        playback_progress_state = current_sample_rate > 0
            ? AUDIO_PLAYBACK_PROGRESS_ACTIVE : AUDIO_PLAYBACK_PROGRESS_NOT_READY;
        publish_current_format_locked(&s->cur_dec, s->cur_path_local,
                                      s->cur_replaygain_linear, s->cur_replaygain_applied);
        track_advanced = true;
        if (mp3_needs_seek_index(&s->cur_dec))
            request_mp3_seek_index_locked(&s->cur_dec, s->cur_path_local, s->cur_generation);
        pthread_mutex_unlock(&audio_mutex);
    } else {
        /* Stale prefetch: a newer next track was armed -- close
         * nxt_dec and let the current track continue to its own EOF. */
        pthread_mutex_unlock(&audio_mutex);
        close_decoder_if_open(&s->nxt_dec, &s->nxt_open);
        s->nxt_format_matches = false;
        DBG_LOG("audio: stale crossfade prefetch discarded (%s)\n",
                safe_path_tail(chunk_staged_path(c)));
    }
}

#ifndef HOST_BUILD
/* One plain chunk through the wide (int32_t / DoP) path. PB_END_TRACK: playback
 * was aborted or the track ended; PB_NEXT_CHUNK: continue the inner loop;
 * PB_PROCEED: the chunk was written. Enters and returns unlocked. */
static pb_step_t plain_chunk_wide(playback_session_t * s, const chunk_cmd_t * c, bool dop_chunk) {
    decoder_read_result_t r_cur = decoder_read_s32(&s->cur_dec, c->chunk_frames, buf_cur_s32);
    uint64_t n_cur = r_cur.frames;

    if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
        s->consecutive_decoder_errors++;
        DBG_LOG("audio: recoverable decoder error #%u at frame %" PRIu64 " (%s)\n",
                s->consecutive_decoder_errors, s->cur_frames_played_local, safe_path_tail(s->cur_path_local));
        if (s->consecutive_decoder_errors >= 10) {
            DBG_LOG("audio: consecutive recoverable errors exceeded limit (%s)\n",
                    safe_path_tail(s->cur_path_local));
            r_cur.status = DECODER_READ_FATAL_ERROR;
        }
    } else if (r_cur.status == DECODER_READ_OK) {
        s->consecutive_decoder_errors = 0;
    }

    if (r_cur.status == DECODER_READ_FATAL_ERROR) {
        DBG_LOG("audio: fatal decoder error at frame %" PRIu64 "/%" PRIu64 " (%s)\n",
                s->cur_frames_played_local, s->cur_dec.total_frames, safe_path_tail(s->cur_path_local));
        abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
        return PB_END_TRACK;
    }

    if (n_cur == 0 && r_cur.status == DECODER_READ_EOF) {
        /* Zero-frame read: check whether this is a premature EOF for a
         * finite local file, or a genuine (or live-stream) end. */
        bool is_nonfinite_stream =s->cur_dec.net_stream != NULL && !s->cur_dec.seekable;
        if (is_premature_eof(s->cur_frames_played_local, s->cur_dec.total_frames,
                             is_nonfinite_stream)) {
            /* Attempt bounded reopen+seek recovery */
            bool recovered = reread_after_premature_eof(s, "", true, c->chunk_frames, buf_cur_s32, &r_cur);
            n_cur = r_cur.frames;
            if (!recovered) {
                DBG_LOG("audio: decoder recovery exhausted at %" PRIu64 "/%" PRIu64
                        " (%s)\n",
                        s->cur_frames_played_local, s->cur_dec.total_frames,
                        safe_path_tail(s->cur_path_local));
                abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
            /* n_cur > 0 now -- fall through to normal processing */
        }

        if (n_cur == 0) {
            if (track_end_or_handoff(s, c) == PB_END_TRACK) return PB_END_TRACK;
            return PB_NEXT_CHUNK;
        }
    }

    if (dop_chunk) {
        /* DoP skips the PCM stage. The next PCM buffer, including
         * one at the same rate and format, must not reuse the
         * low-pass from the PCM that preceded this chunk. */
        peq_note_pcm_discontinuity();
        s->need_fade_in = false; /* DSD bits: nothing may scale them */
    } else {
    apply_gain_s32(buf_cur_s32, (size_t) n_cur * s->cur_dec.channels, s->cur_replaygain_linear);
    peq_process_s32(buf_cur_s32, (size_t) n_cur, (int) s->cur_dec.channels, s->cur_dec.sample_rate);
    apply_gain_s32(buf_cur_s32, (size_t) n_cur * s->cur_dec.channels, c->vol);
    }

    if (s->need_fade_in && n_cur > 0) {
        fade_in_s32(buf_cur_s32, n_cur, s->cur_dec.channels, s->cur_dec.sample_rate);
        s->need_fade_in = false;
    }

    uint64_t delivered = 0;
    /* buf_cur is idle here -- this chunk decoded into buf_cur_s32
     * instead -- so it doubles as the S16 fallback scratch buffer
     * if the device turns out not to actually be S24_LE by
     * write time (see write_device_with_retry_s32_ex()'s own
     * comment). */
    write_result_t wr = dop_chunk
        ? write_device_with_retry_dop(buf_cur_s32, n_cur, s->cur_dec.channels, s->cur_dec.sample_rate,
                                      s->cur_path_local, &delivered)
        : write_device_with_retry_s32(buf_cur_s32, n_cur, s->cur_dec.channels,
                                      s->cur_dec.sample_rate, s->cur_path_local,
                                      s->buf_cur, &delivered);
    s->cur_frames_played_local += delivered;
    publish_frames_played(s->cur_frames_played_local);

    if (wr == WRITE_RESULT_FAILED && dop_chunk &&
        dop_fall_back_to_pcm(&s->cur_dec, &s->cur_frames_played_local, s->cur_path_local,
                             s->cur_replaygain_linear, s->cur_replaygain_applied)) {
        /* The DoP stream failed: continue converted from the last
         * delivered frame instead of stopping the track. */
        s->need_fade_in = true;
        return PB_NEXT_CHUNK;
    }
    if (wr == WRITE_RESULT_ABORTED) {
        return PB_NEXT_CHUNK;
    } else if (wr == WRITE_RESULT_FAILED) {
        DBG_LOG("audio: output failure, stopping (%s)\n",
                safe_path_tail(s->cur_path_local));
        abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
        return PB_END_TRACK;
    }
    return PB_PROCEED;
}
#endif

/* A/B loop state of one plain s16 chunk. loop_requested outlives the prepare
 * step: the write span disables the loop on a short or failed write. */
typedef struct {
    uint64_t read_frames;
    bool loop_requested;
} ab_loop_chunk_t;

/* Adopts a finished A/B switch job, wraps at the loop end and limits the read
 * to the loop end. Enters and returns unlocked. */
static ab_loop_chunk_t ab_loop_prepare(playback_session_t * s, const chunk_cmd_t * c) {
    uint64_t read_frames = c->chunk_frames;
    uint64_t loop_start_frame = 0;
    uint64_t loop_end_frame = 0;
    ab_switch_adopt(&s->ab_active, &s->cur_dec, s->cur_frames_played_local,
                    s->cur_generation);
    pthread_mutex_lock(&audio_mutex);
    bool loop_requested = ab_loop_enabled && !ab_switch_requested &&
                          ab_loop_generation == s->cur_generation &&
                          !crossfade_enabled && s->tempo_rate == 1.0 &&
                          decoder_supports_sample_accurate_loop(&s->cur_dec);
    double loop_start_seconds = ab_loop_start_seconds;
    double loop_end_seconds = ab_loop_end_seconds;
    pthread_mutex_unlock(&audio_mutex);
    if (loop_requested &&
        !audio_ab_loop_frames(loop_start_seconds, loop_end_seconds,
                              s->cur_dec.sample_rate, s->cur_dec.total_frames,
                              &loop_start_frame, &loop_end_frame))
        loop_requested = false;
    if (loop_requested && s->cur_frames_played_local >= loop_end_frame) {
        if (decoder_seek(&s->cur_dec, loop_start_frame)) {
            peq_note_pcm_discontinuity();
            s->cur_frames_played_local = loop_start_frame;
            pthread_mutex_lock(&audio_mutex);
            if (playback_generation == s->cur_generation)
                frames_played = s->cur_frames_played_local;
            pthread_mutex_unlock(&audio_mutex);
        } else {
            loop_requested = false;
            pthread_mutex_lock(&audio_mutex);
            if (ab_loop_generation == s->cur_generation)
                ab_loop_enabled = false;
            pthread_mutex_unlock(&audio_mutex);
        }
    }
    if (loop_requested)
        read_frames = audio_ab_loop_read_frames(s->cur_frames_played_local,
                                                loop_end_frame, read_frames);
    return (ab_loop_chunk_t) { .read_frames = read_frames, .loop_requested = loop_requested };
}

/* One plain chunk through the s16 path; same result contract as
 * plain_chunk_wide(). */
static pb_step_t plain_chunk_s16(playback_session_t * s, const chunk_cmd_t * c) {
    ab_loop_chunk_t ab = ab_loop_prepare(s, c);
    uint64_t read_frames = ab.read_frames;
    decoder_read_result_t r_cur = s->tempo_finished
        ? (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_EOF }
        : decoder_read_s16(&s->cur_dec, read_frames, s->buf_cur);
    uint64_t n_cur = r_cur.frames;
    if (r_cur.status != DECODER_READ_OK) {
        audio_clear_ab_switch();
        ab_switch_free_job(s->ab_active);
        s->ab_active = NULL;
    }

    if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
        s->consecutive_decoder_errors++;
        DBG_LOG("audio: recoverable decoder error #%u at frame %" PRIu64 " (%s)\n",
                s->consecutive_decoder_errors, s->cur_frames_played_local, safe_path_tail(s->cur_path_local));
        if (s->consecutive_decoder_errors >= 10) {
            DBG_LOG("audio: consecutive recoverable errors exceeded limit (%s)\n",
                    safe_path_tail(s->cur_path_local));
            r_cur.status = DECODER_READ_FATAL_ERROR;
        }
    } else if (r_cur.status == DECODER_READ_OK) {
        s->consecutive_decoder_errors = 0;
    }

    if (r_cur.status == DECODER_READ_FATAL_ERROR) {
        DBG_LOG("audio: fatal decoder error at frame %" PRIu64 "/%" PRIu64 " (%s)\n",
                s->cur_frames_played_local, s->cur_dec.total_frames, safe_path_tail(s->cur_path_local));
        abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
        return PB_END_TRACK;
    }

    if (n_cur == 0 && r_cur.status == DECODER_READ_EOF) {
        /* Zero-frame read: check whether this is a premature EOF for a
         * finite local file, or a genuine (or live-stream) end. */
        bool is_nonfinite_stream =s->cur_dec.net_stream != NULL && !s->cur_dec.seekable;
        if (is_premature_eof(s->cur_frames_played_local, s->cur_dec.total_frames,
                                 is_nonfinite_stream)) {
            /* Attempt bounded reopen+seek recovery */
            bool recovered = reread_after_premature_eof(s, "", false, read_frames, s->buf_cur, &r_cur);
            n_cur = r_cur.frames;
            if (!recovered) {
                DBG_LOG("audio: decoder recovery exhausted at %" PRIu64 "/%" PRIu64
                        " (%s)\n",
                        s->cur_frames_played_local, s->cur_dec.total_frames,
                        safe_path_tail(s->cur_path_local));
                abort_playback(AUDIO_ERROR_DECODER_FAILED, s->cur_generation);
                return PB_END_TRACK;
            }
            /* n_cur > 0 now -- fall through to normal processing */
        }

        if (n_cur == 0) {
            if (track_end_or_handoff(s, c) == PB_END_TRACK) return PB_END_TRACK;
            return PB_NEXT_CHUNK;
        }
        }

    ab_switch_mix_source(&s->ab_active, s->buf_cur, s->buf_out, n_cur,
                         s->cur_dec.channels);

    apply_gain(s->buf_cur, (size_t) n_cur * s->cur_dec.channels, s->cur_replaygain_linear);
    peq_process(s->buf_cur, (size_t) n_cur, (int) s->cur_dec.channels, s->cur_dec.sample_rate);
    apply_gain(s->buf_cur, (size_t) n_cur * s->cur_dec.channels, c->vol);

    if (s->need_fade_in && n_cur > 0) {
        fade_in_s16(s->buf_cur, n_cur, s->cur_dec.channels, s->cur_dec.sample_rate);
        s->need_fade_in = false;
    }

#ifndef HOST_BUILD
    uint64_t delivered = 0;
    write_result_t wr = write_device_with_retry(s->buf_cur, n_cur, s->cur_dec.channels,
                                                 s->cur_dec.sample_rate, s->cur_path_local,
                                                 &delivered);
    s->cur_frames_played_local += delivered;
    pthread_mutex_lock(&audio_mutex);
        if (ab.loop_requested && (delivered != n_cur || wr != WRITE_RESULT_OK))
            ab_loop_enabled = false;
    frames_played = s->cur_frames_played_local;
    pthread_mutex_unlock(&audio_mutex);

        if (wr != WRITE_RESULT_OK || delivered != n_cur) {
            audio_clear_ab_switch();
            ab_switch_free_job(s->ab_active);
            s->ab_active = NULL;
        }

    if (wr == WRITE_RESULT_ABORTED) {
        return PB_NEXT_CHUNK;
    } else if (wr == WRITE_RESULT_FAILED) {
        DBG_LOG("audio: output failure, stopping (%s)\n",
                safe_path_tail(s->cur_path_local));
        abort_playback(AUDIO_ERROR_OUTPUT_FAILED, s->cur_generation);
        return PB_END_TRACK;
    }
#else
    write_device(s->buf_cur, n_cur, s->cur_dec.channels);
    s->cur_frames_played_local += n_cur;
    publish_frames_played(s->cur_frames_played_local);
#endif
    return PB_PROCEED;
}

/* The playback loop of one started track: one iteration per chunk. */
static void session_play_track(playback_session_t * s) {
    for (;;) {
        tempo_sync_generation(s);
        ab_switch_housekeeping(s);
        chunk_cmd_t c;
        chunk_read_commands(s, &c);

#ifndef HOST_BUILD
        if (chunk_check_output_route(s, &c) == PB_END_TRACK) break;
#endif

        if (c.do_restart || c.do_stop) {
            /* Phase 4: Controlled transition ramp-down on manual stop/restart */
            if (s->tempo_rate == 1.0 && !s->mp3_seek_output_held && s->cur_open && s->cur_dec.sample_rate > 0) {
                fade_out_for_transition(&s->cur_dec, s->buf_cur, s->cur_replaygain_linear, c.vol, s->cur_path_local);
            }
            if (c.do_restart) s->should_restart = true;
            if (c.do_stop) s->was_stopped = true;
            break;
        }

        if (c.hold_for_mp3_seek) {
            chunk_hold_for_mp3_index(s);
            continue;
        }

        /* A permanent failure or exhausted transient retry resumes from
         * the confirmed decoder cursor, with the same fade-in used after
         * a successful seek. */
        if (c.remain_paused && !c.do_seek)
            continue;
        if (s->mp3_seek_output_held && !c.do_seek)
            s->mp3_seek_output_held = false;

        if (c.do_seek) {
            if (chunk_apply_seek(s, &c) == PB_END_TRACK) break;
            continue;
        }

        {
            pb_step_t step = chunk_tempo(s, &c);
            if (step == PB_END_TRACK) break;
            if (step == PB_NEXT_CHUNK) continue;
        }

        blend_window_t b;
        blend_prepare(s, &c, &b);

        if (b.in_blend_window && s->nxt_open && s->nxt_format_matches) {
            pb_step_t step;
#ifndef HOST_BUILD
            if (can_use_wide_crossfade(&s->cur_dec, &s->nxt_dec) && audio_output_is_s24_active())
                step = crossfade_chunk_wide(s, &c, &b);
            else
#endif
                step = crossfade_chunk_s16(s, &c, &b);
            if (step == PB_END_TRACK) break;
            if (step == PB_PROCEED) crossfade_promote_next(s, &c);
            continue;
        }

        /* Not blending (crossfade off, not yet near the end, or the next
         * track's format doesn't match) -- plain single-source playback. */
#ifndef HOST_BUILD
        /* can_use_wide_path() alone is source-side eligibility only --
         * whether the device is ACTUALLY open as S24_LE right now
         * can differ when hw_params negotiation failed. Checking audio_output_is_s24_active() as well
         * here, at the single point that decides whether this whole
         * chunk decodes/processes/writes through the s32 path, means a
         * stale or failed wide-path prediction falls through to the
         * ordinary s16 path below instead of reaching
         * audio_output_write_s24(), which would otherwise correctly
         * refuse the write (audio_output_is_s24_active() false) and
         * exhaust this chunk's retry budget into a hard playback failure. */
        bool dop_chunk = decoder_is_dop(&s->cur_dec);
        /* DoP frames never take the s16 path; the next chunk's output
         * check falls back to PCM if the DoP stream is gone. */
        if (dop_chunk && !audio_output_is_dop_active()) continue;
        if (!s->tempo_finished && (dop_chunk || (can_use_wide_path(&s->cur_dec) && audio_output_is_s24_active()))) {
            if (plain_chunk_wide(s, &c, dop_chunk) == PB_END_TRACK) break;
        } else
#endif
        {
            if (plain_chunk_s16(s, &c) == PB_END_TRACK) break;
        }
    }
}

static void * audio_thread_func(void * arg) {
    (void) arg;
    set_worker_thread_name("audio");
    install_thread_crash_altstack(); /* see its own comment (main.c) */
    /* A modest priority boost, not full SCHED_FIFO: real-time scheduling
     * would risk priority inversion against audio_mutex (also taken by the
     * UI thread for format/volume reads), which could make things worse
     * under contention rather than better. nice(-5) just makes this thread
     * win CFS scheduling contests against normal-priority UI/decode work
     * (library scans/MP3 indexing already deliberately lower their own
     * priority the same way, see mp3_index_worker's nice(10) in audio_mp3_index.c) without
     * that inversion risk. Best-effort: fails silently without CAP_SYS_NICE
     * (e.g. an unprivileged host dev build), leaving the thread at its
     * inherited default priority. Matters most at high sample rates, where
     * the ALSA buffer covers less wall-clock time and a UI-caused scheduling
     * delay is more likely to outlast it -- see open_device()'s own comment
     * on why period_size is now rate-scaled for the same reason. */
    (void) nice(-5);

    playback_session_t session = {
        .tempo_rate = 1.0,
        .tempo_error = AUDIO_ERROR_DECODER_FAILED,
        .cur_replaygain_linear = 1.0f,
        .nxt_replaygain_linear_local = 1.0f,
    };
    playback_session_t * s = &session;

    s->buf_cur = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
    s->buf_next = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
    s->buf_out = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
#ifndef HOST_BUILD
    /* File-scope statics so the eligibility gates can verify allocation.
     * A missing primary buffer disables all wide playback; missing optional
     * blend buffers disable only wide crossfade. */
    buf_cur_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
    /* Only used for a wide (both-sides-S24_LE) crossfade blend -- see
     * mix_crossfade_s32()'s own caller below. Idle the rest of the time,
     * same as buf_cur_s32 is idle outside the plain wide-playback path. */
    buf_next_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
    buf_out_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
#endif

#ifndef HOST_BUILD
    /* Prime the ALSA audio device open-and-close once on startup so the codec
     * driver enters a safe low-power bias state, preventing jack insertion/removal pops
     * before the first track plays. */
    if (ensure_device(2, 44100)) close_device();
#endif

    for (;;) {
        tempo_drop(&s->tempo, &s->tempo_pending_frames);
        s->tempo_finished = false;
        startup_request_t req;
        session_wait_for_request(s, &req);
        if (!session_start_track(s, &req)) continue;

        session_play_track(s);
        session_after_track_loop(s);

        if (s->should_restart) continue; /* reopen at the top of the outer loop */

        session_end_track(s);
    }
    return NULL;
}

void audio_init(void) {
#ifdef HOST_BUILD
    SDL_InitSubSystem(SDL_INIT_AUDIO);
#endif
    peq_init();

    if (!thread_started) {
        thread_started = true;
        /* HE-AAC v2 playback is ~85 KiB, and a crossfade keeps two decoders. */
        enum { AUDIO_THREAD_STACK_SIZE = 512 * 1024 };
        pthread_attr_t attr;
        pthread_attr_t * attr_ptr = NULL;
        bool attr_initialized = pthread_attr_init(&attr) == 0;
        if (attr_initialized &&
            pthread_attr_setstacksize(&attr, AUDIO_THREAD_STACK_SIZE) == 0)
            attr_ptr = &attr;
        pthread_create(&audio_thread, attr_ptr, audio_thread_func, NULL);
        if (attr_initialized) pthread_attr_destroy(&attr);
    }
}

static void play_file_request(const char * path, double start_seconds,
                         bool has_replaygain, double replaygain_gain_db,
                         bool has_replaygain_peak, double replaygain_peak, bool read_tags, int gain_mode) {
    pthread_mutex_lock(&audio_mutex);
    ab_switch_clear_locked();
    free(restart_path);
    restart_path = strdup(path);
    restart_read_replaygain = read_tags;
    restart_replaygain_mode = gain_mode;
    startup_pending = true;
    startup_paused = false;
    startup_metadata_ready = false;
    stopped_startup_generation_valid = false;
    restart_start_seconds = start_seconds;
    restart_replaygain_linear = (float) replaygain_to_linear(has_replaygain, replaygain_gain_db, has_replaygain_peak, replaygain_peak);
    restart_replaygain_applied = has_replaygain;
    free(next_path);
    next_path = NULL; /* the caller re-arms this via audio_set_next_track() right after */
    next_track_generation++;
    restart_requested = true;
    stop_requested = false;
    paused = false;
    track_finished = false;
    track_advanced = false;
    last_playback_error = AUDIO_ERROR_NONE;
    last_playback_error_generation = 0;
    ab_loop_enabled = false;
    playback_generation++;
    remote_track_stream_begin_playback(path, playback_generation);
    atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
    atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
    finish_mp3_deferred_seek_locked();
    seek_pending = false;
    free(active_path);
    active_path = strdup(path);
    clear_current_format_locked();
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

void audio_play_file_at(const char * path, double start_seconds,
                         bool has_replaygain, double replaygain_gain_db,
                         bool has_replaygain_peak, double replaygain_peak) {
    play_file_request(path, start_seconds, has_replaygain, replaygain_gain_db,
                      has_replaygain_peak, replaygain_peak, false, 0);
}
void audio_play_file_at_with_replaygain_mode(const char * path, double start_seconds, int mode) {
    play_file_request(path, start_seconds, false, 0.0, false, 0.0, true, mode);
}

audio_track_metadata_wait_result_t audio_wait_track_metadata(uint64_t generation,
                                                              track_metadata_t * out) {
    if (!out) return AUDIO_TRACK_METADATA_REPLACED;
    pthread_mutex_lock(&audio_mutex);
    while (playback_generation == generation && startup_pending && !stop_requested &&
           !startup_metadata_ready)
        pthread_cond_wait(&audio_cond, &audio_mutex);
    audio_track_metadata_wait_result_t result;
    if (playback_generation == generation && !stop_requested &&
        startup_metadata_ready && startup_metadata_generation == generation) {
        *out = startup_metadata;
        result = AUDIO_TRACK_METADATA_READY;
    } else {
        memset(out, 0, sizeof(*out));
        result = ((stop_requested && playback_generation == generation) || (stopped_startup_generation_valid &&
                  stopped_startup_generation == generation))
            ? AUDIO_TRACK_METADATA_STOPPED : AUDIO_TRACK_METADATA_REPLACED;
    }
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

static bool set_next_track(const char * path, bool has_replaygain, double replaygain_gain_db,
                           bool has_replaygain_peak, double replaygain_peak, bool guarded, uint64_t generation) {
    pthread_mutex_lock(&audio_mutex);
    if (guarded && (playback_generation != generation || stop_requested)) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }
    free(next_path);
    next_path = path ? strdup(path) : NULL;
    next_replaygain_linear = (float) replaygain_to_linear(has_replaygain, replaygain_gain_db, has_replaygain_peak, replaygain_peak);
    next_replaygain_applied = has_replaygain;
    /* Increment generation so any in-flight snapshot the playback thread
     * already took (for a crossfade prefetch) is detected as stale and
     * discarded when the blend window arrives. */
    next_track_generation++;
    pthread_mutex_unlock(&audio_mutex);
    return true;
}

void audio_set_next_track(const char * path, bool has_replaygain, double gain,
                           bool has_peak, double peak) {
    set_next_track(path, has_replaygain, gain, has_peak, peak, false, 0);
}
bool audio_set_next_track_for_generation(const char * path, bool has_replaygain, double gain,
                           bool has_peak, double peak, uint64_t generation) {
    return set_next_track(path, has_replaygain, gain, has_peak, peak, true, generation);
}

bool audio_set_playback_speed(const char * directory, double speed) {
    if (!isfinite(speed) || speed < 0.5 || speed > 2.0) return false;
    directory = speed_normalize_path(directory);
    if ((!directory && speed != 1.0) || (directory && (directory[0] != '/' || strlen(directory) >= sizeof(speed_directory)))) return false;
    char normalized[sizeof(speed_directory)];
    snprintf(normalized, sizeof(normalized), "%s", directory ? directory : "");
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/') normalized[--length] = '\0';
    if (directory && (length <= 1 || strstr(normalized, "/../") || strstr(normalized, "/./") ||
        (length >= 3 && strcmp(normalized + length - 3, "/..") == 0))) return false;
    directory = directory ? normalized : NULL;
    pthread_mutex_lock(&audio_mutex);
    /* Unsupported active formats reject a changed tempo, rather than claiming
     * to apply it. The persisted preference can still apply to the next book. */
    if (speed != 1.0 && have_current && current_format_info.valid && directory) {
        size_t n = strlen(directory);
        const char * active = speed_normalize_path(active_path);
        if (active && strncmp(active, directory, n) == 0 && active[n] == '/' &&
            (current_format_info.is_dsd || current_format_info.is_stream || current_format_info.channels > 2 ||
             current_sample_rate < 8000 || current_sample_rate > 96000)) {
            pthread_mutex_unlock(&audio_mutex);
            return false;
        }
    }
    snprintf(speed_directory, sizeof(speed_directory), "%s", directory ? directory : "");
    ab_switch_clear_locked();
    requested_playback_speed = speed;
    speed_setting_serial++;
    if (speed != 1.0)
        ab_loop_enabled = false;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    return true;
}
double audio_get_playback_speed(void) {
    pthread_mutex_lock(&audio_mutex);
    double speed = have_current ? applied_playback_speed : 1.0;
    pthread_mutex_unlock(&audio_mutex);
    return speed;
}

void audio_set_crossfade_enabled(bool enabled) {
    pthread_mutex_lock(&audio_mutex);
    if (enabled)
        ab_switch_clear_locked();
    crossfade_enabled = enabled;
    if (enabled)
        ab_loop_enabled = false;
    pthread_mutex_unlock(&audio_mutex);
}

bool audio_set_ab_loop(double start_seconds, double end_seconds) {
    if (!isfinite(start_seconds) || !isfinite(end_seconds) ||
        start_seconds < 0.0 || end_seconds <= start_seconds)
        return false;
    pthread_mutex_lock(&audio_mutex);
    bool supported =
        have_current && current_format_info.valid &&
        !current_format_info.is_stream && !current_format_info.is_dsd &&
        current_format_info.source_bit_depth > 0 &&
        current_format_info.source_bit_depth <= 16 && current_sample_rate > 0 &&
        current_total_frames > 0 && !ab_switch_requested && playback_plain_1x_locked() &&
        (current_format_info.codec == AUDIO_CODEC_FLAC ||
         current_format_info.codec == AUDIO_CODEC_PCM);
    double duration = current_sample_rate ? (double)current_total_frames /
                                                (double)current_sample_rate
                                          : 0.0;
    uint64_t start_frame, end_frame;
    if (!supported || end_seconds > duration ||
        !audio_ab_loop_frames(start_seconds, end_seconds, current_sample_rate,
                              current_total_frames, &start_frame, &end_frame)) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }
    ab_loop_start_seconds = start_seconds;
    ab_loop_end_seconds = end_seconds;
    ab_loop_generation = playback_generation;
    ab_loop_enabled = true;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    return true;
}

void audio_clear_ab_loop(void) {
    pthread_mutex_lock(&audio_mutex);
    ab_loop_enabled = false;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

bool audio_loop_start(const char * path) {
    if (!path || !path[0] || is_stream_url(path)) return false;
    decoder_t dec;
    if (!decoder_open(&dec, path)) return false;
    loop_clip_t * clip = NULL;
    bool ok = dec.channels >= 1 && dec.channels <= 2 &&
              dec.sample_rate >= 8000 && dec.sample_rate <= 384000 &&
              dec.total_frames > 0 &&
              dec.total_frames <= LOOP_CLIP_MAX_BYTES / (dec.channels * sizeof(int16_t));
    if (ok) {
        clip = calloc(1, sizeof(*clip));
        ok = clip != NULL;
    }
    if (ok) {
        clip->pcm = malloc((size_t) dec.total_frames * dec.channels * sizeof(int16_t));
        clip->channels = dec.channels;
        clip->rate = dec.sample_rate;
        ok = clip->pcm != NULL;
    }
    while (ok && clip->frames < dec.total_frames) {
        decoder_read_result_t r = decoder_read_s16(&dec, dec.total_frames - clip->frames,
                                                   clip->pcm + clip->frames * clip->channels);
        clip->frames += r.frames;
        if (r.status != DECODER_READ_OK || r.frames == 0) break;
    }
    decoder_close(&dec);
    if (!ok || clip->frames == 0) {
        loop_clip_free(clip);
        return false;
    }
    pthread_mutex_lock(&audio_mutex);
    loop_clip_free(loop_clip);
    loop_clip = clip;
    loop_clip_serial++;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    return true;
}

void audio_loop_stop(void) {
    pthread_mutex_lock(&audio_mutex);
    if (loop_clip) loop_clip_clear_locked();
    pthread_mutex_unlock(&audio_mutex);
}

bool audio_loop_is_active(void) {
    pthread_mutex_lock(&audio_mutex);
    bool active = loop_clip != NULL;
    pthread_mutex_unlock(&audio_mutex);
    return active;
}

bool audio_get_ab_loop(double * start_seconds, double * end_seconds) {
    if (!start_seconds || !end_seconds)
        return false;
    pthread_mutex_lock(&audio_mutex);
    bool enabled = ab_loop_enabled && ab_loop_generation == playback_generation;
    if (enabled) {
        *start_seconds = ab_loop_start_seconds;
        *end_seconds = ab_loop_end_seconds;
    } else {
        *start_seconds = 0.0;
        *end_seconds = 0.0;
    }
    pthread_mutex_unlock(&audio_mutex);
    return enabled;
}

void audio_set_low_power_mode(bool enabled) {
    pthread_mutex_lock(&audio_mutex);
    low_power_mode = enabled;
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_bt_output(bool enabled) {
#ifndef HOST_BUILD
    /* No lock: audio_output.c's own bt_requested flag is only ever read by
     * whichever single thread currently owns the output device (this
     * app's own playback thread, or usb_dac_bridge.c's bridge thread --
     * see audio_output.h's own comment on why only one is ever active at
     * a time) -- a stale read here just means the output switch lands on
     * the next audio_output_ensure() call instead of immediately, not a
     * correctness bug. */
    audio_output_set_bt_requested(enabled);
#else
    (void) enabled; /* host build has no Bluetooth output path -- SDL only */
#endif
}

void audio_set_usb_output(bool enabled, const char * alsa_device) {
#ifndef HOST_BUILD
    /* Same no-lock reasoning as audio_set_bt_output() right above. */
    audio_output_set_usb_requested(enabled, alsa_device);
#else
    (void) enabled;
    (void) alsa_device; /* host build has no USB audio-host output path -- SDL only
                      */
#endif
}

void audio_toggle_pause(void) {
    pthread_mutex_lock(&audio_mutex);
    if ((!have_current && !startup_pending) || stop_requested) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (startup_pending) startup_paused = !startup_paused;
    else paused = !paused;
    bool now_paused = startup_pending ? startup_paused : paused;
    if (now_paused) {
        ab_switch_clear_locked();
        ab_loop_enabled = false;
    }
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);

#ifdef HOST_BUILD
    if (sdl_dev != 0) {
        SDL_PauseAudioDevice(sdl_dev, now_paused ? 1 : 0);
    }
#else
    (void) now_paused;
#endif
}

void audio_stop(void) {
    pthread_mutex_lock(&audio_mutex);
    ab_switch_clear_locked();
    ab_loop_enabled = false;
    stop_requested = true;
    if (startup_pending) {
        /* Cancel both a queued restart and a consumed request still reading
         * tags/opening its decoder. Preserve the generation of established
         * playback so its final resume checkpoint remains usable. */
        stopped_startup_generation = playback_generation;
        stopped_startup_generation_valid = true;
        ++playback_generation;
        restart_requested = false;
        free(restart_path);
        restart_path = NULL;
    }
    startup_pending = false;
    atomic_store_explicit(&mp3_index_stop_flag, true, memory_order_relaxed);
    pthread_cond_broadcast(&audio_cond);
    remote_track_stream_cancel_all();
    pthread_mutex_unlock(&audio_mutex);
}

bool audio_is_idle(void) {
    pthread_mutex_lock(&audio_mutex);
    bool idle = !have_current && !startup_pending && !stop_requested && !restart_requested &&
        !atomic_load_explicit(&local_output_active, memory_order_acquire);
    pthread_mutex_unlock(&audio_mutex);
    return idle;
}

bool audio_is_playing(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = !stop_requested && (startup_pending ? !startup_paused : have_current && !paused);
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_is_paused(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = !stop_requested && (startup_pending ? startup_paused : have_current && paused);
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

/* Publish a coalesced command and wake the playback thread. Always called
 * with audio_mutex held and returns with it unlocked. */
static void finish_seek_request_and_unlock(void) {
    ab_switch_clear_locked();
    seek_pending = true;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

static void request_seek_and_unlock(uint64_t frame) {
    if (frame > current_total_frames) frame = current_total_frames;
    seek_pending_frame = frame;
    seek_pending_is_percent = false;
    seek_pending_playback_generation = playback_generation;
    finish_seek_request_and_unlock();
}

void audio_seek(double seconds) {
    if (!isfinite(seconds)) return;
    pthread_mutex_lock(&audio_mutex);
    if (!have_current || current_sample_rate == 0 || current_total_frames == 0 || !active_path ||
        (current_format_info.valid && !current_format_info.seekable)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (seconds < 0.0) seconds = 0.0;
    double duration = (double) current_total_frames / (double) current_sample_rate;
    if (seconds > duration) seconds = duration;
    uint64_t frame = (uint64_t) (seconds * (double) current_sample_rate);
    ab_loop_enabled = false;
    request_seek_and_unlock(frame);
}

/* Seeks to a percentage of the currently playing track. Stores the percentage
 * alongside playback_generation so the audio thread converts it using the
 * correct decoder metadata for that generation. */
void audio_seek_percent(double percent) {
    if (!isfinite(percent)) return;
    pthread_mutex_lock(&audio_mutex);
    if (!active_path || (current_format_info.valid && !current_format_info.seekable)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    seek_pending_percent = percent;
    seek_pending_is_percent = true;
    seek_pending_playback_generation = playback_generation;
    ab_loop_enabled = false;
    finish_seek_request_and_unlock();
}

double audio_get_position_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result = (current_sample_rate != 0)
        ? (double) frames_played / (double) current_sample_rate
        : 0.0;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

double audio_get_resume_position_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result;
    if (mp3_seek_deferred && mp3_seek_deferred_generation == playback_generation &&
        mp3_seek_deferred_sample_rate > 0) {
        result = (double) mp3_seek_deferred_frame /
                 (double) mp3_seek_deferred_sample_rate;
    } else if (counter_generation != playback_generation) {
        /* A newer request owns this generation while its decoder is still
         * opening. Do not leak the prior track's frame counter to resume
         * callers; report that request's sanitized initial checkpoint. */
        result = isfinite(restart_start_seconds) && restart_start_seconds >= 0.0
            ? restart_start_seconds : 0.0;
    } else {
        result = current_sample_rate != 0
            ? (double) frames_played / (double) current_sample_rate : 0.0;
    }
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

double audio_get_duration_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result = (current_sample_rate != 0)
        ? (double) current_total_frames / (double) current_sample_rate
        : 0.0;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_get_playback_progress(const char * expected_path, audio_playback_progress_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!expected_path) return false;

    pthread_mutex_lock(&audio_mutex);
    bool terminal = playback_progress_state == AUDIO_PLAYBACK_PROGRESS_NATURAL_EOF ||
                    playback_progress_state == AUDIO_PLAYBACK_PROGRESS_MANUAL_STOP;
    bool valid = active_path && strcmp(active_path, expected_path) == 0 &&
                 counter_generation == playback_generation && current_sample_rate > 0 &&
                 !(mp3_seek_deferred && mp3_seek_deferred_generation == playback_generation) &&
                 ((playback_progress_state == AUDIO_PLAYBACK_PROGRESS_ACTIVE && have_current) ||
                  (terminal && !have_current));
    if (valid) {
        out->position_seconds = (double) frames_played / (double) current_sample_rate;
        out->duration_seconds = (double) current_total_frames / (double) current_sample_rate;
        out->is_playing = have_current && !stop_requested && !paused;
        out->is_paused = have_current && !stop_requested && paused;
        out->state = playback_progress_state;
        out->generation = counter_generation;
    }
    pthread_mutex_unlock(&audio_mutex);
    return valid;
}

unsigned int audio_get_sample_rate(void) {
    pthread_mutex_lock(&audio_mutex);
    unsigned int result = current_sample_rate;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_get_current_format_info(audio_current_format_info_t * out) {
    if (!out) return false;
    pthread_mutex_lock(&audio_mutex);
    bool valid = have_current && current_format_info.valid;
    if (valid) {
        *out = current_format_info;
        out->software_volume_gain = (double) volume_gain;
        out->playback_speed = applied_playback_speed;
        out->crossfade_enabled = crossfade_enabled;
    }
    else memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&audio_mutex);
    return valid;
}

bool audio_consume_track_finished(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = track_finished;
    track_finished = false;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_consume_track_advanced(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = track_advanced;
    track_advanced = false;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

audio_error_t audio_consume_error_ex(uint64_t * out_generation) {
    pthread_mutex_lock(&audio_mutex);
    audio_error_t err = last_playback_error;
    if (out_generation) *out_generation = last_playback_error_generation;
    last_playback_error = AUDIO_ERROR_NONE;
    last_playback_error_generation = 0;
    pthread_mutex_unlock(&audio_mutex);
    return err;
}

uint64_t audio_get_playback_generation(void) {
    pthread_mutex_lock(&audio_mutex);
    uint64_t gen = playback_generation;
    pthread_mutex_unlock(&audio_mutex);
    return gen;
}
