#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "library_artwork_internal.h"
#include "metadata_db.h"

typedef enum {
    LIBRARY_THUMB_LIST_ALBUMS,
    LIBRARY_THUMB_LIST_ARTIST_ALBUMS,
    LIBRARY_THUMB_LIST_ARTISTS,
    LIBRARY_THUMB_LIST_ALBUM_ARTISTS,
    LIBRARY_THUMB_LIST_ALL_SONGS,
    LIBRARY_THUMB_LIST_RECENTLY_ADDED,
    LIBRARY_THUMB_LIST_FILES_SEARCH,
    LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH,
    LIBRARY_THUMB_LIST_GENRE_GROUPS,
    LIBRARY_THUMB_LIST_GENRE_ALBUMS,
    LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM
} library_thumbnail_list_role_t;

typedef enum {
    LIBRARY_THUMB_POLICY_LAZY,
    LIBRARY_THUMB_POLICY_WARMUP_RETRY
} library_thumbnail_policy_t;

typedef struct {
    thumbnail_key_kind_t key_kind;
    metadata_db_group_kind_t artist_group_kind;
} library_thumbnail_context_t;

typedef enum {
    LIBRARY_THUMB_COVER_ALL,
    LIBRARY_THUMB_COVER_ALBUM
} library_thumbnail_cover_scope_t;

typedef struct {
    library_thumbnail_cover_scope_t scope;
    int64_t target_song_id;
} library_thumbnail_cover_request_t;

typedef struct {
    bool lazy_active;
    bool warmup_active;
    bool cover_active;
    bool cover_done;
    bool cover_cancelled;
    int cover_done_count;
    int cover_total_count;
    int cover_error_count;
    int decoded_cache_entries; /* Diagnostics only; zero without UI_PERF_TRACE. */
} library_thumbnail_status_t;

typedef bool (*library_thumbnail_policy_cb_t)(library_thumbnail_policy_t policy);
typedef library_thumbnail_context_t (*library_thumbnail_genre_context_cb_t)(void);
typedef bool (*library_thumbnail_detach_group_cb_t)(const lv_image_dsc_t *descriptor);
typedef void (*library_thumbnail_refresh_group_cb_t)(void);
typedef void (*library_thumbnail_show_busy_cb_t)(library_thumbnail_cover_scope_t scope);

/* All public methods and callbacks run on the LVGL thread unless explicitly
 * noted. Callbacks are synchronous. List/screen/overlay pointers are borrowed
 * and must be registered before use. forget_list() is safe after deletion:
 * it compares pointer identities only and never dereferences the object. */
void library_thumbnails_init(bool artist_images_enabled,
                             library_thumbnail_policy_cb_t policy,
                             library_thumbnail_genre_context_cb_t genre_context,
                             library_thumbnail_detach_group_cb_t detach_group,
                             library_thumbnail_refresh_group_cb_t refresh_group);
void library_thumbnails_bind_list(lv_obj_t *screen, lv_obj_t *list,
                                  library_thumbnail_list_role_t role);
void library_thumbnails_forget_list(lv_obj_t *list);
void library_thumbnails_decorate_group_row(lv_obj_t *list, lv_obj_t *row,
                                           lv_obj_t *image, int logical_index,
                                           int pool_slot, int64_t song_id,
                                           uint64_t artwork_key);
void library_thumbnails_begin_view(lv_obj_t *list);
void library_thumbnails_end_view(lv_obj_t *list);
void library_thumbnails_group_rows_rebuilt(lv_obj_t *list);
void library_thumbnails_start_warmup(void);
void library_thumbnails_retry_warmup(void);
void library_thumbnails_cancel_warmup(void);
void library_thumbnails_invalidate_storage(void);
void library_thumbnails_note_album_page_open(void);
bool library_thumbnails_start_cover_reload(const library_thumbnail_cover_request_t *request,
                                           library_thumbnail_show_busy_cb_t show_busy);
/* Cancel then join without publishing the normal cover result. */
void library_thumbnails_cancel_cover_reload(void);
/* Abort-only join used before UI deletion; does not commit or refresh. */
void library_thumbnails_join_cover_abort(void);
/* Normal completion: caller first clears library_rescan_active; this joins,
 * consumes the handoff, and refreshes registered lists. */
void library_thumbnails_finish_cover_reload(void);
void library_thumbnails_get_status(library_thumbnail_status_t *out);
void library_thumbnails_set_artist_images_enabled(bool enabled);
void library_thumbnails_invalidate_album(uint64_t album_key);
void library_thumbnails_invalidate_publication_view(void);
void library_thumbnails_invalidate_publication(bool clear_all, bool invalidate_artists);
void library_thumbnails_resize_alias_cache(void);
void library_thumbnails_clear_cache(void);
/* Invalidates/cancels thumbnail work, joins the lazy decoder and ordinary
 * warmer, discards (does not commit) preload, and retains cache contents.
 * Cover-runner abort/finish joins are explicit. Cache clearing is separate. */
void library_thumbnails_quiesce(void);
void library_thumbnails_release_memory(void);
