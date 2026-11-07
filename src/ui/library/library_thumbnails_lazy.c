#include "library_thumbnails_internal.h"
#include "../../core/ui/display/ui_wake.h"
#include "../../core/process/worker_thread.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../library/artwork/coordinator/artwork_failure_cache.h"
#include "../shell/gui_navigation.h"
#include "../../core/system/logging/db_log.h"
#include "../diagnostics/ui_frame_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

atomic_int album_thumbnail_queue_count;
static uint64_t album_lazy_job_started_ms;
unsigned album_lazy_queued;
unsigned album_lazy_completed;
unsigned album_lazy_with_art;
unsigned album_lazy_stale;

#define ALBUM_THUMBNAIL_QUEUE_SIZE 20

static pthread_t album_thumbnail_thread;
atomic_bool album_thumbnail_active;
bool album_thumbnail_active_key_valid;
static uint64_t album_thumbnail_active_key;
static thumbnail_key_kind_t album_thumbnail_active_key_kind;
static int album_thumbnail_active_cache_epoch;
static int album_thumbnail_active_view_generation;
static int album_thumbnail_active_logical_index;
static metadata_db_group_kind_t album_thumbnail_active_artist_group_kind;
static atomic_bool album_thumbnail_done;

/* The lazy decoder's result, written by its thread and consumed by album_thumbnail_poll_cb() once
 * album_thumbnail_done is set. `kind` is the kind of request it answers; key_kind/key name the pixels.
 * have_source_mtime/source_mtime are set alongside pixels in album_thumbnail_thread_func(), valid only
 * when pixels is NULL (a failed decode) -- lets album_thumbnail_poll_cb()
 * ask artwork_failure_cache_is_blocked() whether THIS specific failure (already
 * recorded by album_thumbnail_load_or_decode_ex() on every one of its own
 * exit paths) was temporary or permanent, instead of caching every failure as
 * a permanent "no art" the way an unconditional `known = true` used to. */
static album_thumbnail_handoff_t album_thumbnail_result;

static lv_timer_t * album_thumbnail_poll_timer;

void thumbnail_poll_resume(void) {
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
}

/* The poll timer starts paused: the first queued or staged work resumes it. */
void album_thumbnail_lazy_init(void) {
    if (album_thumbnail_poll_timer) return;
    album_thumbnail_poll_timer = lv_timer_create(album_thumbnail_poll_cb, 50, NULL);
    if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
}

/* Joins the visible-row decoder, drops its unconsumed result and parks the poll timer. */
void album_thumbnail_lazy_quiesce(void) {
    if (atomic_load(&album_thumbnail_active)) {
        pthread_join(album_thumbnail_thread, NULL);
        atomic_store(&album_thumbnail_active, false);
    }
    atomic_store(&album_thumbnail_done, false);
    free(album_thumbnail_result.pixels);
    album_thumbnail_result.pixels = NULL;
    if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
}

static album_thumbnail_request_t album_thumbnail_queue[ALBUM_THUMBNAIL_QUEUE_SIZE];

bool album_thumbnail_lazy_work_pending(void) {
    return atomic_load(&album_thumbnail_queue_count) > 0 || atomic_load(&album_thumbnail_active) ||
           atomic_load(&prewarm_active) || atomic_load(&prewarm_waiting);
}

/* Drops the queued requests `match` selects, keeping the order of the rest. */
void album_thumbnail_queue_remove_if(bool (*match)(const album_thumbnail_request_t *, const void *),
                                            const void * ctx) {
    int count = atomic_load(&album_thumbnail_queue_count), kept = 0;
    for (int i = 0; i < count; i++) {
        if (match(&album_thumbnail_queue[i], ctx)) continue;
        if (kept != i) album_thumbnail_queue[kept] = album_thumbnail_queue[i];
        kept++;
    }
    atomic_store(&album_thumbnail_queue_count, kept);
}

/* ctx of the key matchers: a request carrying only the key (and group) to remove. */
static bool queue_match_key(const album_thumbnail_request_t * req, const void * ctx) {
    const album_thumbnail_request_t * want = ctx;
    return req->key_kind == want->key_kind && req->key == want->key;
}

