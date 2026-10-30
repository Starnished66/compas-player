#include "gui_artist_data.h"
#include "../../../audio/probe/track_probe.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../../../library/metadata/durations/duration_lookup.h"
#include "../../../core/process/worker_thread.h"
#include "../../../core/process/worker_notify.h"
#include <unistd.h>
#include <lvgl/lvgl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* Artists with more tracks than this keep their total time hidden. */
#define ARTIST_MAX_TRACKS 20000
#define ARTIST_MAX_GENRES 64

typedef struct { char album[128], album_artist[128]; int tracks, year; } album_agg_t;
typedef struct { char name[128]; int count; } genre_agg_t;

/* Worker-owned until `done`; the UI thread owns the track arrays after that. */
typedef struct {
    metadata_db_group_kind_t kind;
    char name[128];
    metadata_db_album_sort_t sort;
    int32_t generation;
    atomic_bool cancel, done;
    pthread_t thread;
    album_agg_t * albums; int album_count, album_cap;
    genre_agg_t genres[ARTIST_MAX_GENRES]; int genre_count;
    int songs, year_min, year_max;
    char ** paths; int32_t * mtime, * size; uint8_t * state; /* 0 miss, 1 known, 2 positively rejected, 3 unknown for this visit */
    int track_cap, track_count;
    bool tracks_overflow;
    /* The tracks the cache did not know, in order, built once by the worker. The
     * UI probes them from `miss_cursor` on and keeps the totals incrementally,
     * so a settled artist costs nothing per poll. */
    int * miss; int miss_count, miss_cursor;
    int unresolved;       /* tracks still in state 0 */
    int unknown;          /* tracks in state 3: not probed, or failed in a way that may be transient */
    uint32_t epoch;       /* duration-cache storage epoch when the job started: its puts carry it */
    double known_seconds; /* sum of the tracks in state 1 */
} job_t;

#define ARTIST_WORKER_STACK (128u * 1024u)
/* A new job is refused (and retried on a later poll) while this many retired
 * workers have not finished: joining a worker that waits on the card would
 * stall the UI thread. */
#define ARTIST_RETIRE_ADMIT 4
#define ARTIST_RETIRE_SLOTS 8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static job_t * current;           /* the page's job; its summary/albums are read under `lock` */
static gui_artist_summary_t summary;
static album_agg_t * published_albums; static int published_album_count;
static gui_artist_revisions_t revisions;
static job_t * retired[ARTIST_RETIRE_SLOTS];
static struct { bool valid; metadata_db_group_kind_t kind; char name[128]; metadata_db_album_sort_t sort; } deferred_open;

/* Probe state (UI thread). */
static track_probe_job_t * probe;
static job_t * probe_owner;       /* the job whose tracks the batch indexes; kept alive until it drains */
static int probe_batch[TRACK_PROBE_MAX_TRACKS]; static int probe_batch_n, probe_fails;
static int probe_batch_cursor;    /* miss_cursor before the batch, to rewind a cancelled one */
static uint32_t probe_retry_tick;
static bool probe_cancelled;
static job_t * probe_gate_job;     /* the job whose finished aggregate a poll has already seen */

static bool is_unknown_genre(const char * g) { return !g[0] || strcasecmp(g, "Unknown Genre") == 0; }

