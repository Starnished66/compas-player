#include "track_metadata_worker.h"
#include "metadata_db.h"
#include "favorite_writer.h"
#include "ui_wake.h"
#include <pthread.h>
#include <stdio.h>
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

static bool make_display_hint(track_metadata_result_t * result) {
    track_metadata_request_t * request = &result->request;
    if ((!request->read_audio && !request->read_file) || request->metadata.has_title ||
        request->metadata.has_artist || request->metadata.has_album) return false;
    song_row_t row = {0};
    if (!metadata_db_get_song_by_path(request->path, &row) || strcmp(row.path, request->path) != 0)
        return false;
    track_metadata_t hint = {0};
#define COPY_HINT(field) do { \
    if (row.tags.field[0]) { \
        snprintf(hint.field, sizeof(hint.field), "%s", row.tags.field); \
        hint.has_##field = true; \
    } \
} while (0)
    COPY_HINT(title);
    COPY_HINT(artist);
    COPY_HINT(album);
    COPY_HINT(album_artist);
    COPY_HINT(genre);
#undef COPY_HINT
    if (row.tags.track_number > 0) {
        hint.track_number = row.tags.track_number;
        hint.has_track_number = true;
    }
    if (row.tags.disc_number > 0) {
        hint.disc_number = row.tags.disc_number;
        hint.has_disc_number = true;
    }
    if (row.tags.release_year > 0) {
        hint.release_year = row.tags.release_year;
        hint.has_release_year = true;
    }
    metadata_normalize_text_tags(&hint);
    request->metadata = hint;
    result->display_hint = true;
    result->enrichment_pending = true;
    return true;
}

static void * worker(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (!pending_valid[0] && !pending_valid[1] && !pending_valid[2] && !play_count)
            pthread_cond_wait(&cond, &mutex);
        /* Manual CURRENT display work goes first; NEXT remains independently queued. */
        int lane = pending_valid[TRACK_METADATA_CURRENT] ? TRACK_METADATA_CURRENT :
                   pending_valid[TRACK_METADATA_NEXT] ? TRACK_METADATA_NEXT :
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
        if (lane == TRACK_METADATA_CURRENT &&
            (result.request.read_audio || result.request.read_file)) {
            make_display_hint(&result);
            if (result.display_hint) {
                bool published = false;
                pthread_mutex_lock(&mutex);
                if (latest[lane] == result.request.token) {
                    completed[lane] = result;
                    completed_valid[lane] = true;
                    published = true;
                }
                pthread_mutex_unlock(&mutex);
                if (published) ui_wake_notify();
            }
        }
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
        result.display_hint = false;
        bool provisional_published = false;
        if (!obsolete && lane == TRACK_METADATA_CURRENT) {
            result.enrichment_pending = true;
            pthread_mutex_lock(&mutex);
            if (latest[lane] == result.request.token) {
                completed[lane] = result;
                completed_valid[lane] = true;
                provisional_published = true;
            }
            pthread_mutex_unlock(&mutex);
            if (provisional_published) ui_wake_notify();
        }
        if (!obsolete && lane != TRACK_METADATA_NEXT) {
            result.favorite = favorite_writer_is_set(result.request.path);
            if (result.request.probe)
                result.has_probe = audio_probe_file_format(result.request.path, &result.probe);
        }
        result.enrichment_pending = false;
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
