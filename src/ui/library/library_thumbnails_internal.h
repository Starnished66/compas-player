#pragma once

#include "library_thumbnails.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Shared between the library_thumbnails*.c files only. Which file owns each piece of state and each function is noted per group. */

#define ALBUM_THUMB_GEN_BATCH 16
#define ALBUM_THUMB_GEN_INTER_ALBUM_US 100000
#define ALBUM_COVER_DECODE_THREAD_STACK_SIZE (4 * 1024 * 1024)

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    library_thumbnail_list_role_t role;
} thumbnail_binding_t;

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

/* One decoded result on its way to the UI thread (the staged boot-preload result, the lazy decoder's
 * result, a prewarm row). `kind` says whether it answers an album or an artist request. */
enum { ALBUM_BOOT_RESULT_ALBUM, ALBUM_BOOT_RESULT_ARTIST };

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

/* Visible-first prewarm (see library_thumbnails_prewarm_artists): one thread decodes the first screen
 * of an artist list from the Music-menu press on, staging results for the UI thread to commit. While
 * it runs the ordinary one-at-a-time decoder waits, so two decoders never run together.
 * The job is a heap record shared by the UI thread and the worker like the library's other abandonable
 * reads (gui_library_async.h): teardown cancels it, detaches the thread and lets the worker free it, so a
 * stalled card read can never block the UI thread; the shared orphan wait bounds the rest. */
#define PREWARM_MAX 12

typedef struct prewarm_job prewarm_job_t;

/* Aliases already resolved in RAM when this warmer run started (the
 * alias table itself is UI-thread only): a restarted run skips them. */
typedef struct { uint64_t key; metadata_db_group_kind_t kind; bool has_album; } album_thumb_gen_alias_key_t;

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

/* State owned by library_thumbnails.c */
extern lv_obj_t *album_thumbnail_active_list;
extern atomic_int album_thumbnail_view_generation;
extern atomic_int album_thumbnail_cache_epoch;
extern atomic_bool artist_thumbnail_work_enabled;
extern bool album_thumbnail_scrolling;

/* State owned by library_thumbnails_cache.c */
extern album_thumbnail_cache_entry_t album_thumbnail_cache[ALBUM_THUMBNAIL_CACHE_SIZE];
extern artist_thumbnail_alias_entry_t * artist_thumbnail_alias_cache;
extern size_t artist_thumbnail_alias_cache_capacity;
extern int first_screen_pin_epoch;

/* State owned by library_thumbnails_lazy.c */
extern atomic_int album_thumbnail_queue_count;
extern unsigned album_lazy_queued;
extern unsigned album_lazy_completed;
extern unsigned album_lazy_with_art;
extern unsigned album_lazy_stale;
extern atomic_bool album_thumbnail_active;
extern bool album_thumbnail_active_key_valid;

/* State owned by library_thumbnails_warmer.c */
extern int album_thumb_gen_ram_filled;
extern uint64_t album_thumb_gen_cached_keys[ALBUM_THUMBNAIL_CACHE_SIZE];
extern thumbnail_key_kind_t album_thumb_gen_cached_key_kinds[ALBUM_THUMBNAIL_CACHE_SIZE];
extern int album_thumb_gen_cached_key_count;
extern album_thumb_gen_alias_key_t * album_thumb_gen_known_aliases;
extern size_t album_thumb_gen_known_alias_count;
extern bool thumbnail_cover_thread_joinable;
extern atomic_bool album_thumb_gen_active;
extern atomic_int album_thumb_gen_done_count;
extern atomic_int album_thumb_gen_cache_epoch;
extern atomic_bool album_thumb_gen_retry_pending;

/* State owned by library_thumbnails_warm_artists.c */
extern atomic_int album_boot_preload_state;
extern atomic_int album_thumb_gen_screens_epoch;
extern int album_thumb_gen_warm_waited_epoch;

/* State owned by library_thumbnails_prewarm.c */
extern prewarm_job_t * prewarm_job;
extern atomic_bool prewarm_active;
extern int prewarm_done_epoch[2];
extern int prewarm_done_first[2];
extern int prewarm_keys_count[2];
extern bool prewarm_boot_pending[2];
extern uint64_t prewarm_keys[2][PREWARM_MAX];
extern atomic_bool prewarm_waiting;