static bool visit(const song_row_t * song, int32_t mtime, int32_t size, void * user) {
    job_t * job = user;
    if (atomic_load(&job->cancel)) return false;
    job->songs++;
    int year = song->tags.release_year;
    if (year > 0) {
        if (!job->year_min || year < job->year_min) job->year_min = year;
        if (year > job->year_max) job->year_max = year;
    }
    album_agg_t * a = NULL;
    for (int i = job->album_count - 1; i >= 0 && i >= job->album_count - 8 && !a; i--) /* songs arrive grouped */
        if (!strcasecmp(job->albums[i].album, song->tags.album) &&
            !strcasecmp(job->albums[i].album_artist, song->tags.album_artist)) a = &job->albums[i];
    for (int i = 0; i < job->album_count - 8 && !a; i++)
        if (!strcasecmp(job->albums[i].album, song->tags.album) &&
            !strcasecmp(job->albums[i].album_artist, song->tags.album_artist)) a = &job->albums[i];
    if (!a) {
        if (job->album_count == job->album_cap) {
            int cap = job->album_cap ? job->album_cap * 2 : 32;
            album_agg_t * grown = realloc(job->albums, (size_t) cap * sizeof(*grown));
            if (!grown) return false;
            job->albums = grown; job->album_cap = cap;
        }
        a = &job->albums[job->album_count++];
        memset(a, 0, sizeof(*a));
        snprintf(a->album, sizeof(a->album), "%s", song->tags.album);
        snprintf(a->album_artist, sizeof(a->album_artist), "%s", song->tags.album_artist);
    }
    a->tracks++;
    if (year > 0 && (!a->year || year < a->year)) a->year = year;
    if (!is_unknown_genre(song->tags.genre)) {
        genre_agg_t * g = NULL;
        for (int i = 0; i < job->genre_count && !g; i++) if (!strcasecmp(job->genres[i].name, song->tags.genre)) g = &job->genres[i];
        if (!g && job->genre_count < ARTIST_MAX_GENRES) {
            g = &job->genres[job->genre_count++];
            snprintf(g->name, sizeof(g->name), "%s", song->tags.genre);
            g->count = 0;
        }
        if (g) g->count++;
    }
    if (job->track_count < ARTIST_MAX_TRACKS) {
        if (job->track_count == job->track_cap) {
            int cap = job->track_cap ? job->track_cap * 2 : 256;
            char ** p = realloc(job->paths, (size_t) cap * sizeof(*p));
            if (p) job->paths = p;
            int32_t * m = realloc(job->mtime, (size_t) cap * sizeof(*m));
            if (m) job->mtime = m;
            int32_t * s = realloc(job->size, (size_t) cap * sizeof(*s));
            if (s) job->size = s;
            uint8_t * st = realloc(job->state, (size_t) cap);
            if (st) job->state = st;
            if (!p || !m || !s || !st) return false;
            job->track_cap = cap;
        }
        char * path = strdup(song->path);
        if (!path) return false;
        int i = job->track_count++;
        job->paths[i] = path; job->mtime[i] = mtime; job->size[i] = size; job->state[i] = 0;
    } else job->tracks_overflow = true;
    return true;
}

static void free_job(job_t * job) {
    if (!job) return;
    for (int i = 0; i < job->track_count; i++) free(job->paths[i]);
    free(job->paths); free(job->mtime); free(job->size); free(job->state); free(job->miss); free(job->albums);
    free(job);
}

/* A settled job (every duration resolved) never looks at its track list again: the totals and the
 * album aggregates are all the page reads. Drops the list (about 130 bytes per track) once the
 * page is left; a re-open of the same artist keeps this job, and finds all it shows still here. */
static void drop_settled_tracks(job_t * job) {
    for (int i = 0; i < job->track_count; i++) free(job->paths[i]);
    free(job->paths); free(job->mtime); free(job->size); free(job->state); free(job->miss);
    job->paths = NULL; job->mtime = job->size = NULL; job->state = NULL; job->miss = NULL;
    job->track_cap = job->track_count = job->miss_count = job->miss_cursor = 0;
}

/* Forgets a partial walk (the database was republished underneath it). */
static void reset_aggregate(job_t * job) {
    for (int i = 0; i < job->track_count; i++) free(job->paths[i]);
    free(job->paths); free(job->mtime); free(job->size); free(job->state); free(job->albums);
    job->paths = NULL; job->mtime = job->size = NULL; job->state = NULL; job->albums = NULL;
    job->track_cap = job->track_count = job->album_count = job->album_cap = job->genre_count = 0;
    job->songs = job->year_min = job->year_max = 0;
    job->tracks_overflow = false;
}

