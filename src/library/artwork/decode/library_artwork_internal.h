#ifndef LIBRARY_ARTWORK_INTERNAL_H
#define LIBRARY_ARTWORK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../albumart/albumart.h"
#include "../albumart/albumart_info.h"
#include "../coordinator/artwork_coordinator.h"
#include "../../metadata/db/metadata_db.h"

/* Shared sizing and input bounds used by artwork services and the UI cache. */
#define ALBUM_THUMBNAIL_PX ALBUMART_THUMBNAIL_SIZE
#define ALBUM_PLAYER_CACHE_PX ALBUMART_PLAYER_CACHE_SIZE
#define THUMBNAIL_SIDECAR_MAX_BYTES ALBUMART_SIDECAR_MAX_BYTES
/* Small startup allowance; the helper has a bounded address-space budget
 * and the returned picture is admitted at its actual compressed size. */
#define ALBUM_ART_METADATA_START_BYTES (1024U * 1024U)

typedef enum {
    THUMBNAIL_KEY_ALBUM,
    THUMBNAIL_KEY_ARTIST
} thumbnail_key_kind_t;

typedef enum {
    ARTIST_THUMB_FOUND,
    ARTIST_THUMB_MISSING,
    ARTIST_THUMB_TEMPORARY,
    ARTIST_THUMB_CANCELLED,
    ARTIST_THUMB_OWN_IMAGE
} artist_thumbnail_result_t;

typedef struct {
    uint16_t * pixels;
    thumbnail_key_kind_t pixel_key_kind;
    uint64_t pixel_key;
    int64_t pixel_song_id;
    time_t pixel_source_mtime;
    bool pixel_have_source_mtime;
    bool has_alias;
    uint64_t alias_album_key;
    int64_t alias_song_id;
    bool negative_confirmed;
} artist_thumbnail_resolution_t;

uint64_t song_album_thumbnail_key(const song_row_t * row);

bool album_thumbnail_sized_cache_hit(const albumart_info_t * info, char * found, size_t found_size);
bool album_player_cache_hit(const albumart_info_t * info, char * found, size_t found_size);
time_t album_source_mtime_with_art_mtime(const song_row_t * song, uint32_t art_mtime);
time_t album_source_mtime(const song_row_t * song, const albumart_info_t * info);
bool album_thumbnail_load_or_decode_with_mtime(const song_row_t * song, artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * user_data, time_t source_mtime,
        bool sized_cache_checked, const char * sidecar_path, bool sidecar_checked,
        uint16_t ** out_pixels);
/* The common case of album_thumbnail_load_or_decode_with_mtime() for a visible row, without the
 * source mtime it computes first: the song has no failure on record and its generated thumbnail
 * is on the card. False (nothing loaded) leaves everything to the full function. */
bool album_thumbnail_load_sized_quick(const song_row_t * song, artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels);
bool album_thumbnail_load_or_decode_ex(const song_row_t * song, artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels);
bool album_thumbnail_reload_persisted(const song_row_t * song);
void album_art_enable_player_cache_materialization(void);

artist_thumbnail_result_t artist_thumbnail_load_or_decode(
        int64_t representative_song_id, metadata_db_group_kind_t group_kind, const char * artist_name,
        artwork_priority_t prio, bool load_own_image, uint64_t artist_key,
        artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution);

#endif
