#include "library_thumbnails.h"
#include "../widgets/lists/compact_list.h"
#include "../widgets/lists/lv_idempotent.h"
#include "../../core/ui/display/ui_wake.h"
#include "../../core/process/worker_thread.h"
#include "library_thumbnail_placeholder.h"
#include "browse/gui_library_async.h"
#include "../../library/browser/paths/library_paths.h"
#include "../../library/metadata/scan/library_scan_storage.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/decode/cover_decode.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../library/artwork/coordinator/artwork_failure_cache.h"
#include "../../library/artwork/support/library_player_art.h"
#include "../player/screen/gui_player.h"
#include "../shell/gui_navigation.h"
#include "../../library/tagcache/core/tagcache.h"
#include "../../core/system/logging/db_log.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../../audio/engine/audio.h"
#include "../diagnostics/ui_frame_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>
#ifdef __linux__
#include <linux/sched.h>
#endif
#include <time.h>
#include <limits.h>

#define ALBUM_THUMB_GEN_BATCH 16
#define ALBUM_THUMB_GEN_INTER_ALBUM_US 100000
#define ALBUM_COVER_DECODE_THREAD_STACK_SIZE (4 * 1024 * 1024)

extern bool gui_navigation_transition_in_progress(void);
extern uint64_t db_log_now_ms(void);
extern long db_log_rss_kb(void);
extern void install_thread_crash_altstack(void);

static library_thumbnail_policy_cb_t thumb_policy_cb;
static library_thumbnail_genre_context_cb_t thumb_genre_context_cb;
static library_thumbnail_detach_group_cb_t thumb_detach_group_cb;
static library_thumbnail_refresh_group_cb_t thumb_refresh_group_cb;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    library_thumbnail_list_role_t role;
} thumbnail_binding_t;
#define THUMBNAIL_BINDING_CAPACITY 16
static thumbnail_binding_t thumbnail_bindings[THUMBNAIL_BINDING_CAPACITY];
static lv_obj_t *album_thumbnail_active_list;
static atomic_int album_thumbnail_view_generation;
static atomic_int album_thumbnail_cache_epoch;
static atomic_int album_thumbnail_queue_count;
static atomic_bool artist_thumbnail_work_enabled = true;
static uint64_t albums_page_open_requested_ms;
static uint64_t album_lazy_job_started_ms;
static unsigned album_lazy_queued;
static unsigned album_lazy_completed;
static unsigned album_lazy_with_art;
static unsigned album_lazy_stale;
static const library_thumbnail_context_t album_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ALBUM, .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const library_thumbnail_context_t artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST, .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const library_thumbnail_context_t album_artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST, .artist_group_kind = METADATA_DB_GROUP_ALBUM_ARTIST
};
static void thumbnail_row_decorator(lv_obj_t *, lv_obj_t *, lv_obj_t *, int, int, int64_t, uint64_t, const char *, void *);
static void refresh_all_thumbnail_lists(void);
static bool album_thumbnail_list_is_visible(lv_obj_t *list);
static thumbnail_binding_t *thumbnail_binding_for_list(lv_obj_t *list);
static void album_thumbnail_cache_epoch_advance(void) {
    atomic_fetch_add(&album_thumbnail_cache_epoch, 1);
}
static bool release_thumbnail_dsc(const lv_image_dsc_t *dsc);
static void refresh_thumbnail_list_visible(lv_obj_t *list);
static void thumbnail_decorate_image(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                     int logical_index, int pool_slot, int64_t representative_song_id,
                                     uint64_t artwork_key, const char * artwork_name, void * ctx,
                                     bool touch_row);
static void refresh_thumbnail_list_landed(lv_obj_t *list, thumbnail_key_kind_t key_kind, uint64_t key,
                                          uint64_t artist_key, thumbnail_key_kind_t request_kind);
static void thumbnail_begin_screen(lv_obj_t *list);
static void thumbnail_end_screen(lv_obj_t *list);


/* ---- Virtualized local-album thumbnails -------------------------------
 * Only the 20 recycled compact-list rows can request artwork. One worker at
 * a time reads/decodes a representative song's embedded or Rockbox albumart
 * file, while a 250-entry RGB565 LRU cache keeps the visible window plus
 * scroll headroom bounded at ~3125 KiB. Persistent sized files live in
 * MUSIC_ROOT_DIR/.compas/albumart/<artist>-<album>.<N>x<N>.bmp. */
#define ALBUM_THUMBNAIL_CACHE_SIZE 250

typedef struct {
    uint64_t key;
    thumbnail_key_kind_t key_kind;
    bool known; /* true even when pixels==NULL: negative cache for albums without art */
    bool pinned; /* a first-screen warm entry: skipped by the LRU victim loop (see first_screen_pin_epoch) */
    uint8_t * pixels;
    lv_image_dsc_t dsc;
    uint32_t last_use;
} album_thumbnail_cache_entry_t;

typedef struct {
    uint64_t artist_key;
    metadata_db_group_kind_t artist_group_kind;
    uint64_t album_key;
    thumbnail_key_kind_t album_key_kind;
    int64_t representative_song_id;
    bool known;
    bool has_album;
    uint32_t last_use;
} artist_thumbnail_alias_entry_t;

typedef struct {
    int64_t song_id;
    uint64_t key;
    thumbnail_key_kind_t key_kind;
    metadata_db_group_kind_t artist_group_kind;
    char artist_name[128];
    int view_generation;
    int cache_epoch;
    int logical_index;
    lv_obj_t * list;
} album_thumbnail_request_t;

#define ALBUM_THUMBNAIL_QUEUE_SIZE 20

static album_thumbnail_cache_entry_t album_thumbnail_cache[ALBUM_THUMBNAIL_CACHE_SIZE];
/* Aliases are small lookup records, not decoded images, so they have their
 * own dynamically sized table (declared above) and do not consume RGB565
 * cache entries. */
static uint32_t album_thumbnail_use_counter;
static pthread_t album_thumbnail_thread;
static atomic_bool album_thumbnail_active;
static bool album_thumbnail_active_key_valid;
static uint64_t album_thumbnail_active_key;
static thumbnail_key_kind_t album_thumbnail_active_key_kind;
static int album_thumbnail_active_cache_epoch;
static int album_thumbnail_active_view_generation;
static int album_thumbnail_active_logical_index;
static metadata_db_group_kind_t album_thumbnail_active_artist_group_kind;
static atomic_bool album_thumbnail_done;
static int64_t album_thumbnail_result_song_id;
static uint64_t album_thumbnail_result_key;
static thumbnail_key_kind_t album_thumbnail_result_key_kind;
static thumbnail_key_kind_t album_thumbnail_result_request_key_kind;
static int album_thumbnail_result_cache_epoch;
static uint8_t * album_thumbnail_result_pixels;
/* Set alongside result_pixels in album_thumbnail_thread_func(), valid only
 * when result_pixels is NULL (a failed decode) -- lets album_thumbnail_poll_cb()
 * ask artwork_failure_cache_is_blocked() whether THIS specific failure (already
 * recorded by album_thumbnail_load_or_decode_ex() on every one of its own
 * exit paths) was temporary or permanent, instead of caching every failure as
 * a permanent "no art" the way an unconditional `known = true` used to. */
static time_t album_thumbnail_result_source_mtime;
static bool album_thumbnail_result_have_source_mtime;
static bool album_thumbnail_result_negative_confirmed;
static bool album_thumbnail_result_has_alias;
static bool album_thumbnail_result_artist_negative_confirmed;
static uint64_t album_thumbnail_result_artist_key;
static metadata_db_group_kind_t album_thumbnail_result_artist_group_kind;
static uint64_t album_thumbnail_result_alias_album_key;
static int64_t album_thumbnail_result_alias_song_id;
static lv_timer_t * album_thumbnail_poll_timer;
static album_thumbnail_request_t album_thumbnail_queue[ALBUM_THUMBNAIL_QUEUE_SIZE];
static bool album_thumbnail_scrolling;

/* Visible-first prewarm (see library_thumbnails_prewarm_artists): one thread decodes the first screen
 * of an artist list from the Music-menu press on, staging results for the UI thread to commit. While
 * it runs the ordinary one-at-a-time decoder waits, so two decoders never run together.
 * The job is a heap record shared by the UI thread and the worker like the library's other abandonable
 * reads (gui_library_async.h): teardown cancels it, detaches the thread and lets the worker free it, so a
 * stalled card read can never block the UI thread; the shared orphan wait bounds the rest. */
#define PREWARM_MAX 12
typedef struct prewarm_job prewarm_job_t;
static prewarm_job_t * prewarm_job;         /* UI thread only: the job not yet reaped or abandoned */
static atomic_bool prewarm_active;          /* a job exists (set at launch, cleared when it is reaped or abandoned) */
static atomic_int prewarm_orphans;          /* given-up jobs whose worker still runs */
static int prewarm_done_epoch[2] = { -1, -1 };
static int prewarm_done_first[2];
static int prewarm_keys_count[2];
static bool prewarm_boot_pending[2];          /* keys came from the boot first-screen warm; valid once it completed */
static uint64_t prewarm_keys[2][PREWARM_MAX]; /* artist keys committed by the last finished run */
#define PREWARM_WAIT_MS 3000
static bool prewarm_waiting;                  /* a request is waiting for the warmer to yield */
static uint32_t prewarm_wait_tick;
static int prewarm_wait_kind_index, prewarm_wait_first, prewarm_wait_rows;
    /* Boot-time RAM cache preload: piggybacks on the existing persistent
     * warmer thread (album_thumb_gen_thread_func) to also hand its first
     * ALBUM_THUMBNAIL_CACHE_SIZE decoded thumbnails to the main/LVGL thread
     * for insertion into album_thumbnail_cache[], instead of only writing the
     * on-disk sized BMP sidecar. Single-slot rendezvous (like the on-screen
     * lazy-decode hand-off above) -- the warmer thread blocks after staging
     * one result until album_thumbnail_poll_cb() has consumed it, so at most
     * one pending buffer ever exists and the cache array is still only ever
     * written from the main thread. */
    static int album_thumb_gen_ram_filled;
    static uint64_t album_thumb_gen_cached_keys[ALBUM_THUMBNAIL_CACHE_SIZE];
    static thumbnail_key_kind_t album_thumb_gen_cached_key_kinds[ALBUM_THUMBNAIL_CACHE_SIZE];
    static int album_thumb_gen_cached_key_count;
    /* Aliases already resolved in RAM when this warmer run started (the
     * alias table itself is UI-thread only): a restarted run skips them. */
    typedef struct { uint64_t key; metadata_db_group_kind_t kind; bool has_album; } album_thumb_gen_alias_key_t;
    static album_thumb_gen_alias_key_t * album_thumb_gen_known_aliases;
    static size_t album_thumb_gen_known_alias_count;
    /* EMPTY: slot free, warmer may stage the next result into it.
     * PENDING: warmer has staged a result and is waiting.
     * CLAIMED: album_thumbnail_poll_cb() has taken ownership and is still
     * reading/committing it -- the warmer must not touch the staged fields
     * again until this goes back to EMPTY. A plain 2-state pending/not-
     * pending flag let the warmer's own wait-loop-exit race the callback:
     * clearing "pending" and THEN reading the payload left a window where
     * the warmer saw the slot as free and overwrote the fields the callback
     * was still using. The CAS transitions below close that window. */
    enum { ALBUM_BOOT_PRELOAD_EMPTY = 0, ALBUM_BOOT_PRELOAD_PENDING, ALBUM_BOOT_PRELOAD_CLAIMED };
    static atomic_int album_boot_preload_state;
    static uint64_t album_boot_preload_result_key;
    static thumbnail_key_kind_t album_boot_preload_result_key_kind;
    static int64_t album_boot_preload_result_song_id;
    static uint8_t * album_boot_preload_result_pixels;
    static bool album_boot_preload_result_have_source_mtime;
    static time_t album_boot_preload_result_source_mtime;
    enum { ALBUM_BOOT_RESULT_ALBUM, ALBUM_BOOT_RESULT_ARTIST };
    static int album_boot_preload_result_kind;
    static int album_boot_preload_result_cache_epoch;
    static uint64_t album_boot_preload_result_artist_key;
    static metadata_db_group_kind_t album_boot_preload_result_artist_group_kind;
    static bool album_boot_preload_result_artist_has_alias;
    static bool album_boot_preload_result_artist_negative_confirmed;
    static uint64_t album_boot_preload_result_alias_album_key;
    static int64_t album_boot_preload_result_alias_song_id;
    static bool album_boot_preload_result_first_screen;
static bool album_thumb_gen_user_visible;
static bool album_thumb_gen_force_reload;
static bool album_thumb_gen_single_album;
static bool album_thumb_gen_altstack_already_installed;
static int64_t album_thumb_gen_target_song_id;
static atomic_int album_thumb_gen_error_count;
static pthread_t thumbnail_cover_thread;
static atomic_bool thumbnail_cover_done;
static bool thumbnail_cover_thread_joinable;



static artist_thumbnail_alias_entry_t * artist_thumbnail_alias_cache;
static size_t artist_thumbnail_alias_cache_capacity;

/* True when the visible thumbnail list shows a row that a just-committed
 * entry can change: a row keyed by it, or an artist row whose alias borrows
 * that album. Independent of view generations, so a result staged before a
 * scroll still repaints the rows that are on screen now. */
static bool thumbnail_active_list_shows(thumbnail_key_kind_t key_kind, uint64_t key) {
    lv_obj_t * list = album_thumbnail_active_list;
    if (!list || !album_thumbnail_list_is_visible(list)) return false;
    if (thumbnail_binding_for_list(list) && thumbnail_binding_for_list(list)->role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM) return true;
    if (compact_list_shows_artwork_key(list, key)) return true;
    if (key_kind != THUMBNAIL_KEY_ALBUM) return false;
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        const artist_thumbnail_alias_entry_t * e = &artist_thumbnail_alias_cache[i];
        if (e->known && e->has_album && e->album_key == key &&
            compact_list_shows_artwork_key(list, e->artist_key))
            return true;
    }
    return false;
}