static void totals(const job_t * job, bool * known, double * seconds) {
    /* A rejected track is known but unplayable: it adds nothing to the total.
     * An unknown one (transient error, path too long, probe gave up) hides it. */
    *known = !job->tracks_overflow && job->track_count > 0 && job->unresolved == 0 && job->unknown == 0;
    *seconds = job->known_seconds;
}

/* Moves one track out of "not yet looked at" and keeps the running totals. */
static void resolve(job_t * job, int t, uint8_t new_state, const duration_info_t * info) {
    if (job->state[t] != 0) return;
    job->state[t] = new_state;
    job->unresolved--;
    if (new_state == 3) job->unknown++;
    if (new_state == 1 && info) job->known_seconds += info->duration_ms / 1000.0;
}

/* Publishes the total only when what the header shows (the chip appears once
 * every track is known) changes, so partial batches cost the page nothing. */
static void publish_totals(job_t * job) {
    bool known; double seconds;
    totals(job, &known, &seconds);
    pthread_mutex_lock(&lock);
    if (current == job) {
        bool changed = known != summary.duration_known || (known && seconds != summary.total_seconds);
        summary.duration_known = known; summary.total_seconds = seconds;
        if (changed) revisions.duration++;
    }
    pthread_mutex_unlock(&lock);
}

static void * worker(void * arg) {
    job_t * job = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    bool walked = metadata_db_walk_group_songs(job->kind, job->name, visit, job);
    /* The walk stops when the database generation changes under it; start again
     * on the new one rather than publish nothing. */
    for (int attempt = 0; !walked && attempt < 2 && !atomic_load(&job->cancel); attempt++) {
        reset_aggregate(job);
        walked = metadata_db_walk_group_songs(job->kind, job->name, visit, job);
    }
    if (!walked || atomic_load(&job->cancel)) { atomic_store(&job->done, true); worker_notify_ui(); return NULL; }

    gui_artist_summary_t s = { .ready = true, .songs = job->songs, .albums = job->album_count,
                               .year_min = job->year_min, .year_max = job->year_max };
    int best = 0;
    for (int i = 0; i < job->genre_count; i++)
        if (job->genres[i].count > best) { best = job->genres[i].count; snprintf(s.genre, sizeof(s.genre), "%s", job->genres[i].name); }
    group_row_t first[ARTIST_PAGE_MOSAIC_ALBUMS];
    int n = metadata_db_get_albums_for_group_sorted(job->kind, job->name, job->sort, 0, ARTIST_PAGE_MOSAIC_ALBUMS, first);
    for (int i = 0; i < n; i++) {
        uint64_t key = 0;
        if (first[i].album_artist[0]) {
            albumart_info_t info = {0};
            snprintf(info.albumartist, sizeof(info.albumartist), "%.127s", first[i].album_artist);
            snprintf(info.album, sizeof(info.album), "%.127s", first[i].name);
            key = albumart_thumbnail_key(&info);
        } else {
            song_row_t rep;
            if (metadata_db_get_song_by_id(first[i].first_song_id, &rep)) key = song_album_thumbnail_key(&rep);
        }
        s.mosaic_song_id[s.mosaic_count] = first[i].first_song_id;
        s.mosaic_key[s.mosaic_count++] = key;
    }
    /* Durations known from the cache; the misses are probed on the UI side. */
    (void) duration_cache_wait_loaded(); /* with no card nothing hits and every track is probed */
    job->miss = job->track_count ? malloc((size_t) job->track_count * sizeof(int)) : NULL;
    for (int i = 0; i < job->track_count && !atomic_load(&job->cancel); i++) {
        duration_info_t info;
        job->unresolved++;
        if (duration_cache_get_blocking(duration_cache_key(job->paths[i], job->mtime[i], job->size[i]), &info)) {
            if (duration_info_is_failed(&info)) resolve(job, i, 2, NULL);
            else resolve(job, i, 1, &info);
        } else if (strlen(job->paths[i]) >= TRACK_PROBE_MAX_PATH_BYTES || !job->miss) {
            resolve(job, i, 3, NULL); /* cannot be probed (or no memory to track it): unknown, not rejected */
        } else job->miss[job->miss_count++] = i;
    }
    bool known; double seconds;
    totals(job, &known, &seconds);
    s.duration_known = known; s.total_seconds = seconds;
    album_agg_t * copy = job->album_count ? malloc((size_t) job->album_count * sizeof(*copy)) : NULL;
    if (copy) memcpy(copy, job->albums, (size_t) job->album_count * sizeof(*copy));
    pthread_mutex_lock(&lock);
    if (current == job) {
        free(published_albums);
        published_albums = copy; published_album_count = copy ? job->album_count : 0;
        copy = NULL;
        summary = s;
        revisions.albums++; revisions.summary++; revisions.duration++;
    }
    pthread_mutex_unlock(&lock);
    free(copy);
    atomic_store(&job->done, true);
    worker_notify_ui(); /* summary, albums and durations are ready: show them, start the probes */
    return NULL;
}

