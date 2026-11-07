#include "library_thumbnails_internal.h"
#include "../assets/gui_image_dsc.h"
#include "../widgets/lists/compact_list.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/coordinator/artwork_failure_cache.h"
#include <stdlib.h>
#include <string.h>

album_thumbnail_cache_entry_t album_thumbnail_cache[ALBUM_THUMBNAIL_CACHE_SIZE];
static uint32_t album_thumbnail_use_counter;

/* Aliases are small lookup records, not decoded images, so they have their
 * own dynamically sized table (declared here) and do not consume RGB565
 * cache entries. */
artist_thumbnail_alias_entry_t * artist_thumbnail_alias_cache;
size_t artist_thumbnail_alias_cache_capacity;

/* True when the visible thumbnail list shows a row that a just-committed
 * entry can change: a row keyed by it, or an artist row whose alias borrows
 * that album. Independent of view generations, so a result staged before a
 * scroll still repaints the rows that are on screen now. */
bool thumbnail_active_list_shows(thumbnail_key_kind_t key_kind, uint64_t key) {
    lv_obj_t * list = album_thumbnail_active_list;
    if (!list || !album_thumbnail_list_is_visible(list)) return false;
    thumbnail_binding_t * binding = thumbnail_binding_for_list(list);
    if (binding && binding->role == LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM) return true;
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

/* The slot holding the key, or -1. Does not touch the LRU order. */
static int album_thumbnail_cache_slot(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++)
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache[i].key == key)
            return i;
    return -1;
}

album_thumbnail_cache_entry_t * album_thumbnail_cache_find(thumbnail_key_kind_t key_kind, uint64_t key) {
    int slot = album_thumbnail_cache_slot(key_kind, key);
    if (slot < 0) return NULL;
    album_thumbnail_cache[slot].last_use = ++album_thumbnail_use_counter;
    return &album_thumbnail_cache[slot];
}

bool artist_thumbnail_alias_cache_resize(int artist_count, int album_artist_count) {
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

bool artist_thumbnail_alias_cache_resize_for_library(void) {
    int artist_count = 0, album_artist_count = 0, unused = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &unused);
    return artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
}

/* The slot holding the artist, or SIZE_MAX. Does not touch the LRU order. */
static size_t artist_thumbnail_alias_slot(uint64_t artist_key, metadata_db_group_kind_t artist_group_kind) {
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++)
        if (artist_thumbnail_alias_cache[i].known &&
            artist_thumbnail_alias_cache[i].artist_key == artist_key &&
            artist_thumbnail_alias_cache[i].artist_group_kind == artist_group_kind)
            return i;
    return SIZE_MAX;
}

artist_thumbnail_alias_entry_t * artist_thumbnail_alias_find(
        uint64_t artist_key, metadata_db_group_kind_t artist_group_kind) {
    size_t slot = artist_thumbnail_alias_slot(artist_key, artist_group_kind);
    if (slot == SIZE_MAX) return NULL;
    artist_thumbnail_alias_cache[slot].last_use = ++album_thumbnail_use_counter;
    return &artist_thumbnail_alias_cache[slot];
}

/* Returns false when nothing was stored (no table could be allocated); callers keep the request then. */
bool artist_thumbnail_alias_cache_commit(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind,
                                                 bool has_album,
                                                 uint64_t album_key, int64_t representative_song_id) {
    if (!artist_thumbnail_alias_cache_capacity &&
        (!artist_thumbnail_alias_cache_resize_for_library() || !artist_thumbnail_alias_cache_capacity))
        return false;
    size_t victim = artist_thumbnail_alias_slot(artist_key, artist_group_kind);
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
    if (victim == SIZE_MAX) return false;
    artist_thumbnail_alias_cache[victim] = (artist_thumbnail_alias_entry_t) {
        .artist_key = artist_key,
        .artist_group_kind = artist_group_kind,
        .album_key = album_key,
        .representative_song_id = representative_song_id,
        .known = true,
        .has_album = has_album,
        .last_use = ++album_thumbnail_use_counter
    };
    return true;
}

/* After an artist's alias / no-art outcome was stored: its own image is superseded by the alias and
 * its queued requests are redundant. One order for the boot, prewarm and lazy commits. */
void artist_thumbnail_alias_settled(uint64_t artist_key, metadata_db_group_kind_t artist_group_kind,
                                    bool has_album) {
    if (has_album) album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST, artist_key);
    album_thumbnail_queue_remove_artist(artist_key, artist_group_kind);
}

void artist_thumbnail_alias_cache_clear(void) {
    free(artist_thumbnail_alias_cache);
    artist_thumbnail_alias_cache = NULL;
    artist_thumbnail_alias_cache_capacity = 0;
}