static bool album_thumbnail_lazy_work_pending(void) {
    return atomic_load(&album_thumbnail_queue_count) > 0 || atomic_load(&album_thumbnail_active) ||
           atomic_load(&prewarm_active) || prewarm_waiting;
}

static album_thumbnail_cache_entry_t * album_thumbnail_cache_find(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache[i].key == key) {
            album_thumbnail_cache[i].last_use = ++album_thumbnail_use_counter;
            return &album_thumbnail_cache[i];
        }
    }
    return NULL;
}

static bool artist_thumbnail_alias_cache_resize(int artist_count, int album_artist_count) {
    size_t wanted = (size_t) (artist_count > 0 ? artist_count : 0) +
                    (size_t) (album_artist_count > 0 ? album_artist_count : 0);
    if (wanted == artist_thumbnail_alias_cache_capacity) return true;
    if (wanted == 0) {
        free(artist_thumbnail_alias_cache);
        artist_thumbnail_alias_cache = NULL;
        artist_thumbnail_alias_cache_capacity = 0;
        return true;
    }
    artist_thumbnail_alias_entry_t * grown = realloc(artist_thumbnail_alias_cache,
                                                       wanted * sizeof(*grown));
    if (!grown) return false;
    if (wanted > artist_thumbnail_alias_cache_capacity)
        memset(grown + artist_thumbnail_alias_cache_capacity, 0,
               (wanted - artist_thumbnail_alias_cache_capacity) * sizeof(*grown));
    artist_thumbnail_alias_cache = grown;
    artist_thumbnail_alias_cache_capacity = wanted;
    return true;
}

static bool artist_thumbnail_alias_cache_resize_for_library(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    return artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
}

static artist_thumbnail_alias_entry_t * artist_thumbnail_alias_find(
        uint64_t artist_key, metadata_db_group_kind_t artist_group_kind) {
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        if (artist_thumbnail_alias_cache[i].known &&
            artist_thumbnail_alias_cache[i].artist_key == artist_key &&
            artist_thumbnail_alias_cache[i].artist_group_kind == artist_group_kind) {
            artist_thumbnail_alias_cache[i].last_use = ++album_thumbnail_use_counter;
            return &artist_thumbnail_alias_cache[i];
        }
    }
    return NULL;
}

static void artist_thumbnail_alias_cache_commit(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind,
                                                 bool has_album,
                                                 uint64_t album_key, int64_t representative_song_id) {
    if (!artist_thumbnail_alias_cache_capacity &&
        !artist_thumbnail_alias_cache_resize_for_library()) return;
    if (!artist_thumbnail_alias_cache_capacity) return;
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        if (artist_thumbnail_alias_cache[i].known &&
            artist_thumbnail_alias_cache[i].artist_key == artist_key &&
            artist_thumbnail_alias_cache[i].artist_group_kind == artist_group_kind) {
            victim = i;
            break;
        }
    }
    if (victim == SIZE_MAX) {
        uint32_t oldest = UINT32_MAX;
        for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
            if (!artist_thumbnail_alias_cache[i].known) {
                victim = i;
                break;
            }
            if (artist_thumbnail_alias_cache[i].last_use < oldest) {
                oldest = artist_thumbnail_alias_cache[i].last_use;
                victim = i;
            }
        }
    }
    if (victim == SIZE_MAX) return;
    artist_thumbnail_alias_cache[victim] = (artist_thumbnail_alias_entry_t) {
        .artist_key = artist_key,
        .artist_group_kind = artist_group_kind,
        .album_key = album_key,
        .album_key_kind = THUMBNAIL_KEY_ALBUM,
        .representative_song_id = representative_song_id,
        .known = true,
        .has_album = has_album,
        .last_use = ++album_thumbnail_use_counter
    };
}

static void artist_thumbnail_alias_cache_clear(void) {
    free(artist_thumbnail_alias_cache);
    artist_thumbnail_alias_cache = NULL;
    artist_thumbnail_alias_cache_capacity = 0;
}

static void artist_thumbnail_alias_cache_remove(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind) {
    artist_thumbnail_alias_entry_t * entry = artist_thumbnail_alias_find(artist_key, artist_group_kind);
    if (entry) memset(entry, 0, sizeof(*entry));
}

static void album_thumbnail_queue_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < album_thumbnail_queue_count; ) {
        if (album_thumbnail_queue[i].key_kind != key_kind || album_thumbnail_queue[i].key != key) {
            i++;
            continue;
        }
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t) (album_thumbnail_queue_count - i - 1));
        album_thumbnail_queue_count--;
    }
}

/* Alias results are per scope: Artists and Album Artists can share a name
 * (same key) and still resolve differently. */
static void album_thumbnail_queue_remove_artist(uint64_t key, metadata_db_group_kind_t group_kind) {
    for (int i = 0; i < album_thumbnail_queue_count; ) {
        if (album_thumbnail_queue[i].key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_queue[i].key != key ||
            album_thumbnail_queue[i].artist_group_kind != group_kind) {
            i++;
            continue;
        }
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t) (album_thumbnail_queue_count - i - 1));
        album_thumbnail_queue_count--;
    }
}

/* Returns true when a row of the visible list lost this entry's image; the
 * caller re-decorates only after the slot is fully cleared or replaced, so
 * the decorator never sees a half-evicted "known, no art" entry. */
static bool album_thumbnail_cache_evict_entry(album_thumbnail_cache_entry_t * e) {
    uint8_t * retired_pixels = e->pixels;
    bool visible_lost = false;
    if (retired_pixels) {
        e->pixels = NULL;
        e->dsc.data = NULL;
        visible_lost = release_thumbnail_dsc(&e->dsc); /* after clearing, so a re-decorate cannot re-attach it */
        free(retired_pixels);
    }
    memset(e, 0, sizeof(*e));
    return visible_lost;
}

static void album_thumbnail_cache_evict(album_thumbnail_cache_entry_t * e) {
    if (album_thumbnail_cache_evict_entry(e)) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static void album_thumbnail_cache_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[i];
        if (!e->known || e->key_kind != key_kind || e->key != key) continue;
        album_thumbnail_cache_evict(e);
        return;
    }
}

static void album_thumbnail_cache_remove_kind(thumbnail_key_kind_t key_kind) {
    bool visible_lost = false;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++)
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache_evict_entry(&album_thumbnail_cache[i]))
            visible_lost = true;
    if (visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static void album_thumbnail_cache_clear(void) {
    album_thumbnail_cache_epoch_advance();
    uint8_t * retired_pixels[ALBUM_THUMBNAIL_CACHE_SIZE];
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        retired_pixels[i] = album_thumbnail_cache[i].pixels;
        album_thumbnail_cache[i].pixels = NULL;
        album_thumbnail_cache[i].dsc.data = NULL;
    }
    refresh_all_thumbnail_lists();
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        free(retired_pixels[i]);
        memset(&album_thumbnail_cache[i], 0, sizeof(album_thumbnail_cache[i]));
    }
    artist_thumbnail_alias_cache_clear();
    album_thumbnail_use_counter = 0;
    artwork_failure_cache_clear();
}

    /* Evicts the LRU slot (or the first unused one) and installs `pixels` (or
     * a durable "known, no art" marker for a PERMANENT failure) into it.
     * Shared by the on-screen lazy-decode commit and the boot-time RAM
     * preload hand-off further below -- both only ever run on the main/LVGL
     * thread, the only thread allowed to touch album_thumbnail_cache[].
     * Takes ownership of `pixels`: either stores it or frees it, never both. */
static uint32_t album_thumbnail_commit_counter;
/* Pins are valid only for the cache epoch they were set in (a new epoch also memsets every entry). */
static int first_screen_pin_epoch = -1;

static void album_thumbnail_pin(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[i];
        if (e->known && e->pixels && e->key_kind == key_kind && e->key == key) { e->pinned = true; return; }
    }
}

static void album_thumbnail_unpin_all(void) {
    first_screen_pin_epoch = -1;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) album_thumbnail_cache[i].pinned = false;
}

uint32_t library_thumbnails_commit_count(void) { return album_thumbnail_commit_counter; }

static void album_thumbnail_cache_commit(thumbnail_key_kind_t key_kind, uint64_t key,
                                         int64_t source_song_id, uint8_t * pixels,
                                         bool have_source_mtime, time_t source_mtime,
                                         bool negative_confirmed) {
    album_thumbnail_commit_counter++;
        /* A failed decode (pixels == NULL) is only worth caching as a durable
         * "known, no art" entry if album_thumbnail_load_or_decode_ex() itself
         * already recorded it as PERMANENT (corrupt/oversized/genuinely
         * absent source). A TEMPORARY failure must NOT be cached here the
         * same way -- album_thumbnail_cache_find()'s only gate is `known`, so
         * caching it at all would make a transient hiccup permanent for this
         * song for the rest of this cache slot's lifetime. Leaving the slot
         * at its just-memset "unused" state (known=false) instead lets a
         * future request legitimately retry this song. */
        bool commit_known = pixels != NULL || negative_confirmed;
        bool victim_visible_lost = false;
        if (!commit_known && key_kind == THUMBNAIL_KEY_ALBUM && have_source_mtime) {
            artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(source_song_id, source_mtime, &fail_reason) &&
                fail_reason == ARTWORK_FAIL_PERMANENT) {
                commit_known = true;
            }
        }
        int victim = -1;
        /* An interrupted-then-restarted boot preload pass can reach the same
         * song_id twice (it always restarts at offset 0). Without this check
         * that would waste a second slot on a duplicate instead of just
         * refreshing the one that's already there. The on-screen lazy-decode
         * path never hits this: queue_album_thumbnail() already skips
         * queuing a song album_thumbnail_cache_find() reports as known. A
         * full pass for the match has to run before the unused/LRU pass
         * below, which stops at the first unused slot -- a duplicate further
         * down the array would otherwise never be seen. */
        for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
            if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
                album_thumbnail_cache[i].key == key) {
                victim = i;
                break;
            }
        }
        /* A TEMPORARY failure must not blank a previously valid entry for
         * this song. pixels is NULL whenever commit_known is false. */
        if (victim >= 0 && !commit_known) {
            free(pixels);
            return;
        }
        if (victim < 0) {
            /* First-screen entries are pinned (at most 24) and skipped unless nothing else is left. */
            int pin_epoch_ok = first_screen_pin_epoch == atomic_load(&album_thumbnail_cache_epoch);
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
                if (!album_thumbnail_cache[i].known) { victim = i; break; }
                if (pin_epoch_ok && album_thumbnail_cache[i].pinned) continue;
                if (album_thumbnail_cache[i].last_use < oldest) {
                    oldest = album_thumbnail_cache[i].last_use;
                    victim = i;
                }
            }
            if (victim < 0) {
                for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
                    if (album_thumbnail_cache[i].last_use < oldest) {
                        oldest = album_thumbnail_cache[i].last_use;
                        victim = i;
                    }
                }
            }
        }
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[victim];
        bool was_pinned = e->pinned && e->known && e->key_kind == key_kind && e->key == key;
        uint8_t * retired_pixels = e->pixels;
        if (retired_pixels) {
            /* A leading lv_image can retain &e->dsc after its row last ran
             * the decorator. Freeing pixels first made that image descriptor
             * point into released heap memory until the row happened to be
             * recycled: a redraw/scroll use-after-free on libraries larger
             * than the LRU. Mark this as a known no-art entry temporarily and
             * repaint every visible row so all references are detached before
             * releasing/reusing the slot. */
            e->pixels = NULL;
            e->dsc.data = NULL;
            victim_visible_lost = release_thumbnail_dsc(&e->dsc); /* after clearing, so a re-decorate cannot re-attach it */
            free(retired_pixels);
        }
        memset(e, 0, sizeof(*e));
        if (commit_known) {
            e->key = key;
            e->key_kind = key_kind;
            e->known = true;
            e->pinned = was_pinned; /* a same-key re-commit keeps its pin */
            e->pixels = pixels;
            e->last_use = ++album_thumbnail_use_counter;
            if (e->pixels) {
                e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                e->dsc.header.w = ALBUM_THUMBNAIL_PX;
                e->dsc.header.h = ALBUM_THUMBNAIL_PX;
                e->dsc.header.stride = ALBUM_THUMBNAIL_PX * 2;
                e->dsc.data = e->pixels;
                e->dsc.data_size = ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2;
            }
        } else {
            free(pixels);
        }
        album_thumbnail_queue_remove(key_kind, key);
        /* Only now is the victim slot fully replaced. */
        if (victim_visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static bool album_thumbnail_commit_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return false;
    bool committed = false, changed = false; /* changed: the cache holds something new to repaint */
    if (album_boot_preload_result_cache_epoch == atomic_load(&album_thumbnail_cache_epoch)) {
        if (album_boot_preload_result_kind == ALBUM_BOOT_RESULT_ALBUM) {
            album_thumbnail_cache_commit(album_boot_preload_result_key_kind,
                album_boot_preload_result_key, album_boot_preload_result_song_id,
                album_boot_preload_result_pixels, album_boot_preload_result_have_source_mtime,
                album_boot_preload_result_source_mtime, false);
            committed = changed = true;
        } else {
            if (album_boot_preload_result_artist_has_alias ||
                album_boot_preload_result_artist_negative_confirmed) {
                artist_thumbnail_alias_cache_commit(album_boot_preload_result_artist_key,
                    album_boot_preload_result_artist_group_kind,
                    album_boot_preload_result_artist_has_alias,
                    album_boot_preload_result_alias_album_key,
                    album_boot_preload_result_alias_song_id);
                album_thumbnail_queue_remove_artist(album_boot_preload_result_artist_key,
                                                    album_boot_preload_result_artist_group_kind);
                if (album_boot_preload_result_artist_has_alias)
                    album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST,
                                                  album_boot_preload_result_artist_key);
                committed = changed = true;
            }
            if (album_boot_preload_result_pixels && album_boot_preload_result_first_screen) {
                album_thumbnail_cache_entry_t * there = album_thumbnail_cache_find(
                    album_boot_preload_result_key_kind, album_boot_preload_result_key);
                if (there && there->pixels) {
                    /* A warm rerun reached a row that is already in RAM: replacing it would only
                     * detach and repaint the same pixels. */
                    free(album_boot_preload_result_pixels);
                    album_boot_preload_result_pixels = NULL;
                    committed = true;
                }
            }
            if (album_boot_preload_result_pixels) {
                album_thumbnail_cache_commit(album_boot_preload_result_key_kind,
                    album_boot_preload_result_key, album_boot_preload_result_song_id,
                    album_boot_preload_result_pixels, false, 0, false);
                committed = changed = true;
            }
        }
        album_boot_preload_result_pixels = NULL;
        if (album_boot_preload_result_first_screen && committed &&
            album_boot_preload_result_kind == ALBUM_BOOT_RESULT_ARTIST) {
            /* Record the row where the press prewarm looks, and pin what holds its pixels. */
            int epoch = album_boot_preload_result_cache_epoch;
            int k = album_boot_preload_result_artist_group_kind == METADATA_DB_GROUP_ALBUM_ARTIST ? 1 : 0;
            if (prewarm_done_epoch[k] != epoch || prewarm_done_first[k] != 0 || !prewarm_boot_pending[k]) {
                prewarm_done_epoch[k] = epoch;
                prewarm_done_first[k] = 0;
                prewarm_keys_count[k] = 0;
                prewarm_boot_pending[k] = true;
            }
            bool recorded = false;
            for (int j = 0; j < prewarm_keys_count[k]; j++)
                if (prewarm_keys[k][j] == album_boot_preload_result_artist_key) { recorded = true; break; }
            if (!recorded && prewarm_keys_count[k] < PREWARM_MAX)
                prewarm_keys[k][prewarm_keys_count[k]++] = album_boot_preload_result_artist_key;
            if (first_screen_pin_epoch != epoch) album_thumbnail_unpin_all();
            first_screen_pin_epoch = epoch;
            album_thumbnail_pin(THUMBNAIL_KEY_ARTIST, album_boot_preload_result_artist_key);
            if (album_boot_preload_result_artist_has_alias)
                album_thumbnail_pin(THUMBNAIL_KEY_ALBUM, album_boot_preload_result_alias_album_key);
        }
    } else {
        free(album_boot_preload_result_pixels);
        album_boot_preload_result_pixels = NULL;
    }
    /* The warmer now runs while list screens are open: repaint only when a
     * row on screen actually shows what was committed, not once per album. */
    /* An artist result may carry an album's pixels too: check both keys. */
    bool shown = (album_boot_preload_result_kind == ALBUM_BOOT_RESULT_ARTIST &&
                  thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_boot_preload_result_artist_key)) ||
                 thumbnail_active_list_shows(album_boot_preload_result_key_kind, album_boot_preload_result_key);
    if (changed && shown) refresh_thumbnail_list_visible(album_thumbnail_active_list);
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
    return true;
}

