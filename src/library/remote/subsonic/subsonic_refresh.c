#include "subsonic_refresh.h"
#include "subsonic_covers.h"
#include "../../../core/process/worker_notify.h"
#include "../../../core/process/worker_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_t worker_thread;
static http_cancel_token_t cancel_token;
static subsonic_refresh_job_t * running;
static atomic_bool finished;
static bool superseded;

static long long now_seconds(void) { return (long long) time(NULL); }

/* Downloads the whole list for job->kind into job->items. */
static bool fetch_list(subsonic_refresh_job_t * job) {
    switch (job->kind) {
        case SUBSONIC_LIST_ARTISTS: {
            subsonic_artist_t * list;
            bool ok = subsonic_get_artists(&job->server, &list, &job->count, &cancel_token);
            job->items = list;
            return ok;
        }
        case SUBSONIC_LIST_ALBUMS: {
            subsonic_album_t * list;
            bool ok = subsonic_get_all_albums(&job->server, &list, &job->count, &cancel_token);
            job->items = list;
            return ok;
        }
        default: {
            subsonic_playlist_t * list;
            bool ok = subsonic_get_playlists(&job->server, &list, &job->count, &cancel_token);
            job->items = list;
            return ok;
        }
    }
}

static bool stamp_equal(const subsonic_library_stamp_t * a, const subsonic_library_stamp_t * b) {
    return a->last_modified == b->last_modified && strcmp(a->last_scan, b->last_scan) == 0;
}

/* Keeps what a fetch brought: nothing when the list is unchanged, only the new
 * stamp when it moved, else the whole list. A songs file holds its stamp in
 * its first line, so it is rewritten whole for either. */
static void keep_fetched(subsonic_refresh_job_t * job, bool stamped, const subsonic_library_stamp_t * stamp) {
    job->ok = true;
    job->entry.synced_at = now_seconds();

    uint64_t hash = subsonic_cache_hash(job->kind, job->items, job->count);
    bool same_list = job->entry.hash != 0 && hash == job->entry.hash;
    bool stamp_moved = stamped && !stamp_equal(&job->entry.stamp, stamp);
    job->entry.hash = hash;
    if (stamped) job->entry.stamp = *stamp;
    job->changed = !same_list;
    if (subsonic_list_is_songs(job->kind)) {
        if (job->changed || stamp_moved)
            (void) subsonic_cache_songs_save(&job->server, job->kind, job->id, job->items, job->count, &job->entry);
    } else if (job->changed) {
        (void) subsonic_cache_save(&job->server, job->kind, job->items, job->count, &job->entry);
    } else if (stamp_moved) {
        (void) subsonic_cache_save_entry(&job->server, job->kind, &job->entry);
    }
    if (!job->changed) {
        free(job->items);
        job->items = NULL;
        job->count = 0;
    }
}

/* The songs of one album or playlist: no cheap marker to ask, so fetch them and
 * compare. The server forgetting the item removes its file. */
static void run_fetch_songs(subsonic_refresh_job_t * job) {
    subsonic_song_t * songs;
    bool album = job->kind == SUBSONIC_LIST_ALBUM_SONGS;
    bool ok = album ? subsonic_get_album_songs(&job->server, job->id, &songs, &job->count, &cancel_token)
                    : subsonic_get_playlist_songs(&job->server, job->id, &songs, &job->count, &cancel_token);
    if (!ok) {
        job->not_found = subsonic_last_error_code() == SUBSONIC_ERROR_NOT_FOUND;
        if (job->not_found) subsonic_cache_songs_remove(&job->server, job->kind, job->id);
        return;
    }
    job->items = songs;
    keep_fetched(job, true, &job->save_stamp);
}