/* Defined in library_thumbnails.c */
bool thumbnail_policy(library_thumbnail_policy_t policy);
thumbnail_binding_t *thumbnail_binding_for_list(lv_obj_t *list);
bool album_thumbnail_list_is_visible(lv_obj_t *list);
void refresh_thumbnail_list_landed(lv_obj_t *list, uint64_t key, uint64_t artist_key, bool artist_request);
void refresh_thumbnail_list_visible(lv_obj_t *list);
bool release_thumbnail_dsc(const lv_image_dsc_t *dsc);
void refresh_all_thumbnail_lists(void);

/* Defined in library_thumbnails_cache.c */
bool thumbnail_active_list_shows(thumbnail_key_kind_t key_kind, uint64_t key);
album_thumbnail_cache_entry_t * album_thumbnail_cache_find(thumbnail_key_kind_t key_kind, uint64_t key);
bool artist_thumbnail_alias_cache_resize(int artist_count, int album_artist_count);
bool artist_thumbnail_alias_cache_resize_for_library(void);
artist_thumbnail_alias_entry_t * artist_thumbnail_alias_find( uint64_t artist_key, metadata_db_group_kind_t artist_group_kind);
bool artist_thumbnail_alias_cache_commit(uint64_t artist_key, metadata_db_group_kind_t artist_group_kind, bool has_album, uint64_t album_key, int64_t representative_song_id);
void artist_thumbnail_alias_cache_clear(void);
void artist_thumbnail_alias_settled(uint64_t artist_key, metadata_db_group_kind_t artist_group_kind, bool has_album);
void artist_thumbnail_alias_cache_remove(uint64_t artist_key, metadata_db_group_kind_t artist_group_kind);
void album_thumbnail_cache_remove(thumbnail_key_kind_t key_kind, uint64_t key);
void album_thumbnail_cache_remove_kind(thumbnail_key_kind_t key_kind);
void album_thumbnail_cache_clear(void);
void album_thumbnail_pin(thumbnail_key_kind_t key_kind, uint64_t key);
void album_thumbnail_unpin_all(void);
void album_thumbnail_cache_commit(thumbnail_key_kind_t key_kind, uint64_t key, int64_t source_song_id, uint8_t * pixels, bool have_source_mtime, time_t source_mtime);

/* Defined in library_thumbnails_lazy.c */
void thumbnail_poll_resume(void);
void album_thumbnail_lazy_init(void);
void album_thumbnail_lazy_quiesce(void);
bool album_thumbnail_lazy_work_pending(void);
void album_thumbnail_queue_remove_if(bool (*match)(const album_thumbnail_request_t *, const void *), const void * ctx);
bool queue_match_list(const album_thumbnail_request_t * req, const void * ctx);
void album_thumbnail_queue_remove(thumbnail_key_kind_t key_kind, uint64_t key);
void album_thumbnail_queue_remove_artist(uint64_t key, metadata_db_group_kind_t group_kind);
void queue_thumbnail(lv_obj_t * list, int logical_index, thumbnail_key_kind_t key_kind, uint64_t key, int64_t representative_song_id, metadata_db_group_kind_t artist_group_kind, const char * artist_name);
void album_thumbnail_poll_cb(lv_timer_t * timer);

/* Defined in library_thumbnails_warmer.c */
bool album_thumb_gen_cached_key(thumbnail_key_kind_t key_kind, uint64_t key);
bool album_thumb_gen_should_cancel(int my_generation);
void cancel_album_thumbnail_generation(void);
void reap_album_thumbnail_generation(void);
bool album_thumb_gen_cancel_cb(void * user_data);

/* Defined in library_thumbnails_warm_artists.c */
bool album_thumbnail_commit_boot_preload(void);
void album_thumbnail_discard_boot_preload(void);
album_thumbnail_handoff_t artist_handoff(int cache_epoch, uint64_t artist_key, metadata_db_group_kind_t group_kind, int64_t first_song_id, const artist_thumbnail_resolution_t * resolution, bool first_screen);
bool album_thumb_gen_handoff_result(int my_generation, const album_thumbnail_handoff_t * result);
bool album_thumb_gen_warm_artist_aliases(int my_generation);
bool album_thumb_gen_warm_first_screens(int my_generation);

/* Defined in library_thumbnails_prewarm.c */
bool prewarm_poll(void);
void prewarm_discard(void);
void prewarm_start_waiting(void);

extern void install_thread_crash_altstack(void);

static inline void album_thumbnail_cache_epoch_advance(void) {
    atomic_fetch_add(&album_thumbnail_cache_epoch, 1);
}
