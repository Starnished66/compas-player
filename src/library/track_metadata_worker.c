#include "track_metadata_worker.h"
#include "metadata_db.h"
#include "favorite_writer.h"
#include "ui_wake.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define PLAY_EVENT_CAPACITY 64
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool ready;
static atomic_bool history_failed;
static track_metadata_request_t pending[TRACK_METADATA_LANES];
static bool pending_valid[TRACK_METADATA_LANES];
static track_metadata_result_t completed[TRACK_METADATA_LANES];
static bool completed_valid[TRACK_METADATA_LANES];
static uint64_t latest[TRACK_METADATA_LANES];
static char * play_events[PLAY_EVENT_CAPACITY];
static unsigned play_head, play_count;
static bool serviced_metadata;

static void * worker(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (!pending_valid[0] && !pending_valid[1] && !pending_valid[2] && !play_count)
            pthread_cond_wait(&cond, &mutex);
        /* Prepare gapless gain before the slower cosmetic DB work. */
        int lane = pending_valid[TRACK_METADATA_NEXT] ? TRACK_METADATA_NEXT :
                   pending_valid[TRACK_METADATA_CURRENT] ? TRACK_METADATA_CURRENT :
                   pending_valid[TRACK_METADATA_FAVORITE] ? TRACK_METADATA_FAVORITE : -1;
        if (play_count && (lane < 0 || (serviced_metadata && lane != TRACK_METADATA_NEXT))) {
            serviced_metadata = false;
            char * path = play_events[play_head];
            play_head = (play_head + 1) % PLAY_EVENT_CAPACITY;
            --play_count;
            pthread_mutex_unlock(&mutex);
            metadata_db_song_play_count_increment(path);
            free(path);
            continue;
        }
        serviced_metadata = true;
        track_metadata_result_t result = { .request = pending[lane] };
        pending_valid[lane] = false;
        pthread_mutex_unlock(&mutex);
        bool obsolete = false;
        if (result.request.read_audio) {
            audio_track_metadata_wait_result_t wait_result =
                audio_wait_track_metadata(result.request.audio_generation, &result.request.metadata);
            obsolete = wait_result == AUDIO_TRACK_METADATA_REPLACED;
            if (wait_result == AUDIO_TRACK_METADATA_STOPPED)
                metadata_read_without_artwork(result.request.path, &result.request.metadata);
        }
        else if (result.request.read_file)
            metadata_read_without_artwork(result.request.path, &result.request.metadata);
        /* Text readers omit blobs; defensively retain a scalar-only result. */
        free(result.request.metadata.picture_data);
        free(result.request.metadata.lyrics);
        result.request.metadata.picture_data = NULL;
        result.request.metadata.picture_size = 0;
        result.request.metadata.lyrics = NULL;
        if (!obsolete && lane != TRACK_METADATA_NEXT) {
            result.favorite = favorite_writer_is_set(result.request.path);
            if (result.request.probe)
                result.has_probe = audio_probe_file_format(result.request.path, &result.probe);
        }
        pthread_mutex_lock(&mutex);
        if (!obsolete && latest[lane] == result.request.token) {
            completed[lane] = result;
            completed_valid[lane] = true;
        }
        pthread_mutex_unlock(&mutex);
        if (!obsolete) ui_wake_notify();
    }
    return NULL;
}
bool track_metadata_worker_start(void) {
    pthread_mutex_lock(&mutex);
    if (!ready) {
        pthread_t thread;
        pthread_attr_t attr;
        bool attr_valid = pthread_attr_init(&attr) == 0;
        pthread_attr_t * options = NULL;
        if (attr_valid && pthread_attr_setstacksize(&attr, 512 * 1024) == 0) options = &attr;
        if (pthread_create(&thread, options, worker, NULL) == 0) {
            pthread_detach(thread);
            ready = true;
        }
        if (attr_valid) pthread_attr_destroy(&attr);
    }
    bool started = ready;
    pthread_mutex_unlock(&mutex);
    return started;
}
bool track_metadata_history_failed(void) {
    return atomic_load_explicit(&history_failed, memory_order_relaxed);
}
bool track_metadata_submit(int lane, const track_metadata_request_t * request) {
    if (lane < 0 || lane >= TRACK_METADATA_LANES || !request || !request->path[0] ||
        request->metadata.picture_data || request->metadata.lyrics) return false;
    if (!track_metadata_worker_start()) return false;
    pthread_mutex_lock(&mutex);
    pending[lane] = *request;
    pending_valid[lane] = true;
    completed_valid[lane] = false;
    latest[lane] = request->token;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    return true;
}
bool track_metadata_take(int lane, track_metadata_result_t * result) {
    if (lane < 0 || lane >= TRACK_METADATA_LANES || !result) return false;
    pthread_mutex_lock(&mutex);
    bool found = completed_valid[lane];
    if (found) { *result = completed[lane]; completed_valid[lane] = false; }
    pthread_mutex_unlock(&mutex);
    return found;
}
bool track_metadata_count_play(const char * path) {
    if (!path || !path[0]) return false;
    if (!track_metadata_worker_start()) {
        atomic_store_explicit(&history_failed, true, memory_order_relaxed);
        return false;
    }
    char * copy = strdup(path);
    if (!copy) {
        atomic_store_explicit(&history_failed, true, memory_order_relaxed);
        return false;
    }
    pthread_mutex_lock(&mutex);
    if (play_count == PLAY_EVENT_CAPACITY) {
        atomic_store_explicit(&history_failed, true, memory_order_relaxed);
        pthread_mutex_unlock(&mutex);
        free(copy);
        return false;
    }
    play_events[(play_head + play_count) % PLAY_EVENT_CAPACITY] = copy;
    ++play_count;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    return true;
}