static void cancel_probe(void) {
    if (probe && !probe_cancelled) { track_probe_job_cancel(probe); probe_cancelled = true; }
}

static void release_probe(void) {
    track_probe_job_destroy(probe);
    probe = NULL;
    probe_owner = NULL;
    probe_batch_n = 0;
}

static int retiring_count(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) if (retired[i]) n++;
    return n;
}

/* Never joins: the worker may be waiting on the card. Admission control in
 * gui_artist_data_open() keeps the slots from filling up. */
static void retire(job_t * job) {
    if (!job) return;
    atomic_store(&job->cancel, true);
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (!retired[i]) { retired[i] = job; return; }
    /* Unreachable while open() refuses new jobs at ARTIST_RETIRE_ADMIT; if it ever
     * happens the worker is already cancelled and is left to finish on its own. */
}

static void reap(void) {
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (retired[i] && retired[i] != probe_owner && atomic_load(&retired[i]->done)) {
            pthread_join(retired[i]->thread, NULL); /* done: only reaps the exiting thread */
            free_job(retired[i]);
            retired[i] = NULL;
        }
}

static void start_job(metadata_db_group_kind_t kind, const char * name, metadata_db_album_sort_t sort) {
    int32_t generation = 0;
    (void) metadata_db_try_get_generation(&generation);
    job_t * job = calloc(1, sizeof(*job));
    if (!job) return;
    job->kind = kind; job->sort = sort; job->generation = generation;
    job->epoch = duration_cache_epoch(); /* before the worker reads the cache */
    snprintf(job->name, sizeof(job->name), "%s", name);
    pthread_mutex_lock(&lock);
    memset(&summary, 0, sizeof(summary));
    free(published_albums); published_albums = NULL; published_album_count = 0;
    current = job;
    revisions.albums++; revisions.summary++; revisions.duration++;
    pthread_mutex_unlock(&lock);
    if (worker_thread_create(&job->thread, ARTIST_WORKER_STACK, worker, job) != 0) {
        pthread_mutex_lock(&lock); current = NULL; pthread_mutex_unlock(&lock);
        free_job(job);
    }
}

void gui_artist_data_open(metadata_db_group_kind_t kind, const char * name, metadata_db_album_sort_t sort) {
    int32_t generation = 0;
    (void) metadata_db_try_get_generation(&generation);
    if (current && current->kind == kind && current->sort == sort && current->generation == generation &&
        !strcmp(current->name, name)) return;
    if (!current && deferred_open.valid && deferred_open.kind == kind && deferred_open.sort == sort &&
        !strcmp(deferred_open.name, name)) return;
    gui_artist_data_close();
    if (retiring_count() >= ARTIST_RETIRE_ADMIT) {
        /* Too many cancelled workers are still winding down: try again on a later poll. */
        deferred_open.valid = true; deferred_open.kind = kind; deferred_open.sort = sort;
        snprintf(deferred_open.name, sizeof(deferred_open.name), "%s", name);
        return;
    }
    start_job(kind, name, sort);
}

