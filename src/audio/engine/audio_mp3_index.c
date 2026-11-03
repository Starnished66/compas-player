/* MP3 seek-point index: shared state, on-disk cache, background worker and
 * the request that the playback thread makes. All state is guarded by
 * audio_mutex unless declared atomic. */
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

/* Persistent cache for MP3 seek-point tables (mp3_seek_cache_load/_save
 * below) -- same on-SD, hidden-directory convention already used by
 * metadata_db.c's METADATA_DB_DIR and albumart.c's ALBUMART_DIR. */
#define COMPAS_DIR SD_COMPAS_ROOT
#define MP3_SEEK_CACHE_DIR COMPAS_DIR "/mp3seek"

/* One scan-only decoder may exist at a time. It never touches the live
 * decoder; the playback thread adopts only its small completed table. */
bool mp3_index_worker_active = false;
mp3_index_job_t * mp3_index_result = NULL;
bool mp3_seek_deferred = false;
uint64_t mp3_seek_deferred_frame = 0;
uint64_t mp3_seek_deferred_generation = 0;
unsigned int mp3_seek_deferred_sample_rate = 0;
unsigned int mp3_seek_retry_count = 0;
uint64_t mp3_seek_retry_after_ms = 0;

/* Lock-free cancellation hint for the worker thread (see
 * mp3_index_should_cancel()): mirrors playback_generation (updated at the
 * same handful of call sites that bump it, and on audio_stop()) so the
 * single-pass scan loop can check "is my job still relevant" without taking
 * audio_mutex on every MP3 frame -- that would serialize it against the
 * real-time playback thread on every iteration, defeating the point of
 * running this off-thread at all. A stale read here only wastes a little
 * extra worker CPU/SD time before the next check catches up -- the actual
 * adopt-vs-discard decision (audio_thread_func's own generation-match check
 * on mp3_index_result) still happens correctly under the mutex regardless. */
atomic_uint mp3_index_active_generation = 0;
atomic_bool mp3_index_stop_flag = false;

static bool mp3_index_should_cancel(uint64_t job_generation) {
    if (atomic_load_explicit(&mp3_index_stop_flag, memory_order_relaxed)) return true;
    return atomic_load_explicit(&mp3_index_active_generation, memory_order_relaxed) != (unsigned int) job_generation;
}

bool mp3_index_cancelled(void * user) {
    const mp3_index_cancel_t * c = user;
    return mp3_index_should_cancel(c->generation) ||
           (c->token && atomic_load_explicit(c->token, memory_order_relaxed) != c->expected_token);
}


/* audio_mutex must be held. A new user/resume target gets a fresh bounded
 * retry budget; worker retries preserve the same target and budget. */
void defer_mp3_seek_locked(const decoder_t * dec, uint64_t generation,
                           uint64_t frame) {
    mp3_seek_deferred = true;
    mp3_seek_deferred_frame = frame;
    mp3_seek_deferred_generation = generation;
    mp3_seek_deferred_sample_rate = dec->sample_rate;
    mp3_seek_retry_count = 0;
    mp3_seek_retry_after_ms = 0;
}

void finish_mp3_deferred_seek_locked(void) {
    mp3_seek_deferred = false;
    mp3_seek_retry_count = 0;
    mp3_seek_retry_after_ms = 0;
}

void free_mp3_index_job(mp3_index_job_t * job) {
    if (!job) return;
    free(job->points);
    free(job->path);
    free(job);
}

/* ---- Persistent on-disk seek-index cache -----------------------------
 * Keyed by a hash of the absolute path (same FNV-1a 64-bit convention as
 * albumart.c's thumbnail_key()); validated against the source file's size
 * and mtime (same trust level as albumart.c's own cache -- see its
 * source_mtime_of()/stored-vs-src check), not a full reopen+reparse. A
 * modified-but-same-size-and-mtime file is the one theoretical gap this
 * shares with every other mtime-keyed cache in this codebase; not treated
 * differently here. Saved only for a scan that ran to completion (see
 * mp3_index_worker()) -- never a cancelled partial one, which would
 * otherwise get permanently mistaken for the real, full index next load. */
#define MP3_SEEK_CACHE_MAGIC 0x33506d68u /* "hmp3" */
#define MP3_SEEK_CACHE_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t file_size;
    int64_t file_mtime;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t stream_length;
    uint64_t stream_start_offset;
    uint32_t delay_frames;
    uint32_t padding_frames;
    uint32_t point_count;
    /* Spacing this table was built at -- checked on load so retuning
     * MP3_SEEK_INDEX_INTERVAL_SECONDS invalidates old on-disk tables
     * immediately instead of silently serving stale-density ones until a
     * file's own size/mtime happens to change. */
    uint32_t interval_seconds;
} mp3_seek_cache_header_t;