static void album_thumbnail_discard_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return;
    free(album_boot_preload_result_pixels);
    album_boot_preload_result_pixels = NULL;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
}

static bool album_thumb_gen_cached_key(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < album_thumb_gen_cached_key_count; i++)
        if (album_thumb_gen_cached_key_kinds[i] == key_kind &&
            album_thumb_gen_cached_keys[i] == key) return true;
    return false;
}

/* Persistent warmer state flags */
static pthread_t album_thumb_gen_thread;
static atomic_bool album_thumb_gen_active;
static atomic_bool album_thumb_gen_cancel;
static atomic_int album_thumb_gen_generation;
static bool album_thumb_gen_thread_joinable;
static atomic_int album_thumb_gen_done_count;
static atomic_int album_thumb_gen_total_count;
static atomic_int album_thumb_gen_cache_epoch;
/* playback_pass_epoch records the cache epoch whose playback-time pass
 * finished, so the retry path does not rerun it every backoff; anything that
 * needs real extraction waits for playback to stop. */
static atomic_int album_thumb_gen_playback_pass_epoch = -1;

static atomic_bool album_thumb_gen_retry_pending;
static uint32_t album_thumb_gen_retry_tick;

static bool album_thumb_gen_should_cancel(int my_generation) {
    return atomic_load(&album_thumb_gen_cancel) ||
           atomic_load(&album_thumb_gen_generation) != my_generation ||
           atomic_load(&album_thumb_gen_cache_epoch) != atomic_load(&album_thumbnail_cache_epoch);
}

static void cancel_album_thumbnail_generation(void) {
    if (!atomic_load(&album_thumb_gen_active)) return;
    atomic_store(&album_thumb_gen_cancel, true);
    atomic_fetch_add(&album_thumb_gen_generation, 1);
}

/* Waits for thumbnail generation thread completion. Requires both the
 * joinable flag and a non-NULL thread handle before calling pthread_join(). */
static void reap_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable) return;
    album_thumb_gen_thread_joinable = false;
    if (!album_thumb_gen_thread) return;
    pthread_join(album_thumb_gen_thread, NULL);
}

/* Dynamic cancellation callbacks */
static bool album_thumb_gen_cancel_cb(void * user_data) {
    int my_gen = (int) (intptr_t) user_data;
    return album_thumb_gen_should_cancel(my_gen) ||
           (!album_thumb_gen_user_visible && album_thumbnail_lazy_work_pending());
}

static bool album_thumbnail_cancel_cb(void * user_data) {
    int cache_epoch = (int) (intptr_t) user_data;
    return cache_epoch != atomic_load(&album_thumbnail_cache_epoch);
}

typedef struct {
    int kind;
    int cache_epoch;
    thumbnail_key_kind_t key_kind;
    uint64_t key;
    int64_t song_id;
    uint8_t * pixels;
    bool have_source_mtime;
    time_t source_mtime;
    uint64_t artist_key;
    metadata_db_group_kind_t artist_group_kind;
    bool artist_has_alias;
    bool artist_negative_confirmed;
    uint64_t alias_album_key;
    int64_t alias_song_id;
    bool first_screen; /* the boot first-screen warm: recorded for the press prewarm and pinned */
} album_thumbnail_handoff_t;

static bool album_thumb_gen_handoff_result(int my_generation,
                                            const album_thumbnail_handoff_t * result) {
    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            free(result->pixels);
            return false;
        }
        usleep(5000);
    }
    if (album_thumb_gen_should_cancel(my_generation)) {
        free(result->pixels);
        return false;
    }

    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_CLAIMED);
    album_boot_preload_result_kind = result->kind;
    album_boot_preload_result_cache_epoch = result->cache_epoch;
    album_boot_preload_result_key_kind = result->key_kind;
    album_boot_preload_result_key = result->key;
    album_boot_preload_result_song_id = result->song_id;
    album_boot_preload_result_pixels = result->pixels;
    album_boot_preload_result_have_source_mtime = result->have_source_mtime;
    album_boot_preload_result_source_mtime = result->source_mtime;
    album_boot_preload_result_artist_key = result->artist_key;
    album_boot_preload_result_artist_group_kind = result->artist_group_kind;
    album_boot_preload_result_artist_has_alias = result->artist_has_alias;
    album_boot_preload_result_artist_negative_confirmed = result->artist_negative_confirmed;
    album_boot_preload_result_alias_album_key = result->alias_album_key;
    album_boot_preload_result_alias_song_id = result->alias_song_id;
    album_boot_preload_result_first_screen = result->first_screen;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_PENDING);

    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            int expected_state = ALBUM_BOOT_PRELOAD_PENDING;
            if (atomic_compare_exchange_strong(&album_boot_preload_state, &expected_state,
                                               ALBUM_BOOT_PRELOAD_CLAIMED)) {
                free(album_boot_preload_result_pixels);
                album_boot_preload_result_pixels = NULL;
                atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
                return false;
            }
        }
        usleep(5000);
    }
    return true;
}

static bool album_thumb_gen_warm_artist_aliases(int my_generation) {
    if (!atomic_load(&artist_thumbnail_work_enabled)) return true;
    const metadata_db_group_kind_t kinds[] = {
        METADATA_DB_GROUP_ARTIST,
        METADATA_DB_GROUP_ALBUM_ARTIST
    };
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    int counts[2];
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    counts[0] = artist_count;
    counts[1] = album_artist_count;
    (void) album_count;

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
        metadata_db_group_kind_t group_kind = kinds[kind_index];
        for (int offset = 0; offset < counts[kind_index];) {
            if (album_thumb_gen_should_cancel(my_generation)) return false;
            if (audio_is_playing() || album_thumbnail_lazy_work_pending() ||
                !artwork_check_memory_admission(ARTWORK_PRIO_WARMER,
                                                ALBUM_ART_METADATA_START_BYTES)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                return false;
            }

            int n = metadata_db_get_groups_page(group_kind, offset,
                                                ALBUM_THUMB_GEN_BATCH, rows);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (album_thumb_gen_should_cancel(my_generation)) return false;
                if (!atomic_load(&artist_thumbnail_work_enabled)) return true;
                if (audio_is_playing() || album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    return false;
                }

                uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
                bool alias_known = false;
                for (size_t k = 0; k < album_thumb_gen_known_alias_count && !alias_known; k++)
                    alias_known = album_thumb_gen_known_aliases[k].key == artist_key &&
                                  album_thumb_gen_known_aliases[k].kind == group_kind;
                if (alias_known || album_thumb_gen_cached_key(THUMBNAIL_KEY_ARTIST, artist_key)) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                artist_thumbnail_resolution_t resolution;
                artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(
                    rows[i].first_song_id, group_kind, rows[i].name, ARTWORK_PRIO_WARMER, false,
                    artist_key, album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                    &resolution);
                if (result == ARTIST_THUMB_OWN_IMAGE) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }
                if (result == ARTIST_THUMB_CANCELLED) {
                    if (!album_thumb_gen_should_cancel(my_generation))
                        atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    return false;
                }
                if (result == ARTIST_THUMB_TEMPORARY) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                if (resolution.has_alias || resolution.negative_confirmed || resolution.pixels) {
                    album_thumbnail_handoff_t handoff = {
                        .kind = ALBUM_BOOT_RESULT_ARTIST,
                        .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                        .key_kind = resolution.pixel_key_kind,
                        .key = resolution.pixel_key,
                        .song_id = resolution.pixel_song_id > 0
                                       ? resolution.pixel_song_id : rows[i].first_song_id,
                        .pixels = (uint8_t *) resolution.pixels,
                        .artist_key = artist_key,
                        .artist_group_kind = group_kind,
                        .artist_has_alias = resolution.has_alias,
                        .artist_negative_confirmed = resolution.negative_confirmed,
                        .alias_album_key = resolution.alias_album_key,
                        .alias_song_id = resolution.alias_song_id
                    };
                    if (!album_thumb_gen_handoff_result(my_generation, &handoff)) return false;
                } else {
                    free(resolution.pixels);
                }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US); /* same cooling yield as the album pass */
            }
            offset += n;
            if (n < ALBUM_THUMB_GEN_BATCH) break;
        }
    }
    return true;
}

/* First screen of Artists and Album Artist, decoded into the RAM cache before the long album pass
 * below starts, so a first open (even soon after boot) finds the rows ready. Same worker, same
 * yields as the rest of the warm-up: it stops for playback, visible-row work, low memory and
 * cancellation, hands every result to the UI thread through the same one-slot rendezvous (so the
 * epoch check applies), and honours the artist-image and lazy settings. It adds no buffers: each
 * decoded thumbnail goes into the existing LRU cache and counts against the album pass's RAM
 * budget (album_thumb_gen_ram_filled), so the cache never holds more than its 250 entries and
 * the warmed rows are not the first to be evicted. Runs once per cache epoch. Returns false when
 * the pass must end (yield or cancel; the retry runs it again). */
#define ALBUM_THUMB_GEN_SCREEN_ROWS 12
#define ALBUM_THUMB_GEN_SCREEN_GAP_US 30000
static atomic_int album_thumb_gen_screens_epoch = -1;
static int album_thumb_gen_warm_waited_epoch = -1; /* worker only: the epoch whose warm waited for playback */

/* The warm's own cancel callback: the warmer's usual rules plus playback, so a decode that is
 * already running at thumbnail priority stops when playback starts instead of finishing its
 * extraction beside it. */
static bool album_thumb_gen_warm_cancel_cb(void * user_data) {
    return album_thumb_gen_cancel_cb(user_data) || audio_is_playing();
}

/* Appends an album the warm just handed over, so the album pass does not decode it a second time.
 * Worker thread: it owns the list while a run is active. */
static bool album_thumb_gen_note_cached(thumbnail_key_kind_t key_kind, uint64_t key) {
    if (album_thumb_gen_cached_key_count >= ALBUM_THUMBNAIL_CACHE_SIZE ||
        album_thumb_gen_cached_key(key_kind, key)) return false;
    album_thumb_gen_cached_keys[album_thumb_gen_cached_key_count] = key;
    album_thumb_gen_cached_key_kinds[album_thumb_gen_cached_key_count++] = key_kind;
    return true;
}

/* True when the snapshot says this artist row is already resolved with nothing left to load (a
 * "no art" marker). Alias rows are re-resolved on a retry: the snapshot stays at 16 bytes a row. */
static bool album_thumb_gen_alias_row_warm(uint64_t artist_key, metadata_db_group_kind_t kind) {
    for (size_t k = 0; k < album_thumb_gen_known_alias_count; k++) {
        const album_thumb_gen_alias_key_t * a = &album_thumb_gen_known_aliases[k];
        if (a->key != artist_key || a->kind != kind) continue;
        return !a->has_album;
    }
    return false;
}

