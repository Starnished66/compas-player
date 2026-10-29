#define _POSIX_C_SOURCE 200809L
#include "track_probe.h"
#include "../../core/process/worker_thread.h"
#include "../../core/process/worker_notify.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* main.c defines the real hook. track_probe_test links this file alone. */
extern void install_thread_crash_altstack(void) __attribute__((weak));

/* decoder_open on HE-AAC v2 runs the same ~70 KiB faad2 chain as playback. */
#define TRACK_PROBE_THREAD_STACK_SIZE (256 * 1024)

typedef struct {
    bool valid;
    audio_codec_t codec;
    double duration_seconds;
    unsigned int source_sample_rate;
    unsigned int source_bit_depth;
    bool is_dsd;
    bool confirmed_failure; /* the format was positively rejected (AUDIO_PROBE_UNSUPPORTED) */
} track_probe_result_t;

struct track_probe_job {
    pthread_t thread;
    atomic_bool cancelled;
    atomic_bool done;
    atomic_size_t published;
    size_t count;
    size_t next;
    char * path_storage;
    const char * paths[TRACK_PROBE_MAX_TRACKS];
    track_probe_result_t results[TRACK_PROBE_MAX_TRACKS];
};

static uint64_t track_probe_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void * track_probe_worker(void * arg) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "trackprobe");
#endif
    worker_lower_priority(WORKER_NICE_BACKGROUND); /* a page is waiting for these */
    if (install_thread_crash_altstack) install_thread_crash_altstack(); /* see its own comment (main.c) */
    track_probe_job_t * job = arg;
    uint64_t last_notify_ms = 0;
    for (size_t i = 0; i < job->count; i++) {
        if (atomic_load_explicit(&job->cancelled, memory_order_acquire)) break;
        audio_current_format_info_t info = {0};
        audio_probe_status_t status = AUDIO_PROBE_ERROR;
        if (audio_probe_file_format_status(job->paths[i], &info, &status) && info.valid) {
            job->results[i].valid = true;
            job->results[i].codec = info.codec;
            job->results[i].duration_seconds = info.duration_seconds;
            job->results[i].source_sample_rate = info.source_sample_rate;
            job->results[i].source_bit_depth = info.source_bit_depth;
            job->results[i].is_dsd = info.is_dsd;
        } else {
            /* Only a positive rejection of the format is remembered; an open
             * or I/O failure may be transient. */
            job->results[i].confirmed_failure = status == AUDIO_PROBE_UNSUPPORTED;
        }
        /* Each slot is written once. The consumer's acquire load makes all
         * slots below published visible without taking a worker-held lock. */
        atomic_store_explicit(&job->published, i + 1, memory_order_release);
        /* Fast files finish in bursts: one wake-up per ~16 ms is enough, and
         * the completion below always notifies. */
        uint64_t now_ms = track_probe_now_ms();
        if (i + 1 < job->count && now_ms - last_notify_ms >= 16) {
            last_notify_ms = now_ms;
            worker_notify_ui();
        }
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    worker_notify_ui();
    return NULL;
}

track_probe_job_t * track_probe_job_start(const char * const * paths, size_t count) {
    if (!paths || !count || count > TRACK_PROBE_MAX_TRACKS) return NULL;
    size_t lengths[TRACK_PROBE_MAX_TRACKS];
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        if (!paths[i]) return NULL;
        size_t length = strnlen(paths[i], TRACK_PROBE_MAX_PATH_BYTES);
        if (length == TRACK_PROBE_MAX_PATH_BYTES) return NULL;
        lengths[i] = length + 1;
        bytes += lengths[i]; /* At most 200 * 2048 bytes. */
    }

    track_probe_job_t * job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->path_storage = malloc(bytes);
    if (!job->path_storage) {
        free(job);
        return NULL;
    }
    job->count = count;
    atomic_init(&job->cancelled, false);
    atomic_init(&job->done, false);
    atomic_init(&job->published, 0);
    char * dst = job->path_storage;
    for (size_t i = 0; i < count; i++) {
        memcpy(dst, paths[i], lengths[i]);
        job->paths[i] = dst;
        dst += lengths[i];
    }
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized &&
        pthread_attr_setstacksize(&attr, TRACK_PROBE_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    int create_rc = pthread_create(&job->thread, attr_ptr, track_probe_worker, job);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        free(job->path_storage);
        free(job);
        return NULL;
    }
    return job;
}

void track_probe_job_cancel(track_probe_job_t * job) {
    if (job) atomic_store_explicit(&job->cancelled, true, memory_order_release);
}

bool track_probe_job_next(track_probe_job_t * job, size_t * index,
                          audio_current_format_info_t * out) {
    if (!job || !index || !out) return false;
    size_t published = atomic_load_explicit(&job->published, memory_order_acquire);
    if (job->next >= published) return false;
    const track_probe_result_t * result = &job->results[job->next];
    memset(out, 0, sizeof(*out));
    out->valid = result->valid;
    out->codec = result->codec;
    out->duration_seconds = result->duration_seconds;
    out->source_sample_rate = result->source_sample_rate;
    out->source_bit_depth = result->source_bit_depth;
    out->is_dsd = result->is_dsd;
    *index = job->next++;
    return true;
}

bool track_probe_job_confirmed_failure(track_probe_job_t * job, size_t index) {
    if (!job || index >= atomic_load_explicit(&job->published, memory_order_acquire)) return false;
    return !job->results[index].valid && job->results[index].confirmed_failure;
}

bool track_probe_job_done(track_probe_job_t * job) {
    return !job || atomic_load_explicit(&job->done, memory_order_acquire);
}

void track_probe_job_destroy(track_probe_job_t * job) {
    if (!job || !track_probe_job_done(job)) return;
    if (pthread_join(job->thread, NULL) != 0) return;
    free(job->path_storage);
    free(job);
}

void track_probe_job_cancel_and_destroy(track_probe_job_t * job) {
    if (!job) return;
    track_probe_job_cancel(job);
    if (pthread_join(job->thread, NULL) != 0) return;
    free(job->path_storage);
    free(job);
}