void artist_thumbnail_alias_cache_remove(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind) {
    artist_thumbnail_alias_entry_t * entry = artist_thumbnail_alias_find(artist_key, artist_group_kind);
    if (entry) memset(entry, 0, sizeof(*entry));
}

/* Returns true when a row of the visible list lost this entry's image; the
 * caller re-decorates only after the slot is fully cleared or replaced, so
 * the decorator never sees a half-evicted "known, no art" entry.
 * A leading lv_image can retain &e->dsc after its row last ran the decorator.
 * Freeing pixels first made that image descriptor point into released heap
 * memory until the row happened to be recycled: a redraw/scroll use-after-free
 * on libraries larger than the LRU. So the pixels are cleared and every
 * visible reference detached before they are released and the slot reused. */
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

void album_thumbnail_cache_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    int slot = album_thumbnail_cache_slot(key_kind, key);
    if (slot >= 0) album_thumbnail_cache_evict(&album_thumbnail_cache[slot]);
}

void album_thumbnail_cache_remove_kind(thumbnail_key_kind_t key_kind) {
    bool visible_lost = false;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++)
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache_evict_entry(&album_thumbnail_cache[i]))
            visible_lost = true;
    if (visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

void album_thumbnail_cache_clear(void) {
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

static uint32_t album_thumbnail_commit_counter;

/* Pins are valid only for the cache epoch they were set in (only cache clear and invalidation reset entries;
 * quiesce, publication-view changes and storage invalidation advance the epoch without clearing them). */
int first_screen_pin_epoch = -1;

void album_thumbnail_pin(thumbnail_key_kind_t key_kind, uint64_t key) {
    int slot = album_thumbnail_cache_slot(key_kind, key);
    if (slot >= 0 && album_thumbnail_cache[slot].pixels) album_thumbnail_cache[slot].pinned = true;
}

void album_thumbnail_unpin_all(void) {
    first_screen_pin_epoch = -1;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) album_thumbnail_cache[i].pinned = false;
}

uint32_t library_thumbnails_commit_count(void) { return album_thumbnail_commit_counter; }

/* Evicts the LRU slot (or the first unused one) and installs `pixels` (or
 * a durable "known, no art" marker for a PERMANENT failure) into it.
 * Shared by the on-screen lazy-decode commit and the boot-time RAM
 * preload hand-off -- both only ever run on the main/LVGL
 * thread, the only thread allowed to touch album_thumbnail_cache[].
 * Takes ownership of `pixels`: either stores it or frees it, never both. */
void album_thumbnail_cache_commit(thumbnail_key_kind_t key_kind, uint64_t key,
                                         int64_t source_song_id, uint8_t * pixels,
                                         bool have_source_mtime, time_t source_mtime) {
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
        bool commit_known = pixels != NULL || (key_kind == THUMBNAIL_KEY_ARTIST_PHOTO && have_source_mtime);
        if (!commit_known && key_kind == THUMBNAIL_KEY_ALBUM && have_source_mtime) {
            artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(source_song_id, source_mtime, &fail_reason) &&
                fail_reason == ARTWORK_FAIL_PERMANENT) {
                commit_known = true;
            }
        }
        /* An interrupted-then-restarted boot preload pass can reach the same
         * song_id twice (it always restarts at offset 0). Without this check
         * that would waste a second slot on a duplicate instead of just
         * refreshing the one that's already there. The on-screen lazy-decode
         * path never hits this: queue_thumbnail() already skips
         * queuing a song album_thumbnail_cache_find() reports as known. A
         * full pass for the match has to run before the unused/LRU pass
         * below, which stops at the first unused slot -- a duplicate further
         * down the array would otherwise never be seen. */
        int victim = album_thumbnail_cache_slot(key_kind, key);
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
        bool victim_visible_lost = album_thumbnail_cache_evict_entry(e);
        if (commit_known) {
            e->key = key;
            e->key_kind = key_kind;
            e->known = true;
            e->pinned = was_pinned; /* a same-key re-commit keeps its pin */
            e->pixels = pixels;
            e->last_use = ++album_thumbnail_use_counter;
            if (e->pixels) {
                gui_image_dsc_init_rgb565(&e->dsc, e->pixels, ALBUM_THUMBNAIL_PX);
            }
        } else {
            free(pixels);
        }
        album_thumbnail_queue_remove(key_kind, key);
        /* Only now is the victim slot fully replaced. */
        if (victim_visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
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

bool library_thumbnails_copy_artist_photo(uint64_t key, uint16_t * dst, bool * no_art) {
    if (no_art) *no_art = false;
    album_thumbnail_cache_entry_t * entry = key ? album_thumbnail_cache_find(THUMBNAIL_KEY_ARTIST_PHOTO, key) : NULL;
    if (!entry) return false;
    if (!entry->pixels) { if (no_art) *no_art = true; return false; }
    memcpy(dst, entry->pixels, (size_t) ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2);
    return true;
}