void gui_artist_data_close(void) {
    cancel_probe();
    deferred_open.valid = false;
    pthread_mutex_lock(&lock);
    job_t * old = current;
    current = NULL;
    memset(&summary, 0, sizeof(summary));
    free(published_albums); published_albums = NULL; published_album_count = 0;
    revisions.albums++; revisions.summary++; revisions.duration++;
    pthread_mutex_unlock(&lock);
    retire(old); /* a running probe batch keeps it alive until it drains */
}

bool gui_artist_data_snapshot(gui_artist_summary_t * out, gui_artist_revisions_t * rev) {
    pthread_mutex_lock(&lock);
    *out = summary;
    if (rev) *rev = revisions;
    pthread_mutex_unlock(&lock);
    return out->ready;
}

bool gui_artist_data_album(const char * album, const char * album_artist, int * year, int * tracks) {
    bool found = false;
    pthread_mutex_lock(&lock);
    for (int pass = 0; pass < 2 && !found; pass++)
        for (int i = 0; i < published_album_count && !found; i++) {
            const album_agg_t * a = &published_albums[i];
            if (strcasecmp(a->album, album)) continue;
            if (pass == 0 && album_artist[0] && strcasecmp(a->album_artist, album_artist)) continue;
            *year = a->year; *tracks = a->tracks; found = true;
        }
    pthread_mutex_unlock(&lock);
    return found;
}

/* FNV-1a over the lower-cased album artist (what gui_artist_data_album() compares, ignoring case);
 * 0 for none. Cached rows keep it so their facts can be looked up again without the strings. */
uint64_t gui_artist_data_album_key(const char * album_artist) {
    if (!album_artist || !album_artist[0]) return 0;
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) album_artist; *p; p++)
        h = (h ^ (unsigned char) tolower(*p)) * UINT64_C(1099511628211);
    return h ? h : 1;
}

bool gui_artist_data_album_by_key(const char * album, uint64_t album_artist_key, int * year, int * tracks) {
    bool found = false;
    pthread_mutex_lock(&lock);
    for (int pass = 0; pass < 2 && !found; pass++)
        for (int i = 0; i < published_album_count && !found; i++) {
            const album_agg_t * a = &published_albums[i];
            if (strcasecmp(a->album, album)) continue;
            if (pass == 0 && album_artist_key && gui_artist_data_album_key(a->album_artist) != album_artist_key) continue;
            *year = a->year; *tracks = a->tracks; found = true;
        }
    pthread_mutex_unlock(&lock);
    return found;
}

static void drain_probe(void) {
    job_t * owner = probe_owner;
    bool done = track_probe_job_done(probe), drained = false;
    for (int budget = 0; budget < 8; budget++) {
        size_t index; audio_current_format_info_t info;
        if (!track_probe_job_next(probe, &index, &info)) { drained = true; break; }
        if (budget == 7) worker_notify_ui(); /* budget spent with results left: come back next pass */
        if (!owner || (int) index >= probe_batch_n) continue;
        int t = probe_batch[index];
        if (t >= owner->track_count || owner->state[t] != 0) continue;
        duration_info_t value;
        if (duration_info_from_probe(&info, &value)) {
            resolve(owner, t, 1, &value);
            duration_cache_put(owner->epoch, owner->paths[t], owner->mtime[t], owner->size[t], &value);
        } else if (!probe_cancelled) {
            /* Rejected (and remembered) only when the format itself was; an I/O or
             * resource failure leaves the track unknown and the total hidden. */
            bool rejected = track_probe_job_confirmed_failure(probe, index);
            resolve(owner, t, rejected ? 2 : 3, NULL);
            if (rejected)
                duration_cache_put_failed(owner->epoch, owner->paths[t], owner->mtime[t], owner->size[t]);
        }
    }
    if (done && drained) {
        /* A cancelled batch left tracks unprobed: they stay at the head of the list. */
        if (probe_cancelled && owner) owner->miss_cursor = probe_batch_cursor;
        release_probe();
        if (owner && owner == current) publish_totals(owner);
    }
}

