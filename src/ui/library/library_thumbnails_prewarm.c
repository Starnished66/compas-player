#include "library_thumbnails_internal.h"
#include "../../core/ui/display/ui_wake.h"
#include "../../core/process/worker_thread.h"
#include "browse/gui_library_async.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../core/system/logging/db_log.h"
#include <stdlib.h>

static metadata_db_group_kind_t prewarm_kind(int kind_index) {
    return kind_index ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST;
}

prewarm_job_t * prewarm_job;         /* UI thread only: the job not yet reaped or abandoned */
atomic_bool prewarm_active;          /* a job exists (set at launch, cleared when it is reaped or abandoned) */
static atomic_int prewarm_orphans;   /* given-up jobs whose worker still runs */
int prewarm_done_epoch[2] = { -1, -1 };
int prewarm_done_first[2];
int prewarm_keys_count[2];
bool prewarm_boot_pending[2];          /* keys came from the boot first-screen warm; valid once it completed */
uint64_t prewarm_keys[2][PREWARM_MAX]; /* artist keys committed by the last finished run */

#define PREWARM_WAIT_MS 3000
atomic_bool prewarm_waiting;         /* a request is waiting for the warmer to yield */
static uint32_t prewarm_wait_tick;
static int prewarm_wait_kind_index, prewarm_wait_first, prewarm_wait_rows;

static void prewarm_free_job(prewarm_job_t * job) {
    if (!job) return;
    int published = atomic_load(&job->published);
    for (int i = 0; i < published; i++) free(job->results[i].pixels);
    free(job);
}

static bool prewarm_cancel_cb(void * user_data) {
    prewarm_job_t * job = user_data;
    return atomic_load(&job->cancel) || job->epoch != atomic_load(&album_thumbnail_cache_epoch);
}

static void * prewarm_thread_func(void * arg) {
    prewarm_job_t * job = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    install_thread_crash_altstack();
    group_row_t rows[PREWARM_MAX];
    int n = metadata_db_get_groups_page(job->kind, job->first, job->count, rows);
    bool clean = n >= 0;
    for (int i = 0; i < n; i++) {
        if (prewarm_cancel_cb(job)) { clean = false; break; }
        uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
        artist_thumbnail_resolution_t resolution;
        (void) artist_thumbnail_load_or_decode(rows[i].first_song_id, job->kind, rows[i].name,
                    ARTWORK_PRIO_THUMBNAIL, true, artist_key, prewarm_cancel_cb, job, &resolution);
        if (prewarm_cancel_cb(job)) {
            free(resolution.pixels);
            clean = false;
            break;
        }
        job->results[i] = artist_handoff(job->epoch, artist_key, job->kind, rows[i].first_song_id,
                                         &resolution, false);
        atomic_store_explicit(&job->published, i + 1, memory_order_release);
    }
    job->finished_clean = clean;
    atomic_store_explicit(&job->done, true, memory_order_release);
    if (library_async_worker_done(&job->fin, &prewarm_orphans)) prewarm_free_job(job); /* the owner gave it up */
    ui_wake_notify();
    return NULL;
}

/* One staged result into the cache, exactly as the ordinary decoder's result is committed.
 * Returns true when the row is now warm (stored, or already there); *changed is set only when the
 * cache or alias table actually changed, so an already-warm row is not repainted. */
static bool prewarm_commit(album_thumbnail_handoff_t * r, bool * changed) {
    *changed = false;
    if (r->cache_epoch != atomic_load(&album_thumbnail_cache_epoch)) {
        free(r->pixels);
        r->pixels = NULL;
        return false;
    }
    bool committed = false, alias_failed = false;
    if (r->artist_has_alias || r->artist_negative_confirmed) {
        artist_thumbnail_alias_entry_t * old = artist_thumbnail_alias_find(r->artist_key, r->artist_group_kind);
        bool alias_changed = !old || old->has_album != r->artist_has_alias ||
            (r->artist_has_alias && old->album_key != r->alias_album_key);
        if (artist_thumbnail_alias_cache_commit(r->artist_key, r->artist_group_kind, r->artist_has_alias,
                                                r->alias_album_key, r->alias_song_id)) {
            if (alias_changed) *changed = true;
            if (r->artist_has_alias && album_thumbnail_cache_find(THUMBNAIL_KEY_ARTIST, r->artist_key))
                *changed = true;
            artist_thumbnail_alias_settled(r->artist_key, r->artist_group_kind, r->artist_has_alias);
            committed = true;
        } else alias_failed = true;
    }
    if (r->pixels) {
        album_thumbnail_cache_entry_t * there = album_thumbnail_cache_find(r->key_kind, r->key);
        if (there && there->pixels) {
            /* Already in RAM (the boot warm or an earlier run): replacing it would only detach and
             * repaint the same pixels. */
            free(r->pixels);
            r->pixels = NULL;
        } else {
            if (r->key_kind == THUMBNAIL_KEY_ARTIST) artist_thumbnail_alias_cache_remove(r->artist_key, r->artist_group_kind);
            album_thumbnail_cache_commit(r->key_kind, r->key, r->song_id, r->pixels, false, 0);
            r->pixels = NULL;
            *changed = true;
        }
        /* The row's own request is now redundant: the ordinary decoder must not decode it again. */
        /* An album's pixels do not resolve the artist row when its alias could not be stored. */
        if (!alias_failed || r->key_kind == THUMBNAIL_KEY_ARTIST) {
            album_thumbnail_queue_remove_artist(r->artist_key, r->artist_group_kind);
            committed = true;
        }
        album_thumbnail_queue_remove(r->key_kind, r->key);
    }
    return committed;
}