typedef struct {
    uint64_t seek_pos_in_bytes;
    uint64_t pcm_frame_index;
    uint16_t mp3_frames_to_discard;
    uint16_t pcm_frames_to_discard;
    uint32_t reserved;
} mp3_seek_cache_point_t; /* 24 bytes -- see MP3_SEEK_INDEX_MAX_POINTS's own comment */

static uint64_t mp3_seek_cache_hash_path(const char * path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char * p = (const unsigned char *) path;
    if (*p) do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*++p);
    return hash;
}

static bool mp3_seek_cache_path(const char * source_path, char * out, size_t out_size) {
    int n = snprintf(out, out_size, "%s/%016llx.idx", MP3_SEEK_CACHE_DIR,
                      (unsigned long long) mp3_seek_cache_hash_path(source_path));
    return n > 0 && (size_t) n < out_size;
}

static bool mp3_seek_cache_load(const char * source_path, const mp3_index_job_t * job,
                                drmp3_seek_point ** out_points, drmp3_uint32 * out_count) {
    struct stat st;
    if (stat(source_path, &st) != 0) return false;

    char path[PATH_MAX];
    if (!mp3_seek_cache_path(source_path, path, sizeof(path))) return false;

    FILE * f = fopen(path, "rb");
    if (!f) return false;

    mp3_seek_cache_header_t hdr;
    bool ok = fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    if (ok) {
        ok = hdr.magic == MP3_SEEK_CACHE_MAGIC && hdr.version == MP3_SEEK_CACHE_VERSION &&
             hdr.file_size == (uint64_t) st.st_size && hdr.file_mtime == (int64_t) st.st_mtime &&
             hdr.sample_rate == job->stream.sample_rate && hdr.channels == job->stream.channels &&
             hdr.stream_length == job->stream.stream_length && hdr.stream_start_offset == job->stream.stream_start_offset &&
             hdr.delay_frames == job->stream.delay_frames && hdr.padding_frames == job->stream.padding_frames &&
             hdr.interval_seconds == MP3_SEEK_INDEX_INTERVAL_SECONDS &&
             hdr.point_count > 0 && hdr.point_count <= MP3_SEEK_INDEX_MAX_POINTS;
    }
    drmp3_seek_point * points = NULL;
    if (ok) {
        points = calloc(hdr.point_count, sizeof(*points));
        if (!points) ok = false;
    }
    for (uint32_t i = 0; ok && i < hdr.point_count; i++) {
        mp3_seek_cache_point_t raw;
        if (fread(&raw, 1, sizeof(raw), f) != sizeof(raw)) { ok = false; break; }
        points[i].seekPosInBytes = raw.seek_pos_in_bytes;
        points[i].pcmFrameIndex = raw.pcm_frame_index;
        points[i].mp3FramesToDiscard = (drmp3_uint16) raw.mp3_frames_to_discard;
        points[i].pcmFramesToDiscard = (drmp3_uint16) raw.pcm_frames_to_discard;
    }
    fclose(f);

    if (!ok) { free(points); return false; }
    *out_points = points;
    *out_count = hdr.point_count;
    return true;
}

/* Same mkstemp+fsync+rename+fsync-directory atomic-write pattern already
 * used by albumart.c's albumart_store_rgb565() / scanner_save_last_boot() --
 * best-effort: a failure here just means the next open scans again, no
 * different from a cold cache. */
static void mp3_seek_cache_save(const char * source_path, const mp3_index_job_t * job,
                                const drmp3_seek_point * points, drmp3_uint32 count) {
    if (count == 0) return;
    struct stat st;
    if (stat(source_path, &st) != 0) return;

    mkdir(COMPAS_DIR, 0755);
    mkdir(MP3_SEEK_CACHE_DIR, 0755);

    char path[PATH_MAX], tmp[PATH_MAX + 16];
    if (!mp3_seek_cache_path(source_path, path, sizeof(path))) return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp)) return;

    int fd = mkstemp(tmp);
    if (fd < 0) return;
    FILE * f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp); return; }

    mp3_seek_cache_header_t hdr = {
        .magic = MP3_SEEK_CACHE_MAGIC, .version = MP3_SEEK_CACHE_VERSION,
        .file_size = (uint64_t) st.st_size, .file_mtime = (int64_t) st.st_mtime,
        .sample_rate = job->stream.sample_rate, .channels = job->stream.channels,
        .stream_length = job->stream.stream_length, .stream_start_offset = job->stream.stream_start_offset,
        .delay_frames = job->stream.delay_frames, .padding_frames = job->stream.padding_frames,
        .point_count = count, .interval_seconds = MP3_SEEK_INDEX_INTERVAL_SECONDS,
    };
    bool ok = fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (drmp3_uint32 i = 0; ok && i < count; i++) {
        mp3_seek_cache_point_t raw = {
            .seek_pos_in_bytes = points[i].seekPosInBytes, .pcm_frame_index = points[i].pcmFrameIndex,
            .mp3_frames_to_discard = points[i].mp3FramesToDiscard,
            .pcm_frames_to_discard = points[i].pcmFramesToDiscard, .reserved = 0,
        };
        ok = fwrite(&raw, 1, sizeof(raw), f) == sizeof(raw);
    }
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (ok) {
        int dfd = open(MP3_SEEK_CACHE_DIR, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }
    if (!ok) unlink(tmp);
}