static bool album_thumb_gen_warm_first_screens_run(int my_generation, int * warmed, int * skipped,
                                                    const char ** reason) {
    int epoch = atomic_load(&album_thumb_gen_cache_epoch);
    if (atomic_load(&album_thumb_gen_screens_epoch) == epoch) { *reason = "already"; return true; }
    if (!atomic_load(&artist_thumbnail_work_enabled)) { *reason = "disabled"; return true; }
    if (thumb_policy_cb && thumb_policy_cb(LIBRARY_THUMB_POLICY_LAZY)) { *reason = "lazy"; return true; }
    if (audio_is_playing()) { *reason = "playing"; return true; } /* runs after playback stops, see the end of the pass */
    const metadata_db_group_kind_t kinds[] = { METADATA_DB_GROUP_ARTIST, METADATA_DB_GROUP_ALBUM_ARTIST };
    group_row_t rows[ALBUM_THUMB_GEN_SCREEN_ROWS];
    bool transient = false;
    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
        metadata_db_group_kind_t group_kind = kinds[kind_index];
        int n = metadata_db_get_groups_page(group_kind, 0, ALBUM_THUMB_GEN_SCREEN_ROWS, rows);
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) { *reason = "cancel"; return false; }
            if (!atomic_load(&artist_thumbnail_work_enabled)) { *reason = "disabled"; return true; }
            if (audio_is_playing()) { *reason = "playing"; return true; }
            if (album_thumbnail_lazy_work_pending() ||
                !artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                *reason = "yield";
                return false;
            }
            uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
            if (album_thumb_gen_cached_key(THUMBNAIL_KEY_ARTIST, artist_key) ||
                album_thumb_gen_alias_row_warm(artist_key, group_kind)) {
                (*skipped)++;
                continue;
            }
            artist_thumbnail_resolution_t resolution;
            artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(
                rows[i].first_song_id, group_kind, rows[i].name, ARTWORK_PRIO_THUMBNAIL, true,
                artist_key, album_thumb_gen_warm_cancel_cb, (void *) (intptr_t) my_generation, &resolution);
            if (result == ARTIST_THUMB_CANCELLED) {
                bool hard = album_thumb_gen_should_cancel(my_generation);
                if (!hard) atomic_store(&album_thumb_gen_retry_pending, true);
                free(resolution.pixels);
                *reason = hard ? "cancel" : audio_is_playing() ? "playing" : "yield";
                return false;
            }
            if (result == ARTIST_THUMB_TEMPORARY || result == ARTIST_THUMB_OWN_IMAGE ||
                !(resolution.has_alias || resolution.negative_confirmed || resolution.pixels)) {
                if (result == ARTIST_THUMB_TEMPORARY) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    transient = true; /* this epoch is not warmed yet: the retry takes the row again */
                }
                free(resolution.pixels);
                (*skipped)++;
                continue;
            }
            bool has_pixels = resolution.pixels != NULL;
            thumbnail_key_kind_t pixel_kind = resolution.pixel_key_kind;
            uint64_t pixel_key = resolution.pixel_key;
            album_thumbnail_handoff_t handoff = {
                .kind = ALBUM_BOOT_RESULT_ARTIST,
                .cache_epoch = epoch,
                .key_kind = resolution.pixel_key_kind,
                .key = resolution.pixel_key,
                .song_id = resolution.pixel_song_id > 0 ? resolution.pixel_song_id : rows[i].first_song_id,
                .pixels = (uint8_t *) resolution.pixels,
                .artist_key = artist_key,
                .artist_group_kind = group_kind,
                .artist_has_alias = resolution.has_alias,
                .artist_negative_confirmed = resolution.negative_confirmed,
                .alias_album_key = resolution.alias_album_key,
                .alias_song_id = resolution.alias_song_id,
                .first_screen = true
            };
            if (!album_thumb_gen_handoff_result(my_generation, &handoff)) { *reason = "cancel"; return false; }
            /* A key the snapshot already holds is already in the budget (a rerun's duplicate is dropped
             * by the commit). */
            if (has_pixels && album_thumb_gen_note_cached(pixel_kind, pixel_key) &&
                album_thumb_gen_ram_filled < ALBUM_THUMBNAIL_CACHE_SIZE) album_thumb_gen_ram_filled++;
            (*warmed)++;
            usleep(ALBUM_THUMB_GEN_SCREEN_GAP_US);
        }
    }
    if (transient) { *reason = "transient"; return true; }
    atomic_store(&album_thumb_gen_screens_epoch, epoch);
    *reason = "done";
    return true;
}

static bool album_thumb_gen_warm_first_screens(int my_generation) {
    int warmed = 0, skipped = 0;
    const char * reason = "";
    bool ok = album_thumb_gen_warm_first_screens_run(my_generation, &warmed, &skipped, &reason);
    if (strcmp(reason, "playing") == 0) album_thumb_gen_warm_waited_epoch = atomic_load(&album_thumb_gen_cache_epoch);
    if (strcmp(reason, "already") != 0)
        DB_LOG("ART_ARTIST", "first_screen_warm warmed=%d skipped=%d reason=%s", warmed, skipped, reason);
    return ok;
}

/* Keep artwork workers below interactive presentation without raising an
 * inherited background priority. Linux applies this to the calling thread. */
static void thumbnail_worker_lower_priority(int nice_value) {
#ifdef __linux__
    errno = 0;
    int inherited = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && inherited < nice_value)
        (void) setpriority(PRIO_PROCESS, 0, nice_value);
#else
    (void) nice_value;
#endif
}

static void * album_thumbnail_thread_func(void * arg) {
    thumbnail_worker_lower_priority(WORKER_NICE_BACKGROUND);
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
    album_thumbnail_result_have_source_mtime = false;
    album_thumbnail_result_negative_confirmed = false;
    album_thumbnail_result_has_alias = false;
    album_thumbnail_result_artist_negative_confirmed = false;
    album_thumbnail_result_artist_key = 0;
    album_thumbnail_result_alias_album_key = 0;
    album_thumbnail_result_alias_song_id = 0;
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
                album_thumbnail_result_source_mtime = album_source_mtime(&song, &info);
                album_thumbnail_result_have_source_mtime = true;
                album_thumbnail_load_or_decode_with_mtime(&song, ARTWORK_PRIO_THUMBNAIL,
                        album_thumbnail_cancel_cb, (void *) (intptr_t) req->cache_epoch,
                        album_thumbnail_result_source_mtime, false, NULL, false, &pixels);
            }
            if (pixels) {
                album_thumbnail_result_have_source_mtime = false;
                artwork_failure_cache_note_success(req->song_id);
            }
        }
    } else {
        artist_thumbnail_resolution_t resolution;
        (void) artist_thumbnail_load_or_decode(req->song_id, req->artist_group_kind, req->artist_name,
                    ARTWORK_PRIO_THUMBNAIL, true, req->key, album_thumbnail_cancel_cb,
                    (void *) (intptr_t) req->cache_epoch, &resolution);
        pixels = resolution.pixels;
        pixel_key = resolution.pixel_key;
        pixel_key_kind = resolution.pixel_key_kind;
        album_thumbnail_result_has_alias = resolution.has_alias;
        album_thumbnail_result_artist_negative_confirmed = resolution.negative_confirmed;
        album_thumbnail_result_artist_key = req->key;
        album_thumbnail_result_artist_group_kind = req->artist_group_kind;
        album_thumbnail_result_alias_album_key = resolution.alias_album_key;
        album_thumbnail_result_alias_song_id = resolution.alias_song_id;
    }
    album_thumbnail_result_song_id = req->song_id;
    album_thumbnail_result_key = pixel_key;
    album_thumbnail_result_key_kind = pixel_key_kind;
    album_thumbnail_result_request_key_kind = req->key_kind;
    album_thumbnail_result_cache_epoch = req->cache_epoch;
    int result_logical_index = req->logical_index;
    album_thumbnail_result_pixels = (uint8_t *) pixels;
    free(req);
    album_thumbnail_done = true;
    ui_wake_notify(); /* hand the result over on the next loop pass, not the next 50 ms poll */
    if (db_logging && key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "decode_end song=%lld row=%d art=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result_song_id, result_logical_index,
               album_thumbnail_result_pixels != NULL,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
    else if (db_logging)
        DB_LOG("ART_ARTIST", "decode_end song=%lld row=%d art=%d negative=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result_song_id, result_logical_index,
               album_thumbnail_result_pixels != NULL, album_thumbnail_result_negative_confirmed,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb song=%lld total_us=%llu pixels=%d\n",
           (long long) album_thumbnail_result_song_id,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           album_thumbnail_result_pixels != NULL);
#endif
    return NULL;
}

/* ---- Persistent (post-scan) album thumbnail generation ----------------
 * Runs once after Update Music Database completes, writing sized BMP files
 * in short bounded batches with generous cooperative delays so CPU and SD
 * activity never cause overheating or starve audio playback. Suspends
 * automatically when audio is playing, visible lazy work is pending, or
 * memory is low. */
#define ALBUM_THUMB_GEN_INTER_BATCH_US 1000000 /* 1.0 s pause between batches */

/* A reload must leave both generated sizes on disk, except for albums
 * albumart_store_rgb565() cannot name (no album or artist tag). */

/* Artist cache keys of every artist named on an album's tracks (full tag
 * and each split name, matching how artist rows are grouped). */
static bool album_artist_cache_keys(const char * album, const char * album_artist,
                                    uint64_t ** out_keys, size_t * out_count) {
    *out_keys = NULL;
    *out_count = 0;
    size_t capacity = 0;
    song_row_t songs[32];
    for (int offset = 0;;) {
        int count = metadata_db_get_album_songs(album, album_artist, offset, songs,
                                                (int) (sizeof(songs) / sizeof(songs[0])));
        if (count <= 0) break;
        for (int i = 0; i < count; i++) {
            char names[TAGCACHE_ARTIST_SPLIT_MAX + 1][TAGCACHE_TAG_MAX];
            int n = tagcache_artist_names(songs[i].tags.artist, names, TAGCACHE_ARTIST_SPLIT_MAX);
            if (n < 0) n = 0;
            snprintf(names[n++], sizeof(names[0]), "%s", songs[i].tags.artist);
            for (int j = 0; j < n; j++) {
                if (!names[j][0]) continue;
                uint64_t key = albumart_artist_thumbnail_key(names[j]);
                bool seen = false;
                for (size_t k = 0; k < *out_count && !seen; k++) seen = (*out_keys)[k] == key;
                if (seen) continue;
                if (*out_count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    uint64_t * grown = realloc(*out_keys, next * sizeof(*grown));
                    if (!grown) return false;
                    *out_keys = grown;
                    capacity = next;
                }
                (*out_keys)[(*out_count)++] = key;
            }
        }
        offset += count;
        if (count < (int) (sizeof(songs) / sizeof(songs[0]))) break;
    }
    return true;
}

