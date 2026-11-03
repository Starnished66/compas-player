/* Background waveform preview extraction. Uses a private decoder and never
 * touches playback state or audio_mutex. */
#include "audio_internal.h"
#include "audio_helpers.h"
#include "../../ui/text/i18n/i18n.h"
#include "../demux/mp4/mp4_demux.h"
#include "../demux/ape/ape_demux.h"
#include "../decoders/ogg/ogg_probe.h"
#include "../effects/peq.h"
#include "../timing/audio_tempo.h"
#include "../../network/http/stream/http_stream_file.h"
#include "../../library/remote/remote_track.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../library/parsers/cue/cue_parser.h"
#include "../../core/system/logging/debug_log.h"
#include "../../core/persistence/storage_paths.h"

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

#ifdef HOST_BUILD
  #include <SDL2/SDL.h>
#else
  #include "../output/audio_output.h"
#endif

/* A separate, bounded decode pass for waveform previews. This deliberately
 * uses decoder_t directly and never touches the active decoder, output, PEQ,
 * replaygain, or playback mutex. */
typedef struct {
    bool (*cancel)(void *);
    void * user;
    struct timespec started;
} waveform_cancel_context_t;

static int64_t timespec_diff_ns(const struct timespec * from, const struct timespec * to) {
    return (int64_t) (to->tv_sec - from->tv_sec) * INT64_C(1000000000) +
           (int64_t) to->tv_nsec - (int64_t) from->tv_nsec;
}

static bool waveform_cancel_or_expired(void * user) {
    waveform_cancel_context_t * context = user;
    if (context->cancel && context->cancel(context->user)) return true;
    struct timespec now;
    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    return timespec_diff_ns(&context->started, &now) > INT64_C(90000000000);
}

/* Normalize one decoded prefix against its loudest bin. The caller's output
 * is always a full-count snapshot, so bins after `completed` stay empty. */
static void waveform_normalize_prefix(uint8_t * bins, size_t count,
                                      size_t completed, uint64_t * sums,
                                      uint64_t * sample_counts, double * rms) {
    double peak_rms = 0.0;
    for (size_t i = 0; i < completed; ++i) {
        if (!sample_counts[i]) {
            rms[i] = 0.0;
            continue;
        }
        rms[i] = sqrt((double)sums[i] / (double)sample_counts[i]);
        if (rms[i] > peak_rms) peak_rms = rms[i];
    }
    if (peak_rms > 0.0) {
        for (size_t i = 0; i < completed; ++i)
            bins[i] = (uint8_t)lround((rms[i] / peak_rms) * 255.0);
    } else {
        memset(bins, 0, completed);
    }
    if (completed < count) memset(bins + completed, 0, count - completed);
}

/* Build a private dense table on a throwaway decoder. The table is attached
 * only to the waveform decoder and freed by decoder_close(); playback and the
 * production MP3 index worker never see or adopt it. */
static bool waveform_prepare_mp3_seek_table(decoder_t * dec, const char * path,
                                             waveform_cancel_context_t * cancel_context,
                                             bool * cancelled) {
    enum { WAVEFORM_MP3_MAX_POINTS = 8192 };
    *cancelled = false;
    if (!dec || dec->type != DECODER_MP3 || dec->net_stream || dec->sample_rate == 0 ||
        dec->channels == 0 || dec->total_frames == 0) return false;

    uint64_t interval = dec->sample_rate / 4;
    if (interval == 0) interval = 1;
    uint64_t desired = dec->total_frames / interval;
    if (desired < 2) return false;
    if (desired > WAVEFORM_MP3_MAX_POINTS) desired = WAVEFORM_MP3_MAX_POINTS;

    drmp3_seek_point * points = calloc((size_t)desired, sizeof(*points));
    drmp3 * scan = malloc(sizeof(*scan));
    if (!points || !scan) {
        free(points);
        free(scan);
        return false;
    }

    bool opened = drmp3_init_file(scan, path, NULL) != 0;
    mp3_stream_id_t stream = mp3_stream_id_of(dec->as.mp3);
    bool same_file = opened && mp3_stream_matches(scan, &stream);
    drmp3_uint32 placed = 0;
    if (same_file) {
        if (waveform_cancel_or_expired(cancel_context)) {
            *cancelled = true;
        } else {
            placed = mp3_build_seek_points_cancellable(scan, dec->total_frames, (drmp3_uint32)desired,
                                                       points, cancelled,
                                                       waveform_cancel_or_expired, cancel_context);
        }
    }
    if (opened) drmp3_uninit(scan);
    free(scan);

    if (*cancelled) {
        free(points);
        return false;
    }
    if (!same_file || placed == 0 ||
        !drmp3_bind_seek_table(dec->as.mp3, placed, points)) {
        free(points);
        return false;
    }
    dec->mp3_seek_points = points;
    dec->mp3_seek_point_count = placed;
    dec->mp3_seek_index_attempted = true;
    return true;
}