static void * mp3_index_worker(void * arg) {
    set_worker_thread_name("mp3index");
    install_thread_crash_altstack(); /* see its own comment (main.c) */
    mp3_index_job_t * job = arg;
    (void) nice(10); /* keep the single-core playback thread responsive */

    drmp3_seek_point * cached_points = NULL;
    drmp3_uint32 cached_count = 0;
    if (mp3_seek_cache_load(job->path, job, &cached_points, &cached_count)) {
        job->points = cached_points;
        job->count = cached_count;
        job->outcome = MP3_INDEX_READY;
        DBG_LOG("audio: MP3 seek-index loaded from cache (%u points, %s)\n",
                job->count, safe_path_tail(job->path));
    } else {
        struct timespec started, finished;
        clock_gettime(CLOCK_MONOTONIC, &started);

        job->points = calloc(job->count, sizeof(*job->points));
        drmp3 * scan = malloc(sizeof(*scan));
        if (!job->points || !scan) {
            free(scan);
            job->outcome = MP3_INDEX_TRANSIENT_FAILURE;
        } else {
            bool opened = drmp3_init_file(scan, job->path, NULL) != 0;
            bool same_file = opened && mp3_stream_matches(scan, &job->stream);
            if (!opened) {
                job->outcome = MP3_INDEX_TRANSIENT_FAILURE;
            } else if (!same_file) {
                job->outcome = MP3_INDEX_PERMANENT_FAILURE;
            } else {
                bool cancelled = false;
                mp3_index_cancel_t cancel = { .generation = job->generation };
                drmp3_uint32 placed = mp3_build_seek_points_cancellable(
                    scan, job->total_pcm_frames, job->count, job->points, &cancelled,
                    mp3_index_cancelled, &cancel);
                job->count = placed;
                if (placed > 0 && !cancelled) {
                    job->outcome = MP3_INDEX_READY;
                    mp3_seek_cache_save(job->path, job, job->points, job->count);
                } else {
                    /* A cancelled scan is reported as transient regardless of
                     * how much progress it made -- it belongs to a track the
                     * playback thread has already moved past, so its partial
                     * result (still allocated in job->points for the
                     * diagnostics above) is never adopted, matching the
                     * generation check at the adoption site. */
                    job->outcome = cancelled ? MP3_INDEX_TRANSIENT_FAILURE : MP3_INDEX_PERMANENT_FAILURE;
                }
            }
            if (opened) drmp3_uninit(scan);
            free(scan);
        }

        clock_gettime(CLOCK_MONOTONIC, &finished);
        uint64_t elapsed_ms = (uint64_t) (finished.tv_sec - started.tv_sec) * 1000ULL;
        if (finished.tv_nsec >= started.tv_nsec)
            elapsed_ms += (uint64_t) (finished.tv_nsec - started.tv_nsec) / 1000000ULL;
        else
            elapsed_ms -= 1000ULL - (uint64_t) (started.tv_nsec - finished.tv_nsec) / 1000000ULL;
        DBG_LOG("audio: MP3 seek-index scan %s (%u points, %" PRIu64 " ms, %s)\n",
                job->outcome == MP3_INDEX_READY ? "ready" : "failed",
                job->count, elapsed_ms, safe_path_tail(job->path));
        (void) elapsed_ms; /* DBG_LOG compiles out without TEST_BUILD_TAG */
    }

    pthread_mutex_lock(&audio_mutex);
    mp3_index_worker_active = false;
    mp3_index_result = job;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    return NULL;
}

/* audio_mutex must be held. Books one failed index setup: retried after a
 * delay, given up on after MP3_SEEK_RETRY_MAX attempts. Returns the value
 * request_mp3_seek_index_locked() reports. */