static void * album_thumb_gen_thread_func(void * arg) {
    if (!album_thumb_gen_altstack_already_installed)
        install_thread_crash_altstack(); /* see its own comment (main.c) */
    album_thumb_gen_altstack_already_installed = false;
    int my_generation = (int) (intptr_t) arg;
    bool user_visible = album_thumb_gen_user_visible;
    thumbnail_worker_lower_priority(user_visible ? WORKER_NICE_BACKGROUND : WORKER_NICE_IDLE);
    bool force_reload = album_thumb_gen_force_reload;
    bool single_album = album_thumb_gen_single_album;
    /* A user-started reload may run during playback, which the coordinator
     * refuses at warmer priority. */
    artwork_priority_t decode_prio = user_visible ? ARTWORK_PRIO_THUMBNAIL : ARTWORK_PRIO_WARMER;
    album_art_enable_player_cache_materialization();
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    int generated = 0, cached = 0, missing = 0, failed = 0;
    DB_LOG("ART_CACHE", "worker_begin generation=%d total=%d rss_kb=%ld", my_generation,
           atomic_load(&album_thumb_gen_total_count), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
    int perf_generated = 0, perf_skipped = 0, perf_missing = 0, perf_failed = 0;
#endif
    atomic_store(&album_thumb_gen_done_count, 0);

    if (album_thumb_gen_should_cancel(my_generation)) goto done;
    if (!user_visible && album_thumbnail_lazy_work_pending()) {
        atomic_store(&album_thumb_gen_retry_pending, true);
        goto done;
    }

    if (force_reload) {
        bool storage_ok = metadata_db_storage_current();
#ifndef HOST_BUILD
        storage_ok = storage_ok && sd_card_root_is_mounted();
#endif
        if (!storage_ok) {
            atomic_fetch_add(&album_thumb_gen_error_count, 1);
            goto done;
        }
        /* gui_player_begin_cover_reload() already invalidated any player
         * decode; wait here, off the UI thread, until one still running can
         * no longer write an old cover back after the deletion below. */
        while (!gui_player_cover_decode_idle()) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;
            usleep(10000);
        }
        bool deleted = false;
        if (single_album) {
            song_row_t target_song;
            if (!metadata_db_get_song_by_id(album_thumb_gen_target_song_id, &target_song)) {
                atomic_fetch_add(&album_thumb_gen_error_count, 1);
                goto done;
            }
            albumart_info_t info;
            albumart_info_from_song_row(&target_song, &info);
            /* Compilations: every track artist may hold a stale "no art"
             * marker with no alias pointing at this album. */
            uint64_t * artist_keys = NULL;
            size_t artist_key_count = 0;
            bool keys_ok = album_artist_cache_keys(target_song.tags.album, target_song.tags.album_artist,
                                                   &artist_keys, &artist_key_count);
            deleted = keys_ok && albumart_delete_generated_cache_for_album(&info, artist_keys, artist_key_count);
            free(artist_keys);
        } else {
            deleted = albumart_delete_all_generated_caches();
        }
        if (!deleted) {
            atomic_fetch_add(&album_thumb_gen_error_count, 1);
            goto done;
        }
    }

    if (!user_visible && !single_album && !force_reload && !album_thumb_gen_warm_first_screens(my_generation))
        goto done;

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    int offset = 0;
    for (;;) {
        if (album_thumb_gen_should_cancel(my_generation)) break;

        /* Yield to visible lazy work or memory pressure. Playback uses the
         * cache-only branch below rather than extracting new artwork. */
        if ((!user_visible && album_thumbnail_lazy_work_pending()) ||
            !artwork_check_memory_admission(decode_prio, ALBUM_ART_METADATA_START_BYTES)) {
            if (user_visible) atomic_fetch_add(&album_thumb_gen_error_count, 1);
            else atomic_store(&album_thumb_gen_retry_pending, true);
            goto done;
        }

        int n;
        if (single_album) {
            if (offset > 0) break;
            memset(rows, 0, sizeof(rows));
            rows[0].first_song_id = album_thumb_gen_target_song_id;
            n = 1;
        } else {
            n = metadata_db_get_albums_page_filtered(NULL, offset, ALBUM_THUMB_GEN_BATCH, rows);
        }
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;

            if (force_reload) {
                bool storage_ok = metadata_db_storage_current();
#ifndef HOST_BUILD
                storage_ok = storage_ok && sd_card_root_is_mounted();
#endif
                if (!storage_ok) {
                    atomic_fetch_add(&album_thumb_gen_error_count, 1);
                    goto done;
                }
            }

            /* Check suspension before each album */
            if (!user_visible && album_thumbnail_lazy_work_pending()) {
                if (user_visible) atomic_fetch_add(&album_thumb_gen_error_count, 1);
                else atomic_store(&album_thumb_gen_retry_pending, true);
                goto done;
            }

            song_row_t song;
            if (!metadata_db_get_song_by_id(rows[i].first_song_id, &song)) {
                missing++;
                if (force_reload) atomic_fetch_add(&album_thumb_gen_error_count, 1);
#ifdef UI_PERF_TRACE
                perf_missing++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            uint64_t album_key = song_album_thumbnail_key(&song);
            if (album_thumb_gen_cached_key(THUMBNAIL_KEY_ALBUM, album_key)) {
                cached++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);

            /* During playback, only read an already generated thumbnail cover
             * (thumbnail priority, which the coordinator admits during
             * playback). Anything needing real extraction waits for playback
             * to stop. */
            if (!user_visible && audio_is_playing()) {
                if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE) {
                    /* RAM budget full: further reads would be decoded only
                     * to be freed. End this playback pass now. */
                    atomic_store(&album_thumb_gen_playback_pass_epoch,
                                 atomic_load(&album_thumb_gen_cache_epoch));
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    goto done;
                }
                uint16_t * cached_pixels = NULL;
                char found[PATH_MAX];
                if (albumart_generated_cache_fresh(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                                   found, sizeof(found))) {
                    uint8_t * data = NULL;
                    uint32_t size = 0;
                    if (albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES,
                                              ARTWORK_PRIO_THUMBNAIL) == ALBUMART_LOAD_OK) {
                        if (cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                ARTWORK_PRIO_THUMBNAIL, album_thumb_gen_cancel_cb,
                                (void *) (intptr_t) my_generation, &cached_pixels) != COVER_DECODE_OK) {
                            free(cached_pixels);
                            cached_pixels = NULL;
                        }
                    }
                    free(data);
                }
                if (!cached_pixels) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                } else {
                    album_thumb_gen_ram_filled++;
                    album_thumbnail_handoff_t handoff = {
                        .kind = ALBUM_BOOT_RESULT_ALBUM,
                        .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                        .key_kind = THUMBNAIL_KEY_ALBUM,
                        .key = album_key,
                        .song_id = song.id,
                        .pixels = (uint8_t *) cached_pixels,
                    };
                    if (!album_thumb_gen_handoff_result(my_generation, &handoff)) goto done;
                    cached++;
                }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
                continue;
            }

            time_t source_mtime = album_source_mtime(&song, &info);

            artwork_fail_reason_t fail_reason;
            if (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason)) {
                if (fail_reason == ARTWORK_FAIL_TEMPORARY)
                    atomic_store(&album_thumb_gen_retry_pending, true);
                failed++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            char found[PATH_MAX];
            /* Still enter the decode call below for the first ALBUM_THUMBNAIL_
             * CACHE_SIZE albums even when both persistent caches already exist
             * -- on every boot after the very first one this is nearly always
             * true for the whole library, and skipping straight to `continue`
             * here (as every album past the RAM budget still does) would mean
             * the RAM-preload hand-off below never runs at all on a normal
             * boot. album_thumbnail_load_or_decode_ex() already returns the
             * existing thumbnail sidecar's pixels immediately in this case without
             * touching the original source file, so this costs one cheap BMP
             * read+decode per album instead of a real cache regeneration. */
            if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE &&
                album_thumbnail_sized_cache_hit(&info, found, sizeof(found)) &&
                album_player_cache_hit(&info, found, sizeof(found))) {
                cached++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            uint16_t * pixels = NULL;
            album_thumbnail_load_or_decode_ex(&song, decode_prio,
                                              album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                                              &pixels);
            if (!pixels && ((!user_visible && audio_is_playing()) || album_thumb_gen_should_cancel(my_generation) ||
                (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason) &&
                 fail_reason == ARTWORK_FAIL_TEMPORARY)))
                atomic_store(&album_thumb_gen_retry_pending, true);
            if (pixels) { artwork_failure_cache_note_success(song.id); generated++; } else { failed++; }
            if (force_reload && pixels && !album_thumbnail_reload_persisted(&song)) {
                /* Decoded but not saved (e.g. card full): the next boot
                 * would lose it, so report the reload as incomplete. */
                atomic_fetch_add(&album_thumb_gen_error_count, 1);
            }
            if (force_reload && !pixels) {
                artwork_fail_reason_t current_reason = ARTWORK_FAIL_NONE;
                bool blocked = artwork_failure_cache_is_blocked(song.id, source_mtime, &current_reason);
                if (!blocked || current_reason == ARTWORK_FAIL_TEMPORARY)
                    atomic_fetch_add(&album_thumb_gen_error_count, 1);
            }
            if (!pixels) {
                artwork_fail_reason_t diag_fail_reason = ARTWORK_FAIL_NONE;
                bool diag_blocked = artwork_failure_cache_is_blocked(song.id, source_mtime, &diag_fail_reason);
                DB_LOG("ART_CACHE", "album_failed song=%lld path=%s reason=%s",
                       (long long) song.id, song.path,
                       !diag_blocked ? "unrecorded" :
                       (diag_fail_reason == ARTWORK_FAIL_PERMANENT ? "PERMANENT" : "TEMPORARY"));
            }

#ifdef UI_PERF_TRACE
            if (pixels) perf_generated++; else perf_failed++;
#endif
            bool keep_in_ram = album_thumb_gen_ram_filled < ALBUM_THUMBNAIL_CACHE_SIZE;
            if (keep_in_ram) album_thumb_gen_ram_filled++;
            if (keep_in_ram || (!user_visible && album_thumbnail_lazy_work_pending())) {
                if (album_thumb_gen_should_cancel(my_generation)) {
                    free(pixels);
                    goto done;
                }
                album_thumbnail_handoff_t handoff = {
                    .kind = ALBUM_BOOT_RESULT_ALBUM,
                    .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                    .key_kind = THUMBNAIL_KEY_ALBUM,
                    .key = album_key,
                    .song_id = song.id,
                    .pixels = (uint8_t *) pixels,
                    .have_source_mtime = pixels == NULL,
                    .source_mtime = source_mtime
                };
                if (!album_thumb_gen_handoff_result(my_generation, &handoff)) goto done;
            } else {
                free(pixels);
            }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);

            if (!user_visible && album_thumbnail_lazy_work_pending()) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                goto done;
            }

            int diag_done = atomic_load(&album_thumb_gen_done_count);
            if ((diag_done % 50) == 0) {
                DB_LOG("ART_CACHE", "progress done=%d total=%d generated=%d cached=%d missing=%d failed=%d elapsed_ms=%llu rss_kb=%ld",
                       diag_done, atomic_load(&album_thumb_gen_total_count), generated, cached, missing, failed,
                       (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
            }

            if (!user_visible) usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
        }
        offset += n;
        if (single_album || n < ALBUM_THUMB_GEN_BATCH) break;

        /* Pause between batches to give CPU/SD bus complete rest */
        if (!user_visible)
            for (int p = 0; p < 10; p++) {
                if (album_thumb_gen_should_cancel(my_generation)) goto done;
                if (album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    goto done;
                }
                usleep(ALBUM_THUMB_GEN_INTER_BATCH_US / 10);
            }
    }
    if (!user_visible && !single_album && audio_is_playing()) {
        /* The playback-time cache-only pass is done for this cache epoch;
         * the artist pass and any skipped extraction wait for playback to
         * stop. */
        atomic_store(&album_thumb_gen_playback_pass_epoch, atomic_load(&album_thumb_gen_cache_epoch));
        atomic_store(&album_thumb_gen_retry_pending, true);
        goto done;
    }
    /* Playback was running when this pass began and has stopped since: the first screens that waited
     * for it run now (a no-op when they were already warmed). */
    if (!user_visible && !single_album && !album_thumb_gen_should_cancel(my_generation) &&
        album_thumb_gen_warm_waited_epoch == atomic_load(&album_thumb_gen_cache_epoch) &&
        !album_thumb_gen_warm_first_screens(my_generation))
        goto done;
    if (!user_visible && !single_album && !album_thumb_gen_should_cancel(my_generation) &&
        !album_thumb_gen_warm_artist_aliases(my_generation))
        goto done;
done:
    DB_LOG("ART_CACHE", "worker_end generation=%d done=%d total=%d generated=%d cached=%d missing=%d failed=%d cancelled=%d elapsed_ms=%llu rss_kb=%ld",
           my_generation, atomic_load(&album_thumb_gen_done_count), atomic_load(&album_thumb_gen_total_count),
           generated, cached, missing, failed, (int) album_thumb_gen_should_cancel(my_generation),
           (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb_gen done=%d generated=%d skipped=%d missing=%d failed=%d us=%llu cancelled=%d\n",
           atomic_load(&album_thumb_gen_done_count), perf_generated, perf_skipped, perf_missing, perf_failed,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           (int) album_thumb_gen_should_cancel(my_generation));
#endif
    atomic_store(&album_thumb_gen_active, false);
    ui_wake_notify(); /* a visible-row decode may have been waiting for this worker to yield */
    return NULL;
}

/* Called once, right after Update Music Database's own scan thread is
 * joined and the database is already fully committed (see this function's
 * own call site) -- never blocks the caller waiting for generation itself
 * to finish, only (briefly) for a STILL-RUNNING previous pass to notice
 * it's been superseded and exit, which happens within roughly one album's
 * worth of decode work given the cancellation check at the top of every
 * iteration. */
static int prepare_album_thumbnail_generation(bool user_visible, bool force_reload,
                                             bool single_album, int64_t target_song_id) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    (void) album_thumbnail_commit_boot_preload();
    album_thumb_gen_user_visible = user_visible;
    album_thumb_gen_force_reload = force_reload;
    album_thumb_gen_single_album = single_album;
    album_thumb_gen_target_song_id = target_song_id;
    album_thumb_gen_altstack_already_installed = false;
    atomic_store(&album_thumb_gen_error_count, 0);
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumb_gen_ram_filled = 0;
    album_thumb_gen_cached_key_count = 0;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * entry = &album_thumbnail_cache[i];
        if (!entry->known || !entry->pixels) continue;
        if (album_thumb_gen_cached_key_count >= ALBUM_THUMBNAIL_CACHE_SIZE) break;
        album_thumb_gen_cached_keys[album_thumb_gen_cached_key_count++] = entry->key;
        album_thumb_gen_cached_key_kinds[album_thumb_gen_cached_key_count - 1] = entry->key_kind;
        album_thumb_gen_ram_filled++; /* every entry with pixels uses a slot, whatever its kind */
    }
    album_thumb_gen_retry_tick = lv_tick_get();

    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    free(album_thumb_gen_known_aliases);
    album_thumb_gen_known_aliases = NULL;
    album_thumb_gen_known_alias_count = 0;
    if (artist_thumbnail_alias_cache_capacity)
        album_thumb_gen_known_aliases = malloc(artist_thumbnail_alias_cache_capacity *
                                               sizeof(*album_thumb_gen_known_aliases));
    for (size_t i = 0; album_thumb_gen_known_aliases && i < artist_thumbnail_alias_cache_capacity; i++)
        if (artist_thumbnail_alias_cache[i].known)
            album_thumb_gen_known_aliases[album_thumb_gen_known_alias_count++] =
                (album_thumb_gen_alias_key_t) { artist_thumbnail_alias_cache[i].artist_key,
                                                artist_thumbnail_alias_cache[i].artist_group_kind,
                                                artist_thumbnail_alias_cache[i].has_album };
    atomic_store(&album_thumb_gen_done_count, 0);
    int total = single_album ? 1 : album_count;
    if (!user_visible && !single_album && atomic_load(&artist_thumbnail_work_enabled))
        total += artist_count + album_artist_count;
    atomic_store(&album_thumb_gen_total_count, total);
    atomic_store(&album_thumb_gen_cancel, false);
    atomic_store(&album_thumb_gen_cache_epoch, atomic_load(&album_thumbnail_cache_epoch));
    int generation = atomic_fetch_add(&album_thumb_gen_generation, 1) + 1;
    atomic_store(&album_thumb_gen_active, true);
    /* Only start_next_album_thumbnail()'s own two trigger points used to
     * need this timer, and both already resume it. The RAM-preload hand-off
     * below now also depends on it ticking to drain album_boot_preload_
     * pending -- without this, a cold boot's first generation pass would
     * stage exactly one result and then block forever with nothing left to
     * consume it. */
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
    DB_LOG("ART_CACHE", "start generation=%d albums=%d rss_kb=%ld",
           generation, single_album ? 1 : album_count, db_log_rss_kb());
    return generation;
}

static bool launch_album_thumbnail_generation(int generation) {
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumb_gen_thread, attr_ptr, album_thumb_gen_thread_func,
                                   (void *) (intptr_t) generation) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        DB_LOG("ART_CACHE", "thread_create_failed generation=%d rss_kb=%ld", generation, db_log_rss_kb());
    } else {
        album_thumb_gen_thread_joinable = true;
    }
    return created;
}