/* UI thread: commits the staged results in one pass (one repaint of the shown rows) and reaps the
 * thread once it has ended. Each result lands as soon as it is staged (one cheap repaint per frame). Returns true while the prewarm
 * still has work or results outstanding. */
bool prewarm_poll(void) {
    prewarm_job_t * job = prewarm_job;
    if (!job) return false;
    bool running = !atomic_load_explicit(&job->done, memory_order_acquire);
    int published = atomic_load_explicit(&job->published, memory_order_acquire);
    int kind_index = job->kind_index;
    if (job->consumed < published) {
        bool shown = false;
        while (job->consumed < published) {
            album_thumbnail_handoff_t * r = &job->results[job->consumed++];
            uint64_t artist_key = r->artist_key, key = r->key;
            thumbnail_key_kind_t key_kind = r->key_kind;
            bool changed = false;
            bool resolved = prewarm_commit(r, &changed);
            /* Every requested row is recorded; the range counts as complete only if all resolved. */
            if (prewarm_keys_count[kind_index] < PREWARM_MAX) prewarm_keys[kind_index][prewarm_keys_count[kind_index]++] = artist_key;
            if (!resolved) job->unresolved++;
            if (resolved) {
                shown = shown || (changed && (thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, artist_key) ||
                        thumbnail_active_list_shows(key_kind, key)));
            }
        }
        if (shown) refresh_thumbnail_list_visible(album_thumbnail_active_list);
    }
    if (running) return true;
    pthread_join(job->thread, NULL);
    if (job->finished_clean && job->unresolved == 0 && job->consumed > 0 && job->epoch == atomic_load(&album_thumbnail_cache_epoch)) {
        prewarm_done_epoch[kind_index] = job->epoch;
        prewarm_done_first[kind_index] = job->first;
        prewarm_boot_pending[kind_index] = false;
    }
    prewarm_free_job(job);
    prewarm_job = NULL;
    atomic_store(&prewarm_active, false);
    return false;
}

/* Stops the job without waiting: the worker ends within a row, the poll reaps it. */
void library_thumbnails_prewarm_cancel(void) {
    atomic_store(&prewarm_waiting, false);
    if (prewarm_job) atomic_store(&prewarm_job->cancel, true);
}

/* UI thread, quiesce: cancel, give the job up and let the worker free it. Never waits; the shared
 * orphan wait of teardown and shutdown bounds the stalled case. */
void prewarm_discard(void) {
    atomic_store(&prewarm_waiting, false);
    prewarm_job_t * job = prewarm_job;
    if (!job) return;
    atomic_store(&job->cancel, true);
    pthread_detach(job->thread);
    if (library_async_abandon(&job->fin, &prewarm_orphans)) prewarm_free_job(job);
    prewarm_job = NULL;
    atomic_store(&prewarm_active, false);
}

void library_thumbnails_prewarm_collect(void) {
    (void) prewarm_poll();
}

/* True when a prewarm may start now: artist images on, not in the lazy policy, no cover reload. */
static bool prewarm_allowed(void) {
    return atomic_load(&artist_thumbnail_work_enabled) &&
           !thumbnail_policy(LIBRARY_THUMB_POLICY_LAZY) &&
           !atomic_load(&album_thumbnail_active) && !thumbnail_cover_thread_joinable;
}

/* True when every recorded row of the finished range is still in RAM (own pixels, or an alias
 * with its album's pixels, or a no-art mark). An empty record is never "all there". */
static bool prewarm_rows_all_there(int kind_index, metadata_db_group_kind_t kind) {
    if (prewarm_keys_count[kind_index] <= 0) return false;
    for (int i = 0; i < prewarm_keys_count[kind_index]; i++) {
        uint64_t key = prewarm_keys[kind_index][i];
        album_thumbnail_cache_entry_t * cached = album_thumbnail_cache_find(THUMBNAIL_KEY_ARTIST, key);
        if (cached && cached->pixels) continue;
        artist_thumbnail_alias_entry_t * alias = artist_thumbnail_alias_find(key, kind);
        if (alias && alias->has_album) {
            album_thumbnail_cache_entry_t * art = album_thumbnail_cache_find(THUMBNAIL_KEY_ALBUM, alias->album_key);
            if (!(art && art->pixels)) return false;
        } else if (!alias) {
            return false;
        }
    }
    return true;
}