static bool queue_match_artist(const album_thumbnail_request_t * req, const void * ctx) {
    const album_thumbnail_request_t * want = ctx;
    return req->key_kind == THUMBNAIL_KEY_ARTIST && req->key == want->key &&
           req->artist_group_kind == want->artist_group_kind;
}

bool queue_match_list(const album_thumbnail_request_t * req, const void * ctx) {
    return req->list == ctx;
}

void album_thumbnail_queue_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    album_thumbnail_request_t want = { .key_kind = key_kind, .key = key };
    album_thumbnail_queue_remove_if(queue_match_key, &want);
}

/* Alias results are per scope: Artists and Album Artists can share a name
 * (same key) and still resolve differently. */
void album_thumbnail_queue_remove_artist(uint64_t key, metadata_db_group_kind_t group_kind) {
    album_thumbnail_request_t want = { .key = key, .artist_group_kind = group_kind };
    album_thumbnail_queue_remove_if(queue_match_artist, &want);
}

static bool album_thumbnail_cancel_cb(void * user_data) {
    int cache_epoch = (int) (intptr_t) user_data;
    return cache_epoch != atomic_load(&album_thumbnail_cache_epoch);
}

static void * album_thumbnail_thread_func(void * arg) {
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    album_thumbnail_request_t * req = (album_thumbnail_request_t *) arg;
    /* Cached once -- see library_scan_once()'s own comment on why an
     * unconditional db_log_now_ms() call here would cost a real syscall on
     * every lazy-loaded thumbnail even with logging disabled. */
    bool db_logging = db_log_enabled();
    uint64_t started_ms = db_logging ? db_log_now_ms() : 0;
    thumbnail_key_kind_t key_kind = req->key_kind;
    if (db_logging && key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "decode_begin song=%lld row=%d cache_epoch=%d rss_kb=%ld",
               (long long) req->song_id, req->logical_index, req->cache_epoch, db_log_rss_kb());
    else if (db_logging)
        DB_LOG("ART_ARTIST", "decode_begin song=%lld row=%d cache_epoch=%d key=%016llx rss_kb=%ld",
               (long long) req->song_id, req->logical_index, req->cache_epoch,
               (unsigned long long) req->key, db_log_rss_kb());
    uint16_t * pixels = NULL;
    album_thumbnail_result.have_source_mtime = false;
    album_thumbnail_result.artist_has_alias = false;
    album_thumbnail_result.artist_negative_confirmed = false;
    album_thumbnail_result.artist_key = 0;
    album_thumbnail_result.alias_album_key = 0;
    album_thumbnail_result.alias_song_id = 0;
    uint64_t pixel_key = req->key;
    thumbnail_key_kind_t pixel_key_kind = req->key_kind;
    if (req->key_kind == THUMBNAIL_KEY_ALBUM) {
        song_row_t song;
        if (metadata_db_get_song_by_id(req->song_id, &song)) {
            /* The generated thumbnail is on the card and nothing failed before: load it without
             * first working out the source's mtime (a search for cover files and two stats,
             * which only matter when something fails). */
            if (!album_thumbnail_load_sized_quick(&song, ARTWORK_PRIO_THUMBNAIL, album_thumbnail_cancel_cb,
                                                  (void *) (intptr_t) req->cache_epoch, &pixels)) {
                albumart_info_t info;
                albumart_info_from_song_row(&song, &info);
                album_thumbnail_result.source_mtime = album_source_mtime(&song, &info);
                album_thumbnail_result.have_source_mtime = true;
                album_thumbnail_load_or_decode_with_mtime(&song, ARTWORK_PRIO_THUMBNAIL,
                        album_thumbnail_cancel_cb, (void *) (intptr_t) req->cache_epoch,
                        album_thumbnail_result.source_mtime, false, NULL, false, &pixels);
            }
            if (pixels) {
                album_thumbnail_result.have_source_mtime = false;
                artwork_failure_cache_note_success(req->song_id);
            }
        }
    } else if (req->key_kind == THUMBNAIL_KEY_ARTIST_PHOTO) {
        artist_thumbnail_resolution_t resolution;
        (void) artist_thumbnail_load_photo(req->song_id, req->artist_group_kind, req->artist_name,
                    ARTWORK_PRIO_THUMBNAIL, req->key, album_thumbnail_cancel_cb,
                    (void *) (intptr_t) req->cache_epoch, &resolution);
        pixels = resolution.pixels;
        /* PHOTO uses this bit for confirmed absence, without an album failure record. */
        album_thumbnail_result.have_source_mtime = resolution.negative_confirmed;
    } else {
        artist_thumbnail_resolution_t resolution;
        (void) artist_thumbnail_load_or_decode(req->song_id, req->artist_group_kind, req->artist_name,
                    ARTWORK_PRIO_THUMBNAIL, true, req->key, album_thumbnail_cancel_cb,
                    (void *) (intptr_t) req->cache_epoch, &resolution);
        pixels = resolution.pixels;
        pixel_key = resolution.pixel_key;
        pixel_key_kind = resolution.pixel_key_kind;
        album_thumbnail_result.artist_has_alias = resolution.has_alias;
        album_thumbnail_result.artist_negative_confirmed = resolution.negative_confirmed;
        album_thumbnail_result.artist_key = req->key;
        album_thumbnail_result.artist_group_kind = req->artist_group_kind;
        album_thumbnail_result.alias_album_key = resolution.alias_album_key;
        album_thumbnail_result.alias_song_id = resolution.alias_song_id;
    }
    album_thumbnail_result.song_id = req->song_id;
    album_thumbnail_result.key = pixel_key;
    album_thumbnail_result.key_kind = pixel_key_kind;
    album_thumbnail_result.kind = req->key_kind == THUMBNAIL_KEY_ARTIST ? ALBUM_BOOT_RESULT_ARTIST : ALBUM_BOOT_RESULT_ALBUM;
    album_thumbnail_result.cache_epoch = req->cache_epoch;
    int result_logical_index = req->logical_index;
    album_thumbnail_result.pixels = (uint8_t *) pixels;
    free(req);
    album_thumbnail_done = true;
    ui_wake_notify(); /* hand the result over on the next loop pass, not the next 50 ms poll */
    if (db_logging && key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "decode_end song=%lld row=%d art=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result.song_id, result_logical_index,
               album_thumbnail_result.pixels != NULL,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
    else if (db_logging)
        DB_LOG("ART_ARTIST", "decode_end song=%lld row=%d art=%d negative=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result.song_id, result_logical_index,
               album_thumbnail_result.pixels != NULL,
               album_thumbnail_result.artist_negative_confirmed,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb song=%lld total_us=%llu pixels=%d\n",
           (long long) album_thumbnail_result.song_id,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           album_thumbnail_result.pixels != NULL);
#endif
    return NULL;
}

static void start_next_album_thumbnail(void) {
    if (thumbnail_policy(LIBRARY_THUMB_POLICY_LAZY) || !album_thumbnail_active_list ||
        !album_thumbnail_list_is_visible(album_thumbnail_active_list) ||
        album_thumbnail_scrolling || atomic_load(&album_thumbnail_active) ||
        atomic_load(&album_thumbnail_queue_count) <= 0) return;
    /* Never run two full cover decoders at once on the 56 MiB target. The
     * post-load warmer can still be finishing its current album when a row
     * queues work. Keep this poll timer alive and give the visible-row job
     * priority as soon as that worker yields instead of doubling peak decode
     * memory. */
    if (atomic_load(&album_thumb_gen_active) || atomic_load(&prewarm_active)) {
        thumbnail_poll_resume();
        return;
    }
    album_thumbnail_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    atomic_store(&album_thumbnail_active, true);
    *req = album_thumbnail_queue[0];
    memmove(&album_thumbnail_queue[0], &album_thumbnail_queue[1],
            sizeof(album_thumbnail_queue[0]) * (size_t) (--album_thumbnail_queue_count));
    album_thumbnail_active_key = req->key;
    album_thumbnail_active_key_kind = req->key_kind;
    album_thumbnail_active_artist_group_kind = req->artist_group_kind;
    album_thumbnail_active_cache_epoch = req->cache_epoch;
    album_thumbnail_active_view_generation = req->view_generation;
    album_thumbnail_active_logical_index = req->logical_index;
    album_thumbnail_active_key_valid = true;
    album_thumbnail_done = false;
    album_lazy_job_started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    bool created = worker_thread_create(&album_thumbnail_thread, ALBUM_COVER_DECODE_THREAD_STACK_SIZE,
                                        album_thumbnail_thread_func, req) == 0;
    if (!created) {
        atomic_store(&album_thumbnail_active, false);
        album_thumbnail_active_key_valid = false;
        free(req);
        return;
    }
    thumbnail_poll_resume();
}

void queue_thumbnail(lv_obj_t * list, int logical_index, thumbnail_key_kind_t key_kind,
                            uint64_t key, int64_t representative_song_id,
                            metadata_db_group_kind_t artist_group_kind, const char * artist_name) {
    if ((key_kind != THUMBNAIL_KEY_ALBUM && !atomic_load(&artist_thumbnail_work_enabled)) ||
        thumbnail_policy(LIBRARY_THUMB_POLICY_LAZY) || representative_song_id <= 0 || key == 0 || album_thumbnail_scrolling ||
        list != album_thumbnail_active_list || !album_thumbnail_list_is_visible(list) ||
        album_thumbnail_cache_find(key_kind, key)) return;
    int view_generation = atomic_load(&album_thumbnail_view_generation);
    int cache_epoch = atomic_load(&album_thumbnail_cache_epoch);
    if (atomic_load(&album_thumbnail_active) && album_thumbnail_active_key_valid &&
        album_thumbnail_active_cache_epoch == cache_epoch &&
        album_thumbnail_active_key_kind == key_kind && album_thumbnail_active_key == key &&
        (key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_active_artist_group_kind == artist_group_kind)) {
        /* A visible request for the same key can move to a newer list view
         * while this cache decode remains useful. Retarget its repaint to
         * that current request without changing codec cancellation state. */
        album_thumbnail_active_view_generation = view_generation;
        album_thumbnail_active_logical_index = logical_index;
        return;
    }
    for (int i = 0; i < album_thumbnail_queue_count; i++)
        if (album_thumbnail_queue[i].key_kind == key_kind && album_thumbnail_queue[i].key == key &&
            (key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_queue[i].artist_group_kind == artist_group_kind))
            return;
    if (album_thumbnail_queue_count >= ALBUM_THUMBNAIL_QUEUE_SIZE) return;
    album_thumbnail_queue[album_thumbnail_queue_count++] = (album_thumbnail_request_t) {
        .song_id = representative_song_id,
        .key = key,
        .key_kind = key_kind,
        .artist_group_kind = artist_group_kind,
        .view_generation = view_generation,
        .cache_epoch = cache_epoch,
        .logical_index = logical_index,
        .list = list
    };
    if (key_kind != THUMBNAIL_KEY_ALBUM)
        snprintf(album_thumbnail_queue[album_thumbnail_queue_count - 1].artist_name,
                 sizeof(album_thumbnail_queue[album_thumbnail_queue_count - 1].artist_name),
                 "%s", artist_name ? artist_name : "");
    album_lazy_queued++;
    if (key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "queued song=%lld row=%d queue_depth=%d view_generation=%d cache_epoch=%d",
               (long long) representative_song_id, logical_index,
               atomic_load(&album_thumbnail_queue_count), view_generation, cache_epoch);
    else
        DB_LOG("ART_ARTIST", "queued song=%lld row=%d key=%016llx queue_depth=%d view_generation=%d cache_epoch=%d",
               (long long) representative_song_id, logical_index, (unsigned long long) key,
               atomic_load(&album_thumbnail_queue_count), view_generation, cache_epoch);
    start_next_album_thumbnail();
}

void album_thumbnail_poll_cb(lv_timer_t * timer) {
    /* Same reasoning as compact_list_poll_fetch_cb()'s own comment
     * (compact_list.c): this timer's row-image/cache-eviction work
     * landing in the same tick as a live gesture's frame-present call is
     * exactly the kind of variable-cost interference that produces uneven
     * frame pacing mid-animation. Deferring costs nothing -- a landed
     * decode just sits ready a tick or two longer, and this timer keeps
     * rescheduling itself regardless. */
    if (gui_navigation_transition_in_progress()) return;
    if (album_thumbnail_scrolling) return;
    (void) album_thumbnail_commit_boot_preload();
    bool prewarming = prewarm_poll();
    prewarm_start_waiting();
    if (!atomic_load(&album_thumbnail_active)) {
        start_next_album_thumbnail();
        if (!atomic_load(&album_thumbnail_active) && !atomic_load(&album_thumb_gen_active) && !prewarming &&
            !atomic_load(&prewarm_active) && !atomic_load(&prewarm_waiting)) lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&album_thumbnail_done)) return;
    pthread_join(album_thumbnail_thread, NULL);
    /* Keep the slot busy until all result fields are consumed. Row refreshes
     * below can synchronously queue work and try to start the next worker. */

    bool cache_epoch_matches = album_thumbnail_result.cache_epoch ==
                               atomic_load(&album_thumbnail_cache_epoch);
    bool result_committed = false;
    bool result_had_art = album_thumbnail_result.pixels != NULL;
    bool alias_dropped = false; /* the artist row's alias could not be stored */
    if (cache_epoch_matches) {
        if (album_thumbnail_result.kind == ALBUM_BOOT_RESULT_ARTIST) {
            if ((album_thumbnail_result.artist_has_alias || album_thumbnail_result.artist_negative_confirmed) &&
                artist_thumbnail_alias_cache_commit(album_thumbnail_result.artist_key,
                    album_thumbnail_result.artist_group_kind,
                    album_thumbnail_result.artist_has_alias, album_thumbnail_result.alias_album_key,
                    album_thumbnail_result.alias_song_id)) {
                artist_thumbnail_alias_settled(album_thumbnail_result.artist_key,
                    album_thumbnail_result.artist_group_kind, album_thumbnail_result.artist_has_alias);
                result_committed = true;
            } else if (album_thumbnail_result.artist_has_alias || album_thumbnail_result.artist_negative_confirmed) {
                /* Nothing stored and the request is already dequeued: repainting the artist row
                 * would only queue and decode it again, so leave it for the next refresh. */
                alias_dropped = true;
            }
            if (album_thumbnail_result.pixels) {
                if (album_thumbnail_result.key_kind == THUMBNAIL_KEY_ARTIST)
                    artist_thumbnail_alias_cache_remove(album_thumbnail_result.artist_key,
                                                        album_thumbnail_result.artist_group_kind);
                album_thumbnail_cache_commit(album_thumbnail_result.key_kind, album_thumbnail_result.key,
                    album_thumbnail_result.song_id, album_thumbnail_result.pixels, false, 0);
                album_thumbnail_result.pixels = NULL;
                result_committed = true;
            }
        } else {
            album_thumbnail_cache_commit(album_thumbnail_result.key_kind, album_thumbnail_result.key,
                album_thumbnail_result.song_id, album_thumbnail_result.pixels,
                album_thumbnail_result.have_source_mtime, album_thumbnail_result.source_mtime);
            album_thumbnail_result.pixels = NULL;
            result_committed = true;
        }
    }
    bool same_view = album_thumbnail_active_view_generation == atomic_load(&album_thumbnail_view_generation);
    bool repaint_result = cache_epoch_matches &&
        ((!alias_dropped && same_view && album_thumbnail_active_list &&
          album_thumbnail_list_is_visible(album_thumbnail_active_list)) ||
         (!alias_dropped && album_thumbnail_result.kind == ALBUM_BOOT_RESULT_ARTIST &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_thumbnail_result.artist_key)) ||
         /* An artist result may carry an album's pixels too (key_kind ALBUM). */
         (result_had_art &&
          thumbnail_active_list_shows(album_thumbnail_result.key_kind, album_thumbnail_result.key)) ||
         (album_thumbnail_result.kind == ALBUM_BOOT_RESULT_ALBUM &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ALBUM, album_thumbnail_result.key)));
    album_lazy_completed++;
    if (result_had_art) album_lazy_with_art++;
    if (!cache_epoch_matches) album_lazy_stale++;
    if (album_thumbnail_result.kind == ALBUM_BOOT_RESULT_ALBUM)
        DB_LOG("ART_LAZY", "result song=%lld row=%d committed=%d repaint=%d art=%d queue_depth=%d ui_wait_ms=%llu",
               (long long) album_thumbnail_result.song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, atomic_load(&album_thumbnail_queue_count),
               album_lazy_job_started_ms ? (unsigned long long) (db_log_now_ms() - album_lazy_job_started_ms) : 0ULL);
    else
        DB_LOG("ART_ARTIST", "result song=%lld row=%d committed=%d repaint=%d art=%d alias=%d album=%016llx queue_depth=%d",
               (long long) album_thumbnail_result.song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, album_thumbnail_result.artist_has_alias,
               (unsigned long long) album_thumbnail_result.alias_album_key,
               atomic_load(&album_thumbnail_queue_count));
    free(album_thumbnail_result.pixels);
    album_thumbnail_result.pixels = NULL;
    if (repaint_result) {
        /* Only the rows bound to the landed artwork, and the rows whose picture
         * the cache evicted meanwhile (compact_list_detach_image_src() marks
         * them): committing artwork can evict a descriptor borrowed by another
         * row, without changing its text. */
        refresh_thumbnail_list_landed(album_thumbnail_active_list,
                                      album_thumbnail_result.key, album_thumbnail_result.artist_key,
                                      album_thumbnail_result.kind == ALBUM_BOOT_RESULT_ARTIST && !alias_dropped);
    }
    atomic_store(&album_thumbnail_active, false);
    album_thumbnail_active_key_valid = false;
    start_next_album_thumbnail();
    if (!atomic_load(&album_thumbnail_active)) {
        /* Nothing else is coming: one full (idempotent, so cheap) pass lets any
         * visible row that was not queued yet queue itself, as every landing's
         * full refresh used to. */
        if (repaint_result && !alias_dropped) refresh_thumbnail_list_visible(album_thumbnail_active_list);
        if (!atomic_load(&album_thumbnail_active)) lv_timer_pause(timer);
    }
}