static void start_album_thumbnail_generation(void) {
    int generation = prepare_album_thumbnail_generation(false, false, false, 0);
    (void) launch_album_thumbnail_generation(generation);
}

struct prewarm_job {
    pthread_t thread;
    atomic_int fin;                 /* worker/owner handoff, see gui_library_async.h */
    atomic_bool cancel;
    atomic_bool done;               /* the worker has staged its last result */
    atomic_int published;           /* results staged so far (worker writes, UI reads) */
    int consumed;                   /* UI thread only */
    metadata_db_group_kind_t kind;
    int kind_index, first, count, epoch;
    bool finished_clean;            /* written before the release store of done */
    int unresolved;                 /* UI thread: rows that ended with neither pixels nor an alias/no-art mark */
    album_thumbnail_handoff_t results[PREWARM_MAX];
};

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
    thumbnail_worker_lower_priority(WORKER_NICE_BACKGROUND);
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
        job->results[i] = (album_thumbnail_handoff_t) {
            .kind = ALBUM_BOOT_RESULT_ARTIST,
            .cache_epoch = job->epoch,
            .key_kind = resolution.pixel_key_kind,
            .key = resolution.pixel_key,
            .song_id = resolution.pixel_song_id > 0 ? resolution.pixel_song_id : rows[i].first_song_id,
            .pixels = (uint8_t *) resolution.pixels,
            .artist_key = artist_key,
            .artist_group_kind = job->kind,
            .artist_has_alias = resolution.has_alias,
            .artist_negative_confirmed = resolution.negative_confirmed,
            .alias_album_key = resolution.alias_album_key,
            .alias_song_id = resolution.alias_song_id
        };
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
    bool committed = false;
    if (r->artist_has_alias || r->artist_negative_confirmed) {
        artist_thumbnail_alias_entry_t * old = artist_thumbnail_alias_find(r->artist_key, r->artist_group_kind);
        if (!old || old->has_album != r->artist_has_alias ||
            (r->artist_has_alias && old->album_key != r->alias_album_key)) *changed = true;
        artist_thumbnail_alias_cache_commit(r->artist_key, r->artist_group_kind, r->artist_has_alias,
                                            r->alias_album_key, r->alias_song_id);
        if (r->artist_has_alias) {
            album_thumbnail_cache_entry_t * own = album_thumbnail_cache_find(THUMBNAIL_KEY_ARTIST, r->artist_key);
            if (own) *changed = true;
            album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST, r->artist_key);
        }
        album_thumbnail_queue_remove_artist(r->artist_key, r->artist_group_kind);
        committed = true;
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
            album_thumbnail_cache_commit(r->key_kind, r->key, r->song_id, r->pixels, false, 0, false);
            r->pixels = NULL;
            *changed = true;
        }
        /* The row's own request is now redundant: the ordinary decoder must not decode it again. */
        album_thumbnail_queue_remove_artist(r->artist_key, r->artist_group_kind);
        album_thumbnail_queue_remove(r->key_kind, r->key);
        committed = true;
    }
    return committed;
}

/* UI thread: commits the staged results in one pass (one repaint of the shown rows) and reaps the
 * thread once it has ended. Each result lands as soon as it is staged (one cheap repaint per frame). Returns true while the prewarm
 * still has work or results outstanding. */
static bool prewarm_poll_ex(bool force) {
    prewarm_job_t * job = prewarm_job;
    if (!job) return false;
    bool running = !atomic_load_explicit(&job->done, memory_order_acquire);
    int published = atomic_load_explicit(&job->published, memory_order_acquire);
    int kind_index = job->kind_index;
    (void) force;
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

static bool prewarm_poll(void) { return prewarm_poll_ex(false); }

/* Stops the job without waiting: the worker ends within a row, the poll reaps it. */
void library_thumbnails_prewarm_cancel(void) {
    prewarm_waiting = false;
    if (prewarm_job) atomic_store(&prewarm_job->cancel, true);
}

/* UI thread, quiesce: cancel, give the job up and let the worker free it. Never waits; the shared
 * orphan wait of teardown and shutdown bounds the stalled case. */
static void prewarm_discard(void) {
    prewarm_waiting = false;
    prewarm_job_t * job = prewarm_job;
    if (!job) return;
    atomic_store(&job->cancel, true);
    pthread_detach(job->thread);
    if (library_async_abandon(&job->fin, &prewarm_orphans)) prewarm_free_job(job);
    prewarm_job = NULL;
    atomic_store(&prewarm_active, false);
}

void library_thumbnails_prewarm_collect(void) {
    (void) prewarm_poll_ex(true);
}

/* True when a prewarm may start now: artist images on, not in the lazy policy, no cover reload. */
static bool prewarm_allowed(void) {
    return atomic_load(&artist_thumbnail_work_enabled) &&
           !(thumb_policy_cb && thumb_policy_cb(LIBRARY_THUMB_POLICY_LAZY)) &&
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
            album_thumbnail_cache_entry_t * art = album_thumbnail_cache_find(alias->album_key_kind, alias->album_key);
            if (!(art && art->pixels)) return false;
        } else if (!alias) {
            return false;
        }
    }
    return true;
}

/* Starts the thread for the request (UI thread; nothing else decoding). */
static void prewarm_launch(int kind_index, int first, int rows) {
    /* Stalled jobs that were given up still hold a thread: never add to them. */
    if (library_async_kind_saturated(&prewarm_orphans)) return;
    metadata_db_group_kind_t kind = kind_index ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST;
    int epoch = atomic_load(&album_thumbnail_cache_epoch);
    const char * why = "not_warmed";
    if (prewarm_done_epoch[kind_index] == epoch && prewarm_done_first[kind_index] == first) {
        if (prewarm_boot_pending[kind_index] && atomic_load(&album_thumb_gen_screens_epoch) != epoch) {
            why = "boot_warm_incomplete";
        } else {
            /* Already warmed for this cache: nothing to do while all of it is still there. */
            bool all_there = prewarm_rows_all_there(kind_index, kind);
            if (all_there) {
                DB_LOG("ART_ARTIST", "prewarm_skipped kind=%d first=%d keys=%d", kind_index, first,
                       prewarm_keys_count[kind_index]);
                return;
            }
            why = "evicted";
        }
    }
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
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
}

/* UI thread, poll: a request that had to wait for the warmer to yield starts once it has, if it is
 * still wanted and allowed. */
static void prewarm_start_waiting(void) {
    if (!prewarm_waiting) return;
    if (lv_tick_elaps(prewarm_wait_tick) > PREWARM_WAIT_MS || !prewarm_allowed()) { prewarm_waiting = false; return; }
    if (prewarm_job || atomic_load(&album_thumb_gen_active) || atomic_load(&album_thumbnail_active)) return;
    prewarm_waiting = false;
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
        metadata_db_group_kind_t kind = kind_index ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST;
        int epoch = atomic_load(&album_thumbnail_cache_epoch);
        if (prewarm_done_epoch[kind_index] == epoch && prewarm_done_first[kind_index] == first_row &&
            !(prewarm_boot_pending[kind_index] && atomic_load(&album_thumb_gen_screens_epoch) != epoch) &&
            prewarm_rows_all_there(kind_index, kind)) {
            DB_LOG("ART_ARTIST", "prewarm_skipped kind=%d first=%d keys=%d", kind_index, first_row,
                   prewarm_keys_count[kind_index]);
            return;
        }
    }
    if (prewarm_job || atomic_load(&album_thumb_gen_active)) {
        /* The warmer (which yields to visible-row work, see album_thumb_gen_cancel_cb) is running: the
         * request waits for it, and counts as visible-row work meanwhile so the warmer stops soon. */
        prewarm_waiting = true;
        prewarm_wait_tick = lv_tick_get();
        prewarm_wait_kind_index = kind_index;
        prewarm_wait_first = first_row;
        prewarm_wait_rows = rows;
        if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
        return;
    }
    prewarm_launch(kind_index, first_row, rows);
}

static void start_next_album_thumbnail(void) {
    if ((thumb_policy_cb && thumb_policy_cb(LIBRARY_THUMB_POLICY_LAZY)) || !album_thumbnail_active_list ||
        !album_thumbnail_list_is_visible(album_thumbnail_active_list) ||
        album_thumbnail_scrolling || atomic_load(&album_thumbnail_active) ||
        atomic_load(&album_thumbnail_queue_count) <= 0) return;
    /* Never run two full cover decoders at once on the 56 MiB target. The
     * post-load warmer can still be finishing its current album when a row
     * queues work. Keep this poll timer alive and give the visible-row job
     * priority as soon as that worker yields instead of doubling peak decode
     * memory. */
    if (atomic_load(&album_thumb_gen_active) || atomic_load(&prewarm_active)) {
        if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
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
    /* See start_album_thumbnail_generation()'s own ALBUM_COVER_DECODE_THREAD_
     * STACK_SIZE comment -- same full JPEG/PNG cover decode, same fix. */
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumbnail_thread, attr_ptr, album_thumbnail_thread_func, req) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumbnail_active, false);
        album_thumbnail_active_key_valid = false;
        free(req);
        return;
    }
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
}

static void queue_thumbnail(lv_obj_t * list, int logical_index, thumbnail_key_kind_t key_kind,
                            uint64_t key, int64_t representative_song_id,
                            metadata_db_group_kind_t artist_group_kind, const char * artist_name) {
    if ((key_kind == THUMBNAIL_KEY_ARTIST && !atomic_load(&artist_thumbnail_work_enabled)) ||
        (thumb_policy_cb && thumb_policy_cb(LIBRARY_THUMB_POLICY_LAZY)) || representative_song_id <= 0 || key == 0 || album_thumbnail_scrolling ||
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
    if (key_kind == THUMBNAIL_KEY_ARTIST)
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











static void album_thumbnail_poll_cb(lv_timer_t * timer) {
    /* Same reasoning as compact_list_poll_fetch_cb()'s own comment
     * (screen_builders.c): this timer's row-image/cache-eviction work
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
            !atomic_load(&prewarm_active) && !prewarm_waiting) lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&album_thumbnail_done)) return;
    pthread_join(album_thumbnail_thread, NULL);
    /* Keep the slot busy until all result fields are consumed. Row refreshes
     * below can synchronously queue work and try to start the next worker. */

    bool cache_epoch_matches = album_thumbnail_result_cache_epoch ==
                               atomic_load(&album_thumbnail_cache_epoch);
    bool result_committed = false;
    bool result_had_art = album_thumbnail_result_pixels != NULL;
    if (cache_epoch_matches) {
        if (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ARTIST) {
            if (album_thumbnail_result_has_alias || album_thumbnail_result_artist_negative_confirmed) {
                artist_thumbnail_alias_cache_commit(album_thumbnail_result_artist_key,
                    album_thumbnail_result_artist_group_kind,
                    album_thumbnail_result_has_alias, album_thumbnail_result_alias_album_key,
                    album_thumbnail_result_alias_song_id);
                if (album_thumbnail_result_has_alias)
                    album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST,
                                                 album_thumbnail_result_artist_key);
                album_thumbnail_queue_remove_artist(album_thumbnail_result_artist_key,
                                                    album_thumbnail_result_artist_group_kind);
                result_committed = true;
            }
            if (album_thumbnail_result_pixels) {
                if (album_thumbnail_result_key_kind == THUMBNAIL_KEY_ARTIST)
                    artist_thumbnail_alias_cache_remove(album_thumbnail_result_artist_key,
                                                        album_thumbnail_result_artist_group_kind);
                album_thumbnail_cache_commit(album_thumbnail_result_key_kind, album_thumbnail_result_key,
                    album_thumbnail_result_song_id, album_thumbnail_result_pixels, false, 0, false);
                album_thumbnail_result_pixels = NULL;
                result_committed = true;
            }
        } else {
            album_thumbnail_cache_commit(THUMBNAIL_KEY_ALBUM, album_thumbnail_result_key,
                album_thumbnail_result_song_id, album_thumbnail_result_pixels,
                album_thumbnail_result_have_source_mtime, album_thumbnail_result_source_mtime,
                album_thumbnail_result_negative_confirmed);
            album_thumbnail_result_pixels = NULL;
            result_committed = true;
        }
    }
    bool same_view = album_thumbnail_active_view_generation == atomic_load(&album_thumbnail_view_generation);
    bool repaint_result = cache_epoch_matches &&
        ((same_view && album_thumbnail_active_list && album_thumbnail_list_is_visible(album_thumbnail_active_list)) ||
         (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ARTIST &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_thumbnail_result_artist_key)) ||
         /* An artist result may carry an album's pixels too (key_kind ALBUM). */
         (result_had_art &&
          thumbnail_active_list_shows(album_thumbnail_result_key_kind, album_thumbnail_result_key)) ||
         (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ALBUM &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ALBUM, album_thumbnail_result_key)));
    album_lazy_completed++;
    if (result_had_art) album_lazy_with_art++;
    if (!cache_epoch_matches) album_lazy_stale++;
    if (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "result song=%lld row=%d committed=%d repaint=%d art=%d queue_depth=%d ui_wait_ms=%llu",
               (long long) album_thumbnail_result_song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, atomic_load(&album_thumbnail_queue_count),
               album_lazy_job_started_ms ? (unsigned long long) (db_log_now_ms() - album_lazy_job_started_ms) : 0ULL);
    else
        DB_LOG("ART_ARTIST", "result song=%lld row=%d committed=%d repaint=%d art=%d alias=%d album=%016llx queue_depth=%d",
               (long long) album_thumbnail_result_song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, album_thumbnail_result_has_alias,
               (unsigned long long) album_thumbnail_result_alias_album_key,
               atomic_load(&album_thumbnail_queue_count));
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (repaint_result) {
        /* Only the rows bound to the landed artwork, and the rows whose picture
         * the cache evicted meanwhile (compact_list_detach_image_src() marks
         * them): committing artwork can evict a descriptor borrowed by another
         * row, without changing its text. */
        refresh_thumbnail_list_landed(album_thumbnail_active_list, album_thumbnail_result_key_kind,
                                      album_thumbnail_result_key, album_thumbnail_result_artist_key,
                                      album_thumbnail_result_request_key_kind);
    }
    atomic_store(&album_thumbnail_active, false);
    album_thumbnail_active_key_valid = false;
    start_next_album_thumbnail();
    if (!atomic_load(&album_thumbnail_active)) {
        /* Nothing else is coming: one full (idempotent, so cheap) pass lets any
         * visible row that was not queued yet queue itself, as every landing's
         * full refresh used to. */
        if (repaint_result) refresh_thumbnail_list_visible(album_thumbnail_active_list);
        if (!atomic_load(&album_thumbnail_active)) lv_timer_pause(timer);
    }
}

