#include "duration_lookup.h"
#include "../db/metadata_db.h"
#include "../../../core/process/worker_thread.h"
#include "../../../core/process/worker_notify.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct duration_lookup {
    pthread_t thread;
    char ** paths;
    size_t count;
    duration_lookup_result_t * results;
    uint32_t epoch;
    atomic_bool done, cancelled;
};

bool duration_info_from_probe(const audio_current_format_info_t * probe, duration_info_t * out) {
    if (!probe || !out || !probe->valid || !(probe->duration_seconds > 0.0) ||
        probe->duration_seconds > 4294967.0) return false;
    uint32_t ms = (uint32_t) (probe->duration_seconds * 1000.0 + 0.5);
    if (ms == 0) return false;
    out->duration_ms = ms;
    out->sample_rate = probe->source_sample_rate > 0 ? (uint32_t) probe->source_sample_rate : 0;
    out->bit_depth = probe->source_bit_depth > 0 && probe->source_bit_depth < 255 ? (uint8_t) probe->source_bit_depth : 0;
    out->codec = (uint8_t) probe->codec;
    out->flags = probe->is_dsd || probe->codec == AUDIO_CODEC_DSD ? DURATION_CACHE_FLAG_DSD : 0;
    return true;
}

bool duration_info_is_hires(const duration_info_t * info) {
    return (info->flags & DURATION_CACHE_FLAG_DSD) || info->sample_rate > 48000 || info->bit_depth > 16;
}

#define DURATION_LOOKUP_STACK (128u * 1024u)

static void * lookup_worker(void * arg) {
    duration_lookup_t * job = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND); /* a page is waiting; it also takes the metadata lock */
    if (!duration_cache_wait_loaded()) { /* no card: nothing is known, nothing can be stored */
        atomic_store(&job->done, true);
        worker_notify_ui();
        return NULL;
    }
    for (size_t i = 0; i < job->count && !atomic_load(&job->cancelled); i++) {
        duration_lookup_result_t * r = &job->results[i];
        if (!metadata_db_get_song_identity(job->paths[i], &r->mtime, &r->size)) {
            struct stat st;
            if (stat(job->paths[i], &st) != 0) continue;
            r->mtime = (int32_t) st.st_mtime;
            r->size = (int32_t) st.st_size;
        }
        r->identified = true;
        r->hit = duration_cache_get_blocking(duration_cache_key(job->paths[i], r->mtime, r->size), &r->info);
    }
    atomic_store(&job->done, true);
    worker_notify_ui(); /* the page plans its probes now, not at the next poll */
    return NULL;
}

duration_lookup_t * duration_lookup_start(const char * const * paths, size_t count) {
    if (!paths || count == 0) return NULL;
    duration_lookup_t * job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->paths = calloc(count, sizeof(char *));
    job->results = calloc(count, sizeof(*job->results));
    if (!job->paths || !job->results) goto fail;
    job->epoch = duration_cache_epoch(); /* before any read: results carry it back with every put */
    for (size_t i = 0; i < count; i++) {
        if (!paths[i] || !(job->paths[i] = strdup(paths[i]))) goto fail;
        job->count = i + 1;
    }
    if (worker_thread_create(&job->thread, DURATION_LOOKUP_STACK, lookup_worker, job) != 0) goto fail;
    return job;
fail:
    for (size_t i = 0; job->paths && i < job->count; i++) free(job->paths[i]);
    free(job->paths); free(job->results); free(job);
    return NULL;
}

uint32_t duration_lookup_epoch(const duration_lookup_t * job) { return job ? job->epoch : 0; }

bool duration_lookup_done(const duration_lookup_t * job) { return !job || atomic_load(&job->done); }

const duration_lookup_result_t * duration_lookup_result(const duration_lookup_t * job, size_t index) {
    return job && atomic_load(&job->done) && index < job->count ? &job->results[index] : NULL;
}

const char * duration_lookup_path(const duration_lookup_t * job, size_t index) {
    return job && index < job->count ? job->paths[index] : NULL;
}

void duration_lookup_cancel(duration_lookup_t * job) { if (job) atomic_store(&job->cancelled, true); }

void duration_lookup_destroy(duration_lookup_t * job) {
    if (!job || !atomic_load(&job->done)) return;
    pthread_join(job->thread, NULL);
    for (size_t i = 0; i < job->count; i++) free(job->paths[i]);
    free(job->paths); free(job->results); free(job);
}

void duration_lookup_cancel_and_destroy(duration_lookup_t * job) {
    if (!job) return;
    atomic_store(&job->cancelled, true);
    pthread_join(job->thread, NULL);
    for (size_t i = 0; i < job->count; i++) free(job->paths[i]);
    free(job->paths); free(job->results); free(job);
}