bool audio_extract_waveform_progress(const char * path, uint8_t * bins, size_t count,
                                     bool (*cancel)(void *), void * user,
                                     void (*progress)(const uint8_t *, size_t, size_t, void *),
                                     void * progress_user) {
    enum {
        WAVEFORM_MAX_BINS = 256,
        WAVEFORM_READ_FRAMES = 4096,
        WAVEFORM_WINDOW_MS = 40,
        WAVEFORM_YIELD_INTERVAL_MS = 100,
        WAVEFORM_YIELD_SLEEP_US = 1000,
        WAVEFORM_MAX_CONSECUTIVE_ERRORS = 10
    };
    const uint64_t budget_ns = UINT64_C(90) * 1000000000;
    /* PCM is 64 KiB and the bin accumulators another ~6 KiB. Together with
     * faad2's HE-AAC v2 frames this overflowed musl's 128 KiB thread stack. */
    struct {
        int16_t pcm[WAVEFORM_READ_FRAMES * MAX_CHANNELS];
        uint64_t sums[WAVEFORM_MAX_BINS];
        uint64_t sample_counts[WAVEFORM_MAX_BINS];
        double rms[WAVEFORM_MAX_BINS];
    } * scratch = NULL;
    decoder_t dec;
    struct timespec started = { 0 }, now = { 0 }, last_yield = { 0 };
    bool opened = false, ok = false;
    uint64_t local_flac_file_size = 0;
    waveform_seek_anchor_t seek_anchor = { 0 };

    if (bins && count <= WAVEFORM_MAX_BINS) memset(bins, 0, count);
    if (!path || !path[0] || !bins || count == 0 || count > WAVEFORM_MAX_BINS ||
        is_stream_url(path) || remote_track_path_is_remote(path) ||
        (cancel && cancel(user))) return false;

    struct stat st;
    char waveform_source[600];
    const char * stat_path = path;
    if (cue_reference_is(path)) {
        if (!cue_reference_audio_path(path, waveform_source, sizeof(waveform_source))) return false;
        stat_path = waveform_source;
    }
    if (stat(stat_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    const char * ext = strrchr(stat_path, '.');
    if (ext && (strcasecmp(ext, ".dsf") == 0 || strcasecmp(ext, ".dff") == 0 || strcasecmp(ext, ".iso") == 0 ||
                strcasecmp(ext, ".ape") == 0)) return false;
    (void) clock_gettime(CLOCK_MONOTONIC, &started);
    last_yield = started;
    waveform_cancel_context_t cancel_context = { .cancel = cancel, .user = user, .started = started };
    scratch = calloc(1, sizeof(*scratch));
    if (!scratch) return false;
    if (!decoder_open(&dec, path)) goto done;
    opened = true;
    if (dec.type == DECODER_FLAC && dec.flac_file) {
        struct stat opened_file_stat;
        if (fstat(fileno(dec.flac_file), &opened_file_stat) == 0 &&
            opened_file_stat.st_size > 0)
            local_flac_file_size = (uint64_t)opened_file_stat.st_size;
    }

    if ((cancel && cancel(user)) || dec.type == DECODER_DSD || dec.type == DECODER_APE ||
        dec.total_frames == 0 || dec.sample_rate == 0 || dec.sample_rate > 192000 ||
        dec.channels == 0 || dec.channels > MAX_CHANNELS ||
        dec.total_frames > (uint64_t)dec.sample_rate * 60 * 60 * 2)
        goto done;

    uint64_t frame = 0;
    bool can_sparse_seek = dec.type == DECODER_WAV || dec.type == DECODER_AIFF ||
                           dec.type == DECODER_CAF || dec.type == DECODER_AAC ||
                           dec.type == DECODER_ALAC || dec.type == DECODER_WAVPACK ||
                           dec.type == DECODER_OPUS || dec.type == DECODER_FLAC ||
                           dec.type == DECODER_VORBIS || dec.type == DECODER_MP3;
    uint64_t window_frames = ((uint64_t)dec.sample_rate * WAVEFORM_WINDOW_MS) / 1000;
    if (window_frames == 0) window_frames = 1;
    bool sparse_seek = can_sparse_seek && dec.total_frames > (uint64_t)count * window_frames;
    if (sparse_seek && dec.type == DECODER_MP3 && !dec.segmented) {
        bool index_cancelled = false;
        if (!waveform_prepare_mp3_seek_table(&dec, path, &cancel_context, &index_cancelled)) {
            if (index_cancelled || waveform_cancel_or_expired(&cancel_context)) goto done;
            sparse_seek = false; /* A private index failure leaves the original decoder untouched. */
        }
    }
    unsigned consecutive_errors = 0;
    bool reached_eof = false;
    bool decoded_samples = false;
    size_t completed_bins = 0;
    for (size_t bin = 0; bin < count && !reached_eof; ++bin) {
        uint64_t bin_start = (dec.total_frames * (uint64_t)bin + count - 1) / count;
        uint64_t bin_end = (dec.total_frames * (uint64_t)(bin + 1) + count - 1) / count;
        if (bin_end <= bin_start) {
            completed_bins = bin + 1;
            if (progress && completed_bins % 4 == 0) {
                if (waveform_cancel_or_expired(&cancel_context)) goto done;
                waveform_normalize_prefix(bins, count, completed_bins, scratch->sums,
                                          scratch->sample_counts, scratch->rms);
                progress(bins, count, completed_bins, progress_user);
            }
            continue;
        }
        uint64_t read_start = bin_start;
        uint64_t read_end = bin_end;
        if (sparse_seek && bin_end - bin_start > window_frames) {
            read_start = bin_start + ((bin_end - bin_start) - window_frames) / 2;
            read_end = read_start + window_frames;
        }
        if (sparse_seek) {
            bool seek_ok = false;
            if (!(cancel && cancel(user))) {
                seek_ok = dec.type == DECODER_FLAC && !dec.segmented
                    ? waveform_flac_seeded_seek(&dec, read_start,
                                                local_flac_file_size,
                                                waveform_cancel_or_expired,
                                                &cancel_context, &seek_anchor)
                    : decoder_seek(&dec, read_start);
            }
            if (!seek_ok) goto done;
            frame = read_start;
            if (cancel && cancel(user)) goto done;
            (void) clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (uint64_t) timespec_diff_ns(&started, &now);
            if (elapsed > budget_ns) goto done;
        }
        uint64_t target_end = sparse_seek ? read_end : bin_end;
        if (frame > target_end) continue;
        while (frame < target_end) {
            if (waveform_cancel_or_expired(&cancel_context)) goto done;
            (void) clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (uint64_t) timespec_diff_ns(&started, &now);
            if (elapsed > budget_ns) goto done;

            uint64_t want = target_end - frame;
            if (want > WAVEFORM_READ_FRAMES) want = WAVEFORM_READ_FRAMES;
            decoder_read_result_t read = decoder_read_s16(&dec, want, scratch->pcm);
            if (read.status == DECODER_READ_FATAL_ERROR) goto done;
            if (read.status == DECODER_READ_RECOVERABLE_ERROR) {
                if (++consecutive_errors >= WAVEFORM_MAX_CONSECUTIVE_ERRORS) goto done;
                if (read.frames == 0) continue;
            } else {
                consecutive_errors = 0;
            }
            if (read.frames == 0) {
                /* Container estimates can outlive a truncated or damaged stream.
                 * A clean EOF is useful when earlier bins contain real samples. */
                if (read.status == DECODER_READ_EOF && decoded_samples) {
                    reached_eof = true;
                    break;
                }
                goto done;
            }
            if (read.frames > want) goto done;

            decoded_samples = true;
            for (uint64_t i = 0; i < read.frames; ++i) {
                for (unsigned ch = 0; ch < dec.channels; ++ch) {
                    int32_t sample = scratch->pcm[i * dec.channels + ch];
                    scratch->sums[bin] += (uint64_t)(sample * sample);
                    scratch->sample_counts[bin]++;
                }
            }
            frame += read.frames;
            if (read.status == DECODER_READ_EOF) {
                /* A decoder may report real samples with EOF on its final read.
                 * Preserve those bins and leave the remaining estimate empty. */
                reached_eof = true;
                break;
            }

            (void) clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t since_yield_ns = timespec_diff_ns(&last_yield, &now);
            if (since_yield_ns >= (int64_t)WAVEFORM_YIELD_INTERVAL_MS * 1000000) {
                if (waveform_cancel_or_expired(&cancel_context)) goto done;
                usleep(WAVEFORM_YIELD_SLEEP_US);
                last_yield = now;
            }
        }
        completed_bins = bin + 1;
        if (progress && completed_bins % 4 == 0) {
            if (waveform_cancel_or_expired(&cancel_context)) goto done;
            waveform_normalize_prefix(bins, count, completed_bins, scratch->sums,
                                      scratch->sample_counts, scratch->rms);
            progress(bins, count, completed_bins, progress_user);
        }
    }

    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    {
        uint64_t elapsed = (uint64_t) timespec_diff_ns(&started, &now);
        if (elapsed > budget_ns || !decoded_samples || waveform_cancel_or_expired(&cancel_context)) goto done;
    }
    waveform_normalize_prefix(bins, count, count, scratch->sums,
                              scratch->sample_counts, scratch->rms);
    if (progress && completed_bins && completed_bins % 4 != 0) {
        if (waveform_cancel_or_expired(&cancel_context)) goto done;
        /* This is the terminal prefix (possibly shortened by an early EOF). */
        waveform_normalize_prefix(bins, count, completed_bins, scratch->sums,
                                  scratch->sample_counts, scratch->rms);
        progress(bins, count, completed_bins, progress_user);
    }
    if (waveform_cancel_or_expired(&cancel_context)) goto done;
    ok = true;

done:
    if (opened) decoder_close(&dec);
    free(scratch);
    if (!ok) memset(bins, 0, count);
    return ok;
}
