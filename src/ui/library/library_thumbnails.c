#include "library_thumbnails_internal.h"
#include "../widgets/lists/compact_list.h"
#include "../widgets/lists/lv_idempotent.h"
#include "library_thumbnail_placeholder.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../core/system/logging/db_log.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include <stdlib.h>
#include <string.h>

#define THUMB_DEFAULT_ASSET "touch_list/list_default_album.png"

static library_thumbnail_policy_cb_t thumb_policy_cb;
static library_thumbnail_genre_context_cb_t thumb_genre_context_cb;
static library_thumbnail_detach_group_cb_t thumb_detach_group_cb;
static library_thumbnail_refresh_group_cb_t thumb_refresh_group_cb;

bool thumbnail_policy(library_thumbnail_policy_t policy) {
    return thumb_policy_cb && thumb_policy_cb(policy);
}

#define THUMBNAIL_BINDING_CAPACITY 16
static thumbnail_binding_t thumbnail_bindings[THUMBNAIL_BINDING_CAPACITY];

lv_obj_t *album_thumbnail_active_list;
atomic_int album_thumbnail_view_generation;
atomic_int album_thumbnail_cache_epoch;
atomic_bool artist_thumbnail_work_enabled = true;
bool album_thumbnail_scrolling;
static uint64_t albums_page_open_requested_ms;

static const library_thumbnail_context_t album_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ALBUM, .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const library_thumbnail_context_t artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST, .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const library_thumbnail_context_t album_artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST, .artist_group_kind = METADATA_DB_GROUP_ALBUM_ARTIST
};

thumbnail_binding_t *thumbnail_binding_for_list(lv_obj_t *list) {
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

bool album_thumbnail_list_is_visible(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding || !thumbnail_role_visible_capability(binding->role)) return false;
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

void refresh_thumbnail_list_landed(lv_obj_t *list, uint64_t key,
                                          uint64_t artist_key, bool artist_request) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding) return;
    if (thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
        return;
    }
    library_thumbnail_context_t context = thumbnail_context_for_list(list);
    landed_thumbnail_t landed = {
        .key = key,
        .artist_key = artist_request ? artist_key : 0,
        .artist_list = context.key_kind == THUMBNAIL_KEY_ARTIST,
        .group_kind = context.artist_group_kind
    };
    compact_list_refresh_decorations_for(list, landed_thumbnail_matches, &landed);
}

void refresh_thumbnail_list_visible(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding) return;
    if (thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
    } else {
        compact_list_refresh_decorations(list);
    }
}