bool gui_artist_data_busy(void) {
    job_t * job = current;
    return probe || (job && !atomic_load(&job->done));
}

void gui_artist_data_poll(bool visible) {
    reap();
    if (deferred_open.valid && retiring_count() < ARTIST_RETIRE_ADMIT) {
        deferred_open.valid = false;
        start_job(deferred_open.kind, deferred_open.name, deferred_open.sort);
    }
    job_t * job = current;
    /* Probe results go to the job that started the batch, even after the page moved on. */
    if (probe) drain_probe();
    if (probe_cancelled && !probe) probe_cancelled = false;
    if (probe && job != probe_owner) cancel_probe();
    if (probe && !visible) cancel_probe();
    if (job && !probe && !visible && atomic_load(&job->done) && job->unresolved == 0 && job->paths)
        drop_settled_tracks(job);
    /* Settled (or nothing to learn): constant time, no allocation. */
    if (!job || probe || !visible || !atomic_load(&job->done) || job->unresolved == 0 || job->tracks_overflow) return;
    /* The pass that sees the finished aggregate draws the page's big update: the probe batch starts
     * on the next one (asked for at once), so its threads do not compete with that frame. */
    if (probe_gate_job != job) {
        probe_gate_job = job;
        worker_notify_ui();
        return;
    }
    if (probe_fails && lv_tick_elaps(probe_retry_tick) < 1000) return;
    const char * paths[TRACK_PROBE_MAX_TRACKS];
    int n = 0, cursor = job->miss_cursor;
    while (cursor < job->miss_count && n < TRACK_PROBE_MAX_TRACKS) {
        int t = job->miss[cursor++];
        if (job->state[t] != 0) continue;
        paths[n] = job->paths[t]; probe_batch[n++] = t;
    }
    if (n == 0) return;
    probe = track_probe_job_start(paths, (size_t) n);
    if (probe) {
        probe_batch_n = n; probe_owner = job; probe_cancelled = false; probe_fails = 0;
        probe_batch_cursor = job->miss_cursor;
        job->miss_cursor = cursor;
    } else {
        probe_retry_tick = lv_tick_get();
        if (++probe_fails >= 3) {
            /* Give up so the page settles; the tracks are tried again next visit. */
            for (int i = job->miss_cursor; i < job->miss_count; i++) resolve(job, job->miss[i], 3, NULL);
            publish_totals(job);
        }
    }
}

/* Final shutdown only; waits until the shared deadline for a worker that is still reading the card. */
static void join_bounded(job_t * job, int64_t deadline_ms) {
    atomic_store(&job->cancel, true);
    while (!atomic_load(&job->done) && worker_ms_left(deadline_ms) > 0) usleep(5000);
    if (atomic_load(&job->done)) { pthread_join(job->thread, NULL); free_job(job); }
    /* else: the process is exiting; the cancelled worker is abandoned. */
}

void gui_artist_data_shutdown(int64_t deadline_ms) {
    if (probe) track_probe_job_cancel_and_destroy(probe);
    probe = NULL; probe_owner = NULL; probe_batch_n = 0;
    pthread_mutex_lock(&lock);
    job_t * old = current;
    current = NULL;
    pthread_mutex_unlock(&lock);
    if (old) join_bounded(old, deadline_ms);
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (retired[i]) { join_bounded(retired[i], deadline_ms); retired[i] = NULL; }
    (void) duration_cache_flush_bounded((uint32_t) worker_ms_left(deadline_ms));
}