/* Playlists have no cheap marker; every other list is judged by the library stamp. */
static void run_fetch(subsonic_refresh_job_t * job) {
    if (subsonic_list_is_songs(job->kind)) {
        run_fetch_songs(job);
        return;
    }
    bool stamped = job->kind != SUBSONIC_LIST_PLAYLISTS;
    subsonic_library_stamp_t stamp = {0};
    if (stamped && !subsonic_get_library_stamp(&job->server, &stamp, &cancel_token)) {
        if (http_cancel_token_is_cancelled(&cancel_token)) return;
        stamped = false; /* no stamp: fetch the list anyway and keep none */
    }
    if (stamped) {
        if (!job->force && subsonic_stamp_unchanged(&job->entry.stamp, &stamp)) {
            job->entry.synced_at = now_seconds(); /* in memory only: nothing worth a card write */
            job->ok = true;
            return;
        }
    }
    if (!fetch_list(job)) return;
    keep_fetched(job, stamped, &stamp);
}

static void * worker_main(void * arg) {
    subsonic_refresh_job_t * job = arg;
    switch (job->mode) {
        case SUBSONIC_REFRESH_FETCH:
            run_fetch(job);
            break;
        case SUBSONIC_REFRESH_PING:
            job->ok = subsonic_ping(&job->server, &cancel_token);
            break;
        case SUBSONIC_REFRESH_PRUNE:
            subsonic_cache_songs_prune(&job->server, SUBSONIC_SONG_FILES_MAX);
            subsonic_covers_prune(&job->server, SUBSONIC_COVER_THUMBS_MAX, SUBSONIC_COVER_LARGE_FILES_MAX);
            job->ok = true;
            break;
    }
    atomic_store_explicit(&finished, true, memory_order_release); /* written last */
    worker_notify_ui();
    return NULL;
}

bool subsonic_refresh_start(const subsonic_refresh_job_t * request) {
    if (running) return false;
    subsonic_refresh_job_t * job = malloc(sizeof(*job));
    if (!job) return false;
    *job = *request;
    job->ok = job->changed = job->not_found = false;
    job->items = NULL;
    job->count = 0;

    atomic_store_explicit(&finished, false, memory_order_relaxed);
    superseded = false;
    http_cancel_token_init(&cancel_token);
    if (pthread_create(&worker_thread, NULL, worker_main, job) != 0) {
        http_cancel_token_destroy(&cancel_token);
        free(job);
        return false;
    }
    running = job;
    return true;
}

bool subsonic_refresh_active(void) { return running != NULL; }

void subsonic_refresh_cancel(void) {
    if (!running) return;
    superseded = true;
    http_cancel_token_cancel(&cancel_token);
}

bool subsonic_refresh_take(subsonic_refresh_job_t * out) {
    if (!running || !atomic_load_explicit(&finished, memory_order_acquire)) return false;
    pthread_join(worker_thread, NULL);
    http_cancel_token_destroy(&cancel_token);
    subsonic_refresh_job_t * job = running;
    running = NULL;
    if (superseded) {
        free(job->items);
        free(job);
        return false;
    }
    *out = *job;
    free(job);
    return true;
}

void subsonic_refresh_shutdown(void) {
    subsonic_refresh_cancel();
    if (!running) return;
    pthread_join(worker_thread, NULL);
    http_cancel_token_destroy(&cancel_token);
    free(running->items);
    free(running);
    running = NULL;
}

enum { LOAD_RUNNING, LOAD_DONE, LOAD_ABANDONED };

struct subsonic_load {
    subsonic_server_t server;
    subsonic_list_kind_t kind;
    char id[64];
    subsonic_load_result_t result;
    atomic_int state;
};

static atomic_int loads_inflight; /* detached read threads still running, abandoned ones included */

int subsonic_load_inflight(void) { return atomic_load(&loads_inflight); }

static void load_release(subsonic_load_t * load) {
    free(load->result.items);
    free(load);
}

/* Whichever side finishes second frees the read. */
static void * load_main(void * arg) {
    subsonic_load_t * load = arg;
    subsonic_load_result_t * r = &load->result;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    r->ok = subsonic_list_is_songs(load->kind)
                ? subsonic_cache_songs_load(&load->server, load->kind, load->id, &r->items, &r->count, &r->entry)
                : subsonic_cache_load(&load->server, load->kind, &r->items, &r->count, &r->entry);
    atomic_fetch_sub(&loads_inflight, 1);
    if (atomic_exchange(&load->state, LOAD_DONE) == LOAD_ABANDONED) load_release(load);
    else worker_notify_ui();
    return NULL;
}