bool release_thumbnail_dsc(const lv_image_dsc_t *dsc) {
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

void refresh_all_thumbnail_lists(void) {
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
    album_thumbnail_lazy_init();
}

/* A row may ask for its thumbnail when it intersects the viewport, partial
 * edge rows included. A compact list's geometry is known from its top offset,
 * row stride and row height, so a row that was only just re-bound is judged
 * correctly; lv_obj_get_y() would still report its previous position until the
 * next layout pass. Other lists keep reading the laid-out row. */
static bool thumbnail_decoration_retry_active;
static lv_timer_t * thumbnail_decoration_retry_timer;
static lv_obj_t * thumbnail_decoration_retry_list;

/* Drops a pending retry, so a list freed (or a new one allocated at its address) is never decorated by it. */
static void thumbnail_decoration_retry_cancel(void) {
    if (thumbnail_decoration_retry_timer) lv_timer_delete(thumbnail_decoration_retry_timer);
    thumbnail_decoration_retry_timer = NULL;
    thumbnail_decoration_retry_list = NULL;
}

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

/* Shows `cover` in a row's leading image, or the "no cover yet" picture when it
 * is NULL. A compact list gets the pre-rendered placeholder and the shared
 * overhang; other lists keep the stock asset. Pads the row for the thumbnail
 * when `row` is not NULL. Writes are idempotent. */
void library_thumbnails_show_row_image(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                       const lv_image_dsc_t * cover) {
    /* Only the compact lists lay their rows out from known geometry and use the
     * pre-rendered placeholder; the group-song rows keep the stock picture. */
    bool compact = compact_list_is_live(list);
    if (row) lv_idem_set_pad_left(row, BOARD_LIBRARY_THUMB_TEXT_X);
    if (cover) {
        lv_idem_image_set_src(image, cover);
        lv_idem_image_set_scale(image, LV_SCALE_NONE);
        if (compact) lv_obj_set_user_data(image, NULL);
        lv_idem_set_hidden(image, false);
        return;
    }
    thumbnail_placeholder_t placeholder;
    if (compact && thumbnail_placeholder_get(&placeholder)) {
        /* The picture is already at its final size: no transform, and the
         * overhang keeps it where the scaled asset used to be drawn. */
        lv_idem_image_set_src(image, placeholder.dsc);
        lv_idem_image_set_scale(image, LV_SCALE_NONE);
        lv_obj_set_user_data(image, (void *) (intptr_t) placeholder.overhang);
    } else {
        const void * src = asset_path(THUMB_DEFAULT_ASSET);
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
}

/* `touch_row` is false for the album page's hero cover, which lays out its own
 * children and has no use for the compact-list padding. */
static void thumbnail_decorate_image(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                     int logical_index, int64_t representative_song_id,
                                     uint64_t artwork_key, const char * artwork_name, void * ctx,
                                     bool touch_row) {
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
        lv_idem_image_set_src(image, asset_path(THUMB_DEFAULT_ASSET));
        lv_idem_set_hidden(image, true);
        return;
    }
    album_thumbnail_cache_entry_t * cached = album_thumbnail_cache_find(context->key_kind, artwork_key);
    artist_thumbnail_alias_entry_t * alias = NULL;
    if ((!cached || !cached->pixels) && context->key_kind == THUMBNAIL_KEY_ARTIST)
        alias = artist_thumbnail_alias_find(artwork_key, context->artist_group_kind);
    if (alias && alias->has_album)
        cached = album_thumbnail_cache_find(THUMBNAIL_KEY_ALBUM, alias->album_key);
    if (cached && cached->pixels) {
        library_thumbnails_show_row_image(list, NULL, image, &cached->dsc);
    } else {
        library_thumbnails_show_row_image(list, NULL, image, NULL);
        if (!thumbnail_row_in_viewport(list, row, logical_index)) return;
        if (alias) {
            if (alias->has_album && !cached)
                queue_thumbnail(list, logical_index, THUMBNAIL_KEY_ALBUM, alias->album_key,
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
    (void) pool_slot; /* part of the compact-list decorator signature */
    thumbnail_decorate_image(list, row, image, logical_index, representative_song_id,
                             artwork_key, artwork_name, ctx, true);
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
    if (thumbnail_decoration_retry_list == list) thumbnail_decoration_retry_cancel();
    bool was_active = album_thumbnail_active_list == list;
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
        if (thumbnail_bindings[i].list == list) memset(&thumbnail_bindings[i], 0, sizeof(thumbnail_bindings[i]));
    album_thumbnail_queue_remove_if(queue_match_list, list);
    if (was_active) {
        album_thumbnail_active_list = NULL;
        atomic_fetch_add(&album_thumbnail_view_generation, 1);
    }
}

void library_thumbnails_group_rows_rebuilt(lv_obj_t *list) {
    if (list == album_thumbnail_active_list) {
        atomic_fetch_add(&album_thumbnail_view_generation, 1);
        album_thumbnail_queue_count = 0;
    }
}

/* The log name of a list's page; a missing binding and the genre roles log as the files search. */
static const char *thumbnail_role_page(const thumbnail_binding_t *binding) {
    if (!binding) return "files_search";
    switch (binding->role) {
    case LIBRARY_THUMB_LIST_ALBUMS: return "albums";
    case LIBRARY_THUMB_LIST_ARTIST_ALBUMS: return "artist_albums";
    case LIBRARY_THUMB_LIST_ARTISTS: return "artists";
    case LIBRARY_THUMB_LIST_ALBUM_ARTISTS: return "album_artists";
    case LIBRARY_THUMB_LIST_ALL_SONGS: return "all_songs";
    case LIBRARY_THUMB_LIST_RECENTLY_ADDED: return "recently_added";
    case LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM: return "group_songs";
    case LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH: return "group_songs_search";
    default: return "files_search";
    }
}

void library_thumbnails_begin_view(lv_obj_t *list) {
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    if (!binding) return;
    album_thumbnail_view_generation++;
    album_thumbnail_active_list = list;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    album_lazy_queued = album_lazy_completed = album_lazy_with_art = album_lazy_stale = 0;
    if (binding->role == LIBRARY_THUMB_LIST_ALBUMS ||
        binding->role == LIBRARY_THUMB_LIST_ARTIST_ALBUMS) {
        DB_LOG("ALBUMS_PAGE", "loaded page=%s request_to_loaded_ms=%llu rss_kb=%ld", thumbnail_role_page(binding),
               albums_page_open_requested_ms ? (unsigned long long)(db_log_now_ms() - albums_page_open_requested_ms) : 0ULL,
               db_log_rss_kb());
    } else {
        DB_LOG("ART_LIST_PAGE", "loaded page=%s rss_kb=%ld", thumbnail_role_page(binding), db_log_rss_kb());
    }
    albums_page_open_requested_ms = 0;
    if (thumbnail_role_is_custom(binding->role)) {
        if (thumb_refresh_group_cb) thumb_refresh_group_cb();
    } else {
        compact_list_refresh_on_show(list);
    }
}

void library_thumbnails_end_view(lv_obj_t *list) {
    if (album_thumbnail_active_list != list) return;
    thumbnail_binding_t *binding = thumbnail_binding_for_list(list);
    album_thumbnail_active_list = NULL;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    if (binding && (binding->role == LIBRARY_THUMB_LIST_ALBUMS ||
                    binding->role == LIBRARY_THUMB_LIST_ARTIST_ALBUMS))
        DB_LOG("ALBUMS_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               thumbnail_role_page(binding),
               album_lazy_queued, album_lazy_completed, album_lazy_with_art, album_lazy_stale, db_log_rss_kb());
    else
        DB_LOG("ART_LIST_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               thumbnail_role_page(binding), album_lazy_queued, album_lazy_completed, album_lazy_with_art,
               album_lazy_stale, db_log_rss_kb());
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
    thumbnail_decorate_image(list, row, image, 0, song_id, artwork_key, NULL,
                             (void *)&album_thumbnail_context, false);
}

void library_thumbnails_note_album_page_open(void) { albums_page_open_requested_ms = db_log_now_ms(); }

/* A request for an artist image, or one raised by an artist list. */
static bool queue_match_artist_work(const album_thumbnail_request_t * req, const void * ctx) {
    (void) ctx;
    const thumbnail_binding_t *binding = thumbnail_binding_for_list(req->list);
    return req->key_kind != THUMBNAIL_KEY_ALBUM ||
           (binding && (binding->role == LIBRARY_THUMB_LIST_ARTISTS ||
                        binding->role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS));
}

void library_thumbnails_set_artist_images_enabled(bool enabled) {
    atomic_store(&artist_thumbnail_work_enabled, enabled);
    if (!enabled) {
        album_thumbnail_queue_remove_if(queue_match_artist_work, NULL);
    }
    for (size_t i = 0; i < THUMBNAIL_BINDING_CAPACITY; ++i)
        if (thumbnail_bindings[i].list && (thumbnail_bindings[i].role == LIBRARY_THUMB_LIST_ARTISTS ||
                                           thumbnail_bindings[i].role == LIBRARY_THUMB_LIST_ALBUM_ARTISTS))
            compact_list_refresh_decorations(thumbnail_bindings[i].list);
}

void library_thumbnails_invalidate_album(uint64_t album_key) {
    album_thumbnail_cache_remove(THUMBNAIL_KEY_ALBUM, album_key);
    album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST);
    album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST_PHOTO);
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
        album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST_PHOTO);
        artist_thumbnail_alias_cache_clear();
    }
}

void library_thumbnails_clear_cache(void) { album_thumbnail_cache_clear(); }

/* UI reloads and database replacement both invalidate every list pointer
 * carried by the lazy artwork request. Stop the warmer first, then join the
 * single visible-row decoder before any screen or DB generation is torn
 * down. Both paths are cooperatively bounded by the artwork timeout. */
void library_thumbnails_quiesce(void) {
    album_thumbnail_cache_epoch_advance();
    atomic_fetch_add(&album_thumbnail_view_generation, 1);
    thumbnail_decoration_retry_cancel();
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    album_thumbnail_discard_boot_preload();
    prewarm_discard();
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumbnail_queue_count = 0;
    album_thumbnail_active_list = NULL;
    album_thumbnail_active_key_valid = false;
    album_thumbnail_lazy_quiesce();
}

void library_thumbnails_release_memory(void) { album_thumbnail_cache_clear(); }

void library_thumbnails_resize_alias_cache(void) {
    (void) artist_thumbnail_alias_cache_resize_for_library();
}