/* UI reloads and database replacement both invalidate every list pointer
 * carried by the lazy artwork request. Stop the warmer first, then join the
 * single visible-row decoder before any screen or DB generation is torn
 * down. Both paths are cooperatively bounded by the artwork timeout. */


static thumbnail_binding_t *thumbnail_binding_for_list(lv_obj_t *list) {
    if (!list) return NULL;
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
        if (thumbnail_bindings[i].list == list) return &thumbnail_bindings[i];
    return NULL;
}

static bool thumbnail_role_is_custom(library_thumbnail_list_role_t role) {
    return role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM;
}

static bool thumbnail_role_visible_capability(library_thumbnail_list_role_t role) {
    return role != LIBRARY_THUMB_LIST_GENRE_GROUPS && role != LIBRARY_THUMB_LIST_GENRE_ALBUMS;
}
static bool thumbnail_role_compact_capability(library_thumbnail_list_role_t role) {
    return role >= LIBRARY_THUMB_LIST_ALBUMS && role <= LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH;
}

static library_thumbnail_context_t thumbnail_context_for_list(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding) return album_thumbnail_context;
    switch (binding->role) {
    case LIBRARY_THUMB_LIST_ARTISTS: return artist_thumbnail_context;
    case LIBRARY_THUMB_LIST_ALBUM_ARTISTS: return album_artist_thumbnail_context;
    case LIBRARY_THUMB_LIST_GENRE_GROUPS:
        return thumb_genre_context_cb ? thumb_genre_context_cb() : artist_thumbnail_context;
    default: return album_thumbnail_context;
    }
}

static bool album_thumbnail_list_is_visible(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding || !list || !thumbnail_role_visible_capability(binding->role)) return false;
    if (lv_screen_active() != binding->screen) return false;
    if (binding->role == LIBRARY_THUMB_LIST_FILES_SEARCH ||
        binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH) {
        return !lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN);
    }
    if (binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM) {
        thumbnail_binding_t *search = NULL;
        for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
            if (thumbnail_bindings[i].role == LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH) search = &thumbnail_bindings[i];
        return !search || lv_obj_has_flag(search->list, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

static void album_thumbnail_screen_loaded_cb(lv_event_t *e) {
    library_thumbnails_begin_view((lv_obj_t *)lv_event_get_user_data(e));
}
static void album_thumbnail_screen_unloaded_cb(lv_event_t *e) {
    library_thumbnails_end_view((lv_obj_t *)lv_event_get_user_data(e));
}

static void album_thumbnail_scroll_cb(lv_event_t *e) {
    lv_obj_t *list = lv_event_get_target(e);
    if (list != album_thumbnail_active_list) return;
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        album_thumbnail_scrolling = true;
        album_thumbnail_queue_count = 0;
        atomic_fetch_add(&album_thumbnail_view_generation, 1);
    } else if (lv_event_get_code(e) == LV_EVENT_SCROLL_END) {
        album_thumbnail_scrolling = false;
        refresh_thumbnail_list_visible(list);
    }
}

typedef struct {
    uint64_t key;
    uint64_t artist_key;
    bool artist_list;
    metadata_db_group_kind_t group_kind;
} landed_thumbnail_t;

/* Does a row with this artwork key show (or alias to) the artwork that landed? */
static bool landed_thumbnail_matches(uint64_t row_key, void *ctx) {
    const landed_thumbnail_t *landed = ctx;
    if (row_key == landed->key || (landed->artist_key && row_key == landed->artist_key)) return true;
    if (!landed->artist_list) return false;
    artist_thumbnail_alias_entry_t *alias = artist_thumbnail_alias_find(row_key, landed->group_kind);
    return alias && alias->has_album && alias->album_key == landed->key;
}

static void refresh_thumbnail_list_landed(lv_obj_t *list, thumbnail_key_kind_t key_kind, uint64_t key,
                                          uint64_t artist_key, thumbnail_key_kind_t request_kind) {
    (void) key_kind;
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding || !list) return;
    if (thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
        return;
    }
    library_thumbnail_context_t context = thumbnail_context_for_list(list);
    landed_thumbnail_t landed = {
        .key = key,
        .artist_key = request_kind == THUMBNAIL_KEY_ARTIST ? artist_key : 0,
        .artist_list = context.key_kind == THUMBNAIL_KEY_ARTIST,
        .group_kind = context.artist_group_kind
    };
    compact_list_refresh_decorations_for(list, landed_thumbnail_matches, &landed);
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

static void refresh_thumbnail_list_visible(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding || !list) return;
    if (thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
    } else {
        compact_list_refresh_decorations(list);
    }
}

static bool release_thumbnail_dsc(const lv_image_dsc_t *dsc) {
    bool active_detached = false;
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i) {
        thumbnail_binding_t *binding = &thumbnail_bindings[i];
        if (!binding->list) continue;
        if (thumbnail_role_compact_capability(binding->role) &&
            compact_list_detach_image_src(binding->list, dsc) &&
            binding->list == album_thumbnail_active_list)
            active_detached = true;
        if (thumbnail_role_is_custom(binding->role) && thumb_detach_group_cb &&
            thumb_detach_group_cb(dsc) && binding->list == album_thumbnail_active_list)
            active_detached = true;
    }
    return active_detached;
}

static void refresh_all_thumbnail_lists(void) {
    bool custom_bound = false;
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i) {
        thumbnail_binding_t *binding = &thumbnail_bindings[i];
        if (!binding->list) continue;
        if (thumbnail_role_compact_capability(binding->role))
            refresh_thumbnail_list_visible(binding->list);
        else if (thumbnail_role_is_custom(binding->role))
            custom_bound = true;
    }
    if (custom_bound && thumb_refresh_group_cb) thumb_refresh_group_cb();
}

static void thumbnail_cover_thread_cleanup(void) {
    atomic_store_explicit(&thumbnail_cover_done, true, memory_order_release);
}

static void *thumbnail_cover_thread_func(void *arg) {
    install_thread_crash_altstack();
#ifdef __linux__
    struct sched_param param = { .sched_priority = 0 };
    (void)sched_setscheduler(0, SCHED_BATCH, &param);
#endif
    thumbnail_worker_lower_priority(WORKER_NICE_BACKGROUND);
    album_thumb_gen_altstack_already_installed = true;
    album_thumb_gen_thread_func(arg);
    thumbnail_cover_thread_cleanup();
    return NULL;
}

void library_thumbnails_init(bool artist_images_enabled,
                             library_thumbnail_policy_cb_t policy,
                             library_thumbnail_genre_context_cb_t genre_context,
                             library_thumbnail_detach_group_cb_t detach_group,
                             library_thumbnail_refresh_group_cb_t refresh_group) {
    thumb_policy_cb = policy;
    thumb_genre_context_cb = genre_context;
    thumb_detach_group_cb = detach_group;
    thumb_refresh_group_cb = refresh_group;
    atomic_store(&artist_thumbnail_work_enabled, artist_images_enabled);
    if (!album_thumbnail_poll_timer) {
        album_thumbnail_poll_timer = lv_timer_create(album_thumbnail_poll_cb, 50, NULL);
        if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
    }
}

void library_thumbnails_bind_list(lv_obj_t *screen, lv_obj_t *list,
                                  library_thumbnail_list_role_t role) {
    if (!screen || !list) return;
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding) {
        for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
            if (!thumbnail_bindings[i].list) { binding = &thumbnail_bindings[i]; break; }
    }
    if (!binding) return;
    *binding = (thumbnail_binding_t) { .screen = screen, .list = list, .role = role };
    if (!thumbnail_role_is_custom(role))
        compact_list_set_row_decorator(list, thumbnail_row_decorator, NULL);
    if (role != LIBRARY_THUMB_LIST_FILES_SEARCH && role != LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH) {
        lv_obj_add_event_cb(screen, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, list);
        lv_obj_add_event_cb(screen, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, list);
    }
    lv_obj_add_event_cb(list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
}

void library_thumbnails_forget_list(lv_obj_t *list) {
    if (!list) return;
    bool was_active = album_thumbnail_active_list == list;
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
        if (thumbnail_bindings[i].list == list) memset(&thumbnail_bindings[i], 0, sizeof(thumbnail_bindings[i]));
    for (int i = 0; i < atomic_load(&album_thumbnail_queue_count);) {
        if (album_thumbnail_queue[i].list != list) { ++i; continue; }
        int count = atomic_load(&album_thumbnail_queue_count);
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t)(count - i - 1));
        atomic_fetch_sub(&album_thumbnail_queue_count, 1);
    }
    if (was_active) {
        album_thumbnail_active_list = NULL;
        atomic_fetch_add(&album_thumbnail_view_generation, 1);
    }
}

void library_thumbnails_begin_view(lv_obj_t *list) { thumbnail_begin_screen(list); }
void library_thumbnails_end_view(lv_obj_t *list) { thumbnail_end_screen(list); }
void library_thumbnails_group_rows_rebuilt(lv_obj_t *list) {
    if (list == album_thumbnail_active_list) {
        atomic_fetch_add(&album_thumbnail_view_generation, 1);
        album_thumbnail_queue_count = 0;
    }
}

static void thumbnail_begin_screen(lv_obj_t *list) {
    if (!thumbnail_binding_for_list(list)) return;
    album_thumbnail_view_generation++;
    album_thumbnail_active_list = list;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    album_lazy_queued = album_lazy_completed = album_lazy_with_art = album_lazy_stale = 0;
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (binding && (binding->role == LIBRARY_THUMB_LIST_ALBUMS ||
                    binding->role == LIBRARY_THUMB_LIST_ARTIST_ALBUMS)) {
        const char *page = binding->role == LIBRARY_THUMB_LIST_ALBUMS ? "albums" : "artist_albums";
        DB_LOG("ALBUMS_PAGE", "loaded page=%s request_to_loaded_ms=%llu rss_kb=%ld", page,
               albums_page_open_requested_ms ? (unsigned long long)(db_log_now_ms() - albums_page_open_requested_ms) : 0ULL,
               db_log_rss_kb());
    } else {
        const char *page = binding && binding->role == LIBRARY_THUMB_LIST_ARTISTS ? "artists" :
                           binding && binding->role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS ? "album_artists" :
                           binding && binding->role == LIBRARY_THUMB_LIST_ALL_SONGS ? "all_songs" :
                           binding && binding->role == LIBRARY_THUMB_LIST_RECENTLY_ADDED ? "recently_added" :
                           binding && binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM ? "group_songs" :
                           binding && binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH ? "group_songs_search" :
                           "files_search";
        DB_LOG("ART_LIST_PAGE", "loaded page=%s rss_kb=%ld", page, db_log_rss_kb());
    }
    albums_page_open_requested_ms = 0;
    if (binding && thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
    } else {
        compact_list_refresh_on_show(list);
    }
}

static void thumbnail_end_screen(lv_obj_t *list) {
    if (album_thumbnail_active_list != list) return;
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    album_thumbnail_active_list = NULL;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    if (binding && (binding->role == LIBRARY_THUMB_LIST_ALBUMS ||
                    binding->role == LIBRARY_THUMB_LIST_ARTIST_ALBUMS))
        DB_LOG("ALBUMS_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               binding->role == LIBRARY_THUMB_LIST_ALBUMS ? "albums" : "artist_albums",
               album_lazy_queued, album_lazy_completed, album_lazy_with_art, album_lazy_stale, db_log_rss_kb());
    else {
        const char *page = binding && binding->role == LIBRARY_THUMB_LIST_ARTISTS ? "artists" :
                           binding && binding->role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS ? "album_artists" :
                           binding && binding->role == LIBRARY_THUMB_LIST_ALL_SONGS ? "all_songs" :
                           binding && binding->role == LIBRARY_THUMB_LIST_RECENTLY_ADDED ? "recently_added" :
                           binding && binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM ? "group_songs" :
                           binding && binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH ? "group_songs_search" :
                           "files_search";
        DB_LOG("ART_LIST_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               page, album_lazy_queued, album_lazy_completed, album_lazy_with_art, album_lazy_stale,
               db_log_rss_kb());
    }
    album_thumbnail_view_generation++;
}

void library_thumbnails_decorate_group_row(lv_obj_t *list, lv_obj_t *row,
                                           lv_obj_t *image, int logical_index,
                                           int pool_slot, int64_t song_id,
                                           uint64_t artwork_key) {
    thumbnail_row_decorator(list, row, image, logical_index, pool_slot, song_id,
                            artwork_key, NULL, (void *)&album_thumbnail_context);
}

void library_thumbnails_decorate_cover_image(lv_obj_t *list, lv_obj_t *row, lv_obj_t *image,
                                             int64_t song_id, uint64_t artwork_key) {
    thumbnail_decorate_image(list, row, image, 0, 0, song_id, artwork_key, NULL,
                             (void *)&album_thumbnail_context, false);
}

void library_thumbnails_start_warmup(void) { start_album_thumbnail_generation(); }