subsonic_load_t * subsonic_load_start(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id) {
    subsonic_load_t * load = calloc(1, sizeof(*load));
    if (!load) return NULL;
    load->server = *server;
    load->kind = kind;
    if (id) snprintf(load->id, sizeof(load->id), "%s", id);
    pthread_t thread;
    atomic_fetch_add(&loads_inflight, 1);
    if (worker_thread_create(&thread, 256 * 1024, load_main, load) != 0) {
        atomic_fetch_sub(&loads_inflight, 1);
        free(load);
        return NULL;
    }
    (void) pthread_detach(thread);
    return load;
}

bool subsonic_load_take(subsonic_load_t ** load, subsonic_load_result_t * result) {
    if (!*load || atomic_load(&(*load)->state) != LOAD_DONE) return false;
    *result = (*load)->result;
    free(*load);
    *load = NULL;
    return true;
}

void subsonic_load_drop(subsonic_load_t ** load) {
    if (!*load) return;
    if (atomic_exchange(&(*load)->state, LOAD_ABANDONED) == LOAD_DONE) load_release(*load);
    *load = NULL;
}

bool subsonic_list_load_open(subsonic_list_state_t * st, const subsonic_server_t * server, subsonic_list_kind_t kind,
                             const char * id) {
    if (st->load) return true;
    st->wants_load = subsonic_load_inflight() >= SUBSONIC_LOAD_MAX; /* a slot frees as reads finish */
    if (st->wants_load) return true;
    st->load = subsonic_load_start(server, kind, id);
    st->load_gen = st->installed_gen;
    return st->load != NULL;
}

bool subsonic_list_load_take(subsonic_list_state_t * st, subsonic_load_result_t * result) {
    if (!subsonic_load_take(&st->load, result)) return false;
    if (st->load_gen == st->installed_gen) return true;
    free(result->items);
    return false;
}

/* Anything parked, or waiting to be read from disk, is older than what is
 * being installed: it must never come back. */
void subsonic_list_supersede(subsonic_list_state_t * st) {
    st->wants_load = false;
    free(st->parked);
    st->parked = NULL;
    st->has_parked = false;
    st->installed_gen++;
}

void subsonic_list_park(subsonic_list_state_t * st, void * items, int count, const subsonic_cache_entry_t * entry) {
    subsonic_list_supersede(st); /* drops the older parked result and overtakes a read in flight */
    st->has_parked = true;
    st->parked = items;
    st->parked_count = count;
    st->parked_entry = *entry;
}

bool subsonic_list_unpark(subsonic_list_state_t * st, void ** items, int * count, subsonic_cache_entry_t * entry) {
    if (!st->has_parked) return false;
    *items = st->parked;
    *count = st->parked_count;
    *entry = st->parked_entry;
    st->has_parked = false;
    st->parked = NULL;
    return true;
}

bool subsonic_list_fetch_due(subsonic_list_state_t * st, bool worker_idle) {
    bool due = st->wants_fetch && worker_idle;
    if (due) st->wants_fetch = false;
    return due;
}

void subsonic_list_reset(subsonic_list_state_t * st) {
    subsonic_load_drop(&st->load);
    free(st->parked);
    memset(st, 0, sizeof(*st));
}

void subsonic_next_clear(subsonic_next_job_t * slot) { memset(slot, 0, sizeof(*slot)); }

void subsonic_next_store(subsonic_next_job_t * slot, const char * server_key) {
    slot->queued = true;
    snprintf(slot->server_key, sizeof(slot->server_key), "%s", server_key);
}

bool subsonic_next_take(subsonic_next_job_t * slot, const char * server_key) {
    bool wanted = slot->queued && strcmp(slot->server_key, server_key) == 0;
    subsonic_next_clear(slot);
    return wanted;
}