/* Why the range needs a prewarm for this cache epoch, or NULL (logging the skip) when it is already warm. */
static const char * prewarm_range_why(int kind_index, int first, int epoch) {
    if (prewarm_done_epoch[kind_index] != epoch || prewarm_done_first[kind_index] != first)
        return "not_warmed";
    if (prewarm_boot_pending[kind_index] && atomic_load(&album_thumb_gen_screens_epoch) != epoch)
        return "boot_warm_incomplete"; /* the keys came from the boot warm, which has not completed */
    if (!prewarm_rows_all_there(kind_index, prewarm_kind(kind_index))) return "evicted"; /* some of it left RAM */
    DB_LOG("ART_ARTIST", "prewarm_skipped kind=%d first=%d keys=%d", kind_index, first,
           prewarm_keys_count[kind_index]);
    return NULL;
}

/* Starts the thread for the request (UI thread; nothing else decoding). */
static void prewarm_launch(int kind_index, int first, int rows) {
    /* Stalled jobs that were given up still hold a thread: never add to them. */
    if (library_async_kind_saturated(&prewarm_orphans)) return;
    metadata_db_group_kind_t kind = prewarm_kind(kind_index);
    int epoch = atomic_load(&album_thumbnail_cache_epoch);
    const char * why = prewarm_range_why(kind_index, first, epoch);
    if (!why) return; /* already warmed for this cache and all of it still there */
    DB_LOG("ART_ARTIST", "prewarm_launch kind=%d first=%d rows=%d why=%s", kind_index, first, rows, why);
    prewarm_job_t * job = calloc(1, sizeof(*job));
    if (!job) return;
    job->kind = kind;
    job->kind_index = kind_index;
    job->first = first;
    job->count = rows;
    job->epoch = epoch;
    prewarm_keys_count[kind_index] = 0;
    prewarm_done_epoch[kind_index] = -1;
    prewarm_boot_pending[kind_index] = false;
    if (worker_thread_create(&job->thread, ALBUM_COVER_DECODE_THREAD_STACK_SIZE, prewarm_thread_func, job) != 0) {
        free(job);
        return;
    }
    prewarm_job = job;
    atomic_store(&prewarm_active, true);
    thumbnail_poll_resume();
}

/* UI thread, poll: a request that had to wait for the warmer to yield starts once it has, if it is
 * still wanted and allowed. */
void prewarm_start_waiting(void) {
    if (!atomic_load(&prewarm_waiting)) return;
    if (lv_tick_elaps(prewarm_wait_tick) > PREWARM_WAIT_MS || !prewarm_allowed()) { atomic_store(&prewarm_waiting, false); return; }
    if (prewarm_job || atomic_load(&album_thumb_gen_active) || atomic_load(&album_thumbnail_active)) return;
    atomic_store(&prewarm_waiting, false);
    prewarm_launch(prewarm_wait_kind_index, prewarm_wait_first, prewarm_wait_rows);
}

void library_thumbnails_prewarm_artists(bool album_artists, int first_row, int rows) {
    int kind_index = album_artists ? 1 : 0;
    if (first_row < 0) first_row = 0;
    if (prewarm_job) {
        prewarm_job_t * job = prewarm_job;
        if (prewarm_poll() && job->kind_index == kind_index && job->first == first_row &&
            !atomic_load(&job->cancel)) return; /* the same request is already running */
        if (prewarm_job) { /* another list's job, or a cancelled one: let it end, then start this one */
            atomic_store(&job->cancel, true);
        }
    }
    if (!prewarm_allowed() || rows < 1) return;
    if (rows > PREWARM_MAX) rows = PREWARM_MAX;
    if (!prewarm_job && atomic_load(&album_thumb_gen_active)) {
        /* Rows already warm in RAM need no prewarm: do not make the album pass yield for nothing. */
        if (!prewarm_range_why(kind_index, first_row, atomic_load(&album_thumbnail_cache_epoch))) return;
    }
    if (prewarm_job || atomic_load(&album_thumb_gen_active)) {
        /* The warmer (which yields to visible-row work, see album_thumb_gen_cancel_cb) is running: the
         * request waits for it, and counts as visible-row work meanwhile so the warmer stops soon. */
        atomic_store(&prewarm_waiting, true);
        prewarm_wait_tick = lv_tick_get();
        prewarm_wait_kind_index = kind_index;
        prewarm_wait_first = first_row;
        prewarm_wait_rows = rows;
        thumbnail_poll_resume();
        return;
    }
    prewarm_launch(kind_index, first_row, rows);
}