static bool mp3_index_setup_failed_locked(decoder_t * dec, const char * path, const char * what) {
    mp3_seek_retry_count++;
    mp3_seek_retry_after_ms = monotonic_ms() + MP3_SEEK_RETRY_DELAY_MS;
    DBG_LOG("audio: MP3 seek-index %s, retry %u/%u (%s)\n",
            what, mp3_seek_retry_count, MP3_SEEK_RETRY_MAX, safe_path_tail(path));
    if (mp3_seek_retry_count >= MP3_SEEK_RETRY_MAX) {
        dec->mp3_seek_index_attempted = true;
        finish_mp3_deferred_seek_locked();
    }
    return mp3_seek_deferred;
}

/* audio_mutex must be held. Setup failures retain the target but are
 * retried only after a delay, preventing a low-memory failure from turning
 * the playback loop into an allocation/thread-creation spin. */
bool request_mp3_seek_index_locked(decoder_t * dec, const char * path,
                                   uint64_t generation) {
    if (dec->mp3_seek_index_attempted) {
        finish_mp3_deferred_seek_locked();
        return false;
    }
    if (mp3_index_worker_active || mp3_index_result) return true;
    if (monotonic_ms() < mp3_seek_retry_after_ms) return true;

    uint64_t interval = (uint64_t) dec->sample_rate * MP3_SEEK_INDEX_INTERVAL_SECONDS;
    uint64_t desired = dec->total_frames / interval;
    if (desired < 2) {
        finish_mp3_deferred_seek_locked();
        return false;
    }
    if (desired > MP3_SEEK_INDEX_MAX_POINTS) desired = MP3_SEEK_INDEX_MAX_POINTS;

    /* Try the on-disk cache synchronously, right here on the playback
     * thread, before spawning a worker at all -- a hit is just a bounded
     * stat()+read (at most ~200 KiB, MP3_SEEK_INDEX_MAX_POINTS'-worth), and
     * this thread is already about to sit idle waiting on the defer/hold
     * mechanism regardless, so there is no real-time cost to paying that
     * bounded read here instead of via a pthread_create()'d worker plus a
     * full close_device()/reopen cycle. Without this, "replaying a song
     * should not require another scan" still paid a thread-hop and a
     * needless device toggle on every replay, sometimes slower in wall
     * time than the interval-based work it exists to avoid. A miss falls
     * through to the normal async scan unchanged. */
    mp3_index_job_t probe = { .stream = mp3_stream_id_of(dec->as.mp3) };
    drmp3_seek_point * cached_points = NULL;
    drmp3_uint32 cached_count = 0;
    if (mp3_seek_cache_load(path, &probe, &cached_points, &cached_count)) {
        if (drmp3_bind_seek_table(dec->as.mp3, cached_count, cached_points)) {
            dec->mp3_seek_points = cached_points;
            dec->mp3_seek_point_count = cached_count;
            dec->mp3_seek_index_attempted = true;
            finish_mp3_deferred_seek_locked();
            DBG_LOG("audio: MP3 seek-index loaded from cache synchronously (%u "
                    "points, %s)\n",
                    cached_count, safe_path_tail(path));
            return false; /* table is already bound -- nothing left to wait on */
        }
        free(cached_points);
    }

    mp3_index_job_t * job = calloc(1, sizeof(*job));
    if (!job || !(job->path = strdup(path))) {
        free_mp3_index_job(job);
        return mp3_index_setup_failed_locked(dec, path, "setup allocation failed");
    }
    job->generation = generation;
    job->stream = mp3_stream_id_of(dec->as.mp3);
    job->total_pcm_frames = dec->total_frames;
    job->count = (drmp3_uint32) desired;

    pthread_t worker;
    /* drmp3_init_file plus the frame walk that builds the seek table. */
    enum { MP3_INDEX_WORKER_STACK_SIZE = 256 * 1024 };
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized &&
        pthread_attr_setstacksize(&attr, MP3_INDEX_WORKER_STACK_SIZE) == 0)
        attr_ptr = &attr;
    mp3_index_worker_active = true;
    if (pthread_create(&worker, attr_ptr, mp3_index_worker, job) != 0) {
        if (attr_initialized) pthread_attr_destroy(&attr);
        mp3_index_worker_active = false;
        free_mp3_index_job(job);
        return mp3_index_setup_failed_locked(dec, path, "worker start failed");
    }
    pthread_detach(worker);
    if (attr_initialized) pthread_attr_destroy(&attr);
    return true;
}