bool library_thumbnails_busy(void) {
    return atomic_load(&album_thumbnail_active) || atomic_load(&album_thumbnail_queue_count) > 0 ||
           atomic_load(&prewarm_active);
}

void library_thumbnails_dispatch_ready(void) {
    /* A decode that is still running has nothing for the poll to do yet. */
    if (album_thumbnail_poll_timer && !lv_timer_get_paused(album_thumbnail_poll_timer) &&
        (!atomic_load(&album_thumbnail_active) || atomic_load(&album_thumbnail_done) ||
         atomic_load(&album_boot_preload_state) == ALBUM_BOOT_PRELOAD_PENDING ||
         (prewarm_job && (atomic_load(&prewarm_job->done) ||
                          atomic_load(&prewarm_job->published) > prewarm_job->consumed))))
        lv_timer_ready(album_thumbnail_poll_timer);
}

void library_thumbnails_request_album(lv_obj_t * list, int64_t song_id, uint64_t artwork_key) {
    queue_thumbnail(list, 0, THUMBNAIL_KEY_ALBUM, artwork_key, song_id, METADATA_DB_GROUP_ARTIST, NULL);
}

void library_thumbnails_request_artist_photo(lv_obj_t * list, int64_t song_id, uint64_t key,
                                             metadata_db_group_kind_t kind, const char * name) {
    queue_thumbnail(list, 0, THUMBNAIL_KEY_ARTIST_PHOTO, key, song_id, kind, name);
}