void library_thumbnails_retry_warmup(void) {
    if (album_thumb_gen_thread_joinable && !atomic_load(&album_thumb_gen_active))
        reap_album_thumbnail_generation();
    if ((thumb_policy_cb && thumb_policy_cb(LIBRARY_THUMB_POLICY_WARMUP_RETRY)) ||
        !atomic_load(&album_thumb_gen_retry_pending) || atomic_load(&album_thumb_gen_active) ||
        atomic_load(&album_thumbnail_active) || album_thumbnail_lazy_work_pending() ||
        (audio_is_playing() && atomic_load(&album_thumb_gen_playback_pass_epoch) ==
                                atomic_load(&album_thumbnail_cache_epoch)) ||
        lv_tick_elaps(album_thumb_gen_retry_tick) < 15000) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif
    if (!artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES)) return;
    start_album_thumbnail_generation();
}

void library_thumbnails_cancel_warmup(void) {
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
}

void library_thumbnails_invalidate_storage(void) {
    album_thumbnail_cache_epoch_advance();
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
}

void library_thumbnails_note_album_page_open(void) { albums_page_open_requested_ms = db_log_now_ms(); }

bool library_thumbnails_start_cover_reload(const library_thumbnail_cover_request_t *request,
                                           library_thumbnail_show_busy_cb_t show_busy) {
    if (!request) return false;
    int generation = prepare_album_thumbnail_generation(true, true,
        request->scope == LIBRARY_THUMB_COVER_ALBUM, request->target_song_id);
    atomic_store(&thumbnail_cover_done, false);
    if (show_busy) show_busy(request->scope);
    pthread_attr_t attr;
    pthread_attr_t *attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized && pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    bool created = pthread_create(&thumbnail_cover_thread, attr_ptr, thumbnail_cover_thread_func,
                                  (void *)(intptr_t)generation) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        atomic_store(&album_thumb_gen_cancel, true);
        return false;
    }
    thumbnail_cover_thread_joinable = true;
    return true;
}

void library_thumbnails_cancel_cover_reload(void) { cancel_album_thumbnail_generation(); }

void library_thumbnails_join_cover_abort(void) {
    if (!thumbnail_cover_thread_joinable) return;
    thumbnail_cover_thread_joinable = false;
    pthread_join(thumbnail_cover_thread, NULL);
    atomic_store(&thumbnail_cover_done, false);
}

void library_thumbnails_finish_cover_reload(void) {
    if (thumbnail_cover_thread_joinable) {
        thumbnail_cover_thread_joinable = false;
        pthread_join(thumbnail_cover_thread, NULL);
    }
    atomic_store(&thumbnail_cover_done, false);
    (void)album_thumbnail_commit_boot_preload();
    refresh_all_thumbnail_lists();
}

void library_thumbnails_get_status(library_thumbnail_status_t *out) {
    if (!out) return;
    *out = (library_thumbnail_status_t) {
        .lazy_active = atomic_load(&album_thumbnail_active),
        .warmup_active = atomic_load(&album_thumb_gen_active),
        .cover_active = thumbnail_cover_thread_joinable,
        .cover_done = atomic_load_explicit(&thumbnail_cover_done, memory_order_acquire),
        .cover_cancelled = atomic_load(&album_thumb_gen_cancel),
        .cover_done_count = atomic_load(&album_thumb_gen_done_count),
        .cover_total_count = atomic_load(&album_thumb_gen_total_count),
        .cover_error_count = atomic_load(&album_thumb_gen_error_count),
        .decoded_cache_entries = 0
    };
#ifdef UI_PERF_TRACE
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; ++i)
        if (album_thumbnail_cache[i].pixels) out->decoded_cache_entries++;
#endif
}

void library_thumbnails_set_artist_images_enabled(bool enabled) {
    atomic_store(&artist_thumbnail_work_enabled, enabled);
    if (!enabled) {
        for (int i = atomic_load(&album_thumbnail_queue_count) - 1; i >= 0; --i) {
            thumbnail_binding_t *binding = thumbnail_binding_for_list(album_thumbnail_queue[i].list);
            bool artist_list = binding && (binding->role == LIBRARY_THUMB_LIST_ARTISTS ||
                                           binding->role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS);
            if (album_thumbnail_queue[i].key_kind != THUMBNAIL_KEY_ARTIST && !artist_list) continue;
            int count = atomic_load(&album_thumbnail_queue_count);
            memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                    sizeof(album_thumbnail_queue[0]) * (size_t)(count - i - 1));
            atomic_fetch_sub(&album_thumbnail_queue_count, 1);
        }
    }
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
        if (thumbnail_bindings[i].list && (thumbnail_bindings[i].role == LIBRARY_THUMB_LIST_ARTISTS ||
                                           thumbnail_bindings[i].role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS))
            compact_list_refresh_decorations(thumbnail_bindings[i].list);
}

void library_thumbnails_invalidate_album(uint64_t album_key) {
    album_thumbnail_cache_remove(THUMBNAIL_KEY_ALBUM, album_key);
    album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST);
    artist_thumbnail_alias_cache_clear();
}

void library_thumbnails_invalidate_publication_view(void) {
    album_thumbnail_cache_epoch_advance();
    atomic_fetch_add(&album_thumbnail_view_generation, 1);
    album_thumbnail_queue_count = 0;
    album_thumbnail_active_key_valid = false;
}

void library_thumbnails_invalidate_publication(bool clear_all, bool invalidate_artists) {
    if (clear_all) album_thumbnail_cache_clear();
    else if (invalidate_artists) {
        album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST);
        artist_thumbnail_alias_cache_clear();
    }
}

void library_thumbnails_clear_cache(void) { album_thumbnail_cache_clear(); }

void library_thumbnails_quiesce(void) {
    album_thumbnail_cache_epoch_advance();
    atomic_fetch_add(&album_thumbnail_view_generation, 1);
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    album_thumbnail_discard_boot_preload();
    prewarm_discard();
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumbnail_queue_count = 0;
    album_thumbnail_active_list = NULL;
    album_thumbnail_active_key_valid = false;
    if (atomic_load(&album_thumbnail_active)) {
        pthread_join(album_thumbnail_thread, NULL);
        atomic_store(&album_thumbnail_active, false);
    }
    atomic_store(&album_thumbnail_done, false);
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
}

void library_thumbnails_release_memory(void) { album_thumbnail_cache_clear(); }

void library_thumbnails_resize_alias_cache(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void)artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
}



/* A row may ask for its thumbnail when it intersects the viewport, partial
 * edge rows included. A compact list's geometry is known from its top offset,
 * row stride and row height, so a row that was only just re-bound is judged
 * correctly; lv_obj_get_y() would still report its previous position until the
 * next layout pass. Other lists keep reading the laid-out row. */
static bool thumbnail_decoration_retry_active;
static lv_obj_t * thumbnail_decoration_retry_list;
static lv_timer_t * thumbnail_decoration_retry_timer;

static void thumbnail_decoration_retry_cb(lv_timer_t * timer) {
    (void) timer; /* one shot: LVGL deletes it after this call */
    lv_obj_t * list = thumbnail_decoration_retry_list;
    thumbnail_decoration_retry_timer = NULL;
    thumbnail_decoration_retry_list = NULL;
    if (!list || !compact_list_is_live(list)) return;
    /* Once, after the opening layout: a list that still has no size is not
     * on screen yet and is decorated when it is shown. */
    thumbnail_decoration_retry_active = true;
    compact_list_refresh_decorations(list);
    thumbnail_decoration_retry_active = false;
}

static bool thumbnail_row_in_viewport(lv_obj_t * list, lv_obj_t * row, int logical_index) {
    int32_t scroll_y = lv_obj_get_scroll_y(list), top, bottom, viewport;
    if (compact_list_row_span(list, logical_index, &top, &bottom, &viewport)) {
        if (lv_obj_get_height(list) <= 0 && !thumbnail_decoration_retry_active && !thumbnail_decoration_retry_timer) {
            /* Bound before the first layout pass: decorate again once after it. */
            thumbnail_decoration_retry_list = list;
            thumbnail_decoration_retry_timer = lv_timer_create(thumbnail_decoration_retry_cb, 60, NULL);
            if (thumbnail_decoration_retry_timer) lv_timer_set_repeat_count(thumbnail_decoration_retry_timer, 1);
        }
        return bottom >= scroll_y && top <= scroll_y + viewport;
    }
    int32_t row_y = lv_obj_get_y(row);
    return row_y + lv_obj_get_height(row) >= scroll_y && row_y <= scroll_y + lv_obj_get_height(list);
}

/* `touch_row` is false for the album page's hero cover, which lays out its own
 * children and has no use for the compact-list padding. */
static void thumbnail_decorate_image(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                     int logical_index, int pool_slot, int64_t representative_song_id,
                                     uint64_t artwork_key, const char * artwork_name, void * ctx,
                                     bool touch_row) {
    (void) pool_slot;
    /* Every write below is skipped when the value is already in place: this
     * runs again for the rows of a list whenever any thumbnail lands, and an
     * unchanged write still invalidates and relays out. */
    /* Lists whose art kind changes (the genre browser) pass NULL and are
     * looked up per row. */
    library_thumbnail_context_t dynamic_context = ctx ? *(const library_thumbnail_context_t *)ctx : thumbnail_context_for_list(list);
    const library_thumbnail_context_t *context = &dynamic_context;
    if (context->key_kind == THUMBNAIL_KEY_ARTIST &&
        !atomic_load(&artist_thumbnail_work_enabled)) {
        if (touch_row) lv_idem_set_pad_left(row, LIST_ROW_LABEL_INSET);
        lv_idem_set_hidden(image, true);
        return;
    }
    /* Card inset + thumbnail + the same breathing room before the text. */
    if (touch_row) lv_idem_set_pad_left(row, BOARD_LIBRARY_THUMB_TEXT_X);
    if (representative_song_id <= 0 || artwork_key == 0) {
        lv_idem_image_set_src(image, asset_path("touch_list/list_default_album.png"));
        lv_idem_set_hidden(image, true);
        return;
    }
    album_thumbnail_cache_entry_t * cached = album_thumbnail_cache_find(context->key_kind, artwork_key);
    artist_thumbnail_alias_entry_t * alias = NULL;
    if ((!cached || !cached->pixels) && context->key_kind == THUMBNAIL_KEY_ARTIST)
        alias = artist_thumbnail_alias_find(artwork_key, context->artist_group_kind);
    /* Only the compact lists lay their rows out from known geometry and use the
     * pre-rendered placeholder; the group-song rows keep the stock picture. */
    bool compact = compact_list_is_live(list);
    if (alias && alias->has_album) {
        cached = album_thumbnail_cache_find(alias->album_key_kind, alias->album_key);
        if (cached && cached->pixels) {
            lv_idem_image_set_src(image, &cached->dsc);
            lv_idem_image_set_scale(image, LV_SCALE_NONE);
            if (compact) lv_obj_set_user_data(image, NULL);
            lv_idem_set_hidden(image, false);
            return;
        }
    }
    if (cached && cached->pixels) {
        lv_idem_image_set_src(image, &cached->dsc);
        lv_idem_image_set_scale(image, LV_SCALE_NONE);
        if (compact) lv_obj_set_user_data(image, NULL);
        lv_idem_set_hidden(image, false);
    } else {
        thumbnail_placeholder_t placeholder;
        if (compact && thumbnail_placeholder_get(&placeholder)) {
            /* The picture is already at its final size: no transform, and the
             * overhang keeps it where the scaled asset used to be drawn. */
            lv_idem_image_set_src(image, placeholder.dsc);
            lv_idem_image_set_scale(image, LV_SCALE_NONE);
            lv_obj_set_user_data(image, (void *) (intptr_t) placeholder.overhang);
        } else {
            const void * src = asset_path("touch_list/list_default_album.png");
            lv_idem_image_set_src(image, src);
            /* Scale from the asset's own width. A hardcoded size assumed the PNG
             * was already the thumbnail size; LV_IMAGE_ALIGN_STRETCH before set_src divided by
             * img->w=0 and killed the process during build_albums_screen(). */
            lv_image_header_t header;
            int32_t src_w = ALBUM_THUMBNAIL_PX;
            if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK && header.w > 0)
                src_w = header.w;
            lv_idem_image_set_scale(image, (ALBUM_THUMBNAIL_PX * LV_SCALE_NONE) / src_w);
            if (compact) lv_obj_set_user_data(image, NULL);
        }
        lv_idem_set_hidden(image, false);
        if (!thumbnail_row_in_viewport(list, row, logical_index)) return;
        if (alias) {
            if (alias->has_album && !cached)
                queue_thumbnail(list, logical_index, alias->album_key_kind, alias->album_key,
                                alias->representative_song_id, METADATA_DB_GROUP_ARTIST, NULL);
        } else if (!cached) {
            queue_thumbnail(list, logical_index, context->key_kind, artwork_key,
                            representative_song_id, context->artist_group_kind, artwork_name);
        }
    }
}

static void thumbnail_row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                    int logical_index, int pool_slot, int64_t representative_song_id,
                                    uint64_t artwork_key, const char * artwork_name, void * ctx) {
    thumbnail_decorate_image(list, row, image, logical_index, pool_slot, representative_song_id,
                             artwork_key, artwork_name, ctx, true);
}

/* Mosaic tiles on the artist page: the page owns a copy of the pixels, so an
 * LRU eviction can never leave it pointing at freed memory. */
bool library_thumbnails_copy_album(uint64_t artwork_key, uint16_t * dst, bool * out_no_art) {
    if (out_no_art) *out_no_art = false;
    album_thumbnail_cache_entry_t * cached = artwork_key ? album_thumbnail_cache_find(THUMBNAIL_KEY_ALBUM, artwork_key) : NULL;
    if (!cached) return false;
    if (!cached->pixels) { if (out_no_art) *out_no_art = true; return false; }
    memcpy(dst, cached->pixels, (size_t) ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2);
    return true;
}

void library_thumbnails_request_album(lv_obj_t * list, int64_t song_id, uint64_t artwork_key) {
    queue_thumbnail(list, 0, THUMBNAIL_KEY_ALBUM, artwork_key, song_id, METADATA_DB_GROUP_ARTIST, NULL);
}
