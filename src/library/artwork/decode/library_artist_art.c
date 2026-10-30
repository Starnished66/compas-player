#include "library_artwork_internal.h"

#include "../albumart/albumart.h"
#include "cover_decode.h"
#include "../../metadata/parsing/metadata.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

static bool thumbnail_directory_of(const char * path, char * out, size_t out_size) {
    if (!path || !path[0] || !out || out_size == 0) return false;
    snprintf(out, out_size, "%s", path);
    char * slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static bool thumbnail_parent_directory(const char * directory, char * out, size_t out_size) {
    if (!directory || !directory[0] || !out || out_size == 0) return false;
    snprintf(out, out_size, "%s", directory);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    char * slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static const char * thumbnail_directory_name(const char * path) {
    const char * slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static bool artist_thumbnail_folder_for_song(const song_row_t * song, const char * artist,
                                              char * artist_dir, size_t artist_dir_size,
                                              char * album_dir, size_t album_dir_size) {
    char track_dir[PATH_MAX];
    if (!song || !thumbnail_directory_of(song->path, track_dir, sizeof(track_dir))) return false;
    snprintf(album_dir, album_dir_size, "%s", track_dir);
    if (albumart_is_disc_folder(thumbnail_directory_name(track_dir))) {
        if (!thumbnail_parent_directory(track_dir, album_dir, album_dir_size)) return false;
    }
    if (!thumbnail_parent_directory(album_dir, artist_dir, artist_dir_size)) return false;

    /* A library with tracks directly in each artist directory has no album
     * subdirectory to step through. Accept that layout only when the folder
     * itself matches the artist tag and its parent is the music root. */
    if (!strcmp(artist_dir, MUSIC_ROOT_DIR) && artist[0] &&
        strcasecmp(thumbnail_directory_name(album_dir), artist) == 0) {
        snprintf(artist_dir, artist_dir_size, "%s", album_dir);
        album_dir[0] = '\0';
    }

    if (!artist_dir[0] || strcmp(artist_dir, MUSIC_ROOT_DIR) == 0 ||
        (album_dir[0] && strcmp(artist_dir, album_dir) == 0))
        return false;
    struct stat st;
    return stat(artist_dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t artist_directory_signature(const char * artist, metadata_db_group_kind_t group_kind,
                                           const song_row_t * representative,
                                           const char * artist_dir) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char * name = (const unsigned char *) (artist ? artist : "");
    while (*name) hash = (hash ^ *name++) * UINT64_C(1099511628211);
    hash = (hash ^ (unsigned char) group_kind) * UINT64_C(1099511628211);
    const char * paths[] = { artist_dir, representative ? representative->path : NULL };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        const char * path = paths[i];
        if (!path || !path[0]) continue;
        for (const unsigned char * p = (const unsigned char *) path; *p; p++)
            hash = (hash ^ *p) * UINT64_C(1099511628211);
        struct stat st;
        uint64_t mtime = stat(path, &st) == 0 ? (uint64_t) st.st_mtime : 0;
        for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (mtime >> (b * 8))) * UINT64_C(1099511628211);
    }

    int64_t album_count = metadata_db_count_albums_for_group(group_kind, artist);
    int album_total = album_count > INT_MAX ? INT_MAX : (int) album_count;
    group_row_t albums[16];
    for (int offset = 0; offset < album_total; ) {
        int want = album_total - offset;
        if (want > 16) want = 16;
        int got = metadata_db_get_albums_for_group(group_kind, artist, offset, want, albums);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            song_row_t song;
            if (!metadata_db_get_song_by_id(albums[i].first_song_id, &song)) continue;
            for (const unsigned char * p = (const unsigned char *) song.path; *p; p++)
                hash = (hash ^ *p) * UINT64_C(1099511628211);
            struct stat st;
            uint64_t mtime = stat(song.path, &st) == 0 ? (uint64_t) st.st_mtime : 0;
            for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (mtime >> (b * 8))) * UINT64_C(1099511628211);
            char directory[PATH_MAX], album_directory[PATH_MAX];
            if (thumbnail_directory_of(song.path, directory, sizeof(directory))) {
                struct stat dir_st;
                uint64_t dir_mtime = stat(directory, &dir_st) == 0 ? (uint64_t) dir_st.st_mtime : 0;
                for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (dir_mtime >> (b * 8))) * UINT64_C(1099511628211);
                if (albumart_is_disc_folder(thumbnail_directory_name(directory)) &&
                    thumbnail_parent_directory(directory, album_directory, sizeof(album_directory)) &&
                    stat(album_directory, &dir_st) == 0) {
                    dir_mtime = (uint64_t) dir_st.st_mtime;
                    for (int b = 0; b < 8; b++)
                        hash = (hash ^ (unsigned char) (dir_mtime >> (b * 8))) * UINT64_C(1099511628211);
                }
            }
        }
        offset += got;
        if (got < want) break;
    }
    return hash ? hash : 1;
}

static bool artist_thumbnail_load_persistent(const char * artist, uint32_t source_mtime,
                                             artwork_priority_t prio,
                                             artwork_cancel_fn cancel_cb, void * user_data,
                                             uint16_t ** out_pixels, bool * out_temporary) {
    *out_temporary = false;
    char found[PATH_MAX];
    if (!albumart_artist_sized_thumb_fresh(artist, source_mtime, ALBUM_THUMBNAIL_PX,
                                           ALBUM_THUMBNAIL_PX, found, sizeof(found)))
        return false;
    uint8_t * data = NULL;
    uint32_t size = 0;
    albumart_load_result_t load = albumart_load_file_ex(found, &data, &size,
                                                         THUMBNAIL_SIDECAR_MAX_BYTES,
                                                         prio);
    if (load == ALBUMART_LOAD_TEMPORARY) {
        *out_temporary = true;
        return false;
    }
    if (load != ALBUMART_LOAD_OK) {
        unlink(found);
        return false;
    }
    cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX,
                                                           ALBUM_THUMBNAIL_PX,
                                                           prio,
                                                           cancel_cb, user_data, out_pixels);
    free(data);
    if (res == COVER_DECODE_OK && *out_pixels) return true;
    if (res == COVER_DECODE_FAIL_CANCELLED) return false;
    if (cover_decode_result_is_temporary(res)) *out_temporary = true;
    else unlink(found);
    return false;
}

static void artist_thumbnail_set_alias(uint64_t album_key, int64_t song_id,
                                       artist_thumbnail_resolution_t * resolution) {
    resolution->has_alias = true;
    resolution->alias_album_key = album_key;
    resolution->alias_song_id = song_id;
}

/* A visible row (or the first-screen warm): the album's thumbnail is already on the card, so load
 * it now instead of leaving it to a second job queued behind every other row's. */
static void artist_thumbnail_load_alias_pixels(const char * found, uint64_t album_key, int64_t song_id,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution) {
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio) != ALBUMART_LOAD_OK)
        return;
    uint16_t * pixels = NULL;
    cover_decode_result_t decoded = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX,
            ALBUM_THUMBNAIL_PX, prio, cancel_cb, cancel_user_data, &pixels);
    free(data);
    if (decoded == COVER_DECODE_OK && pixels) {
        resolution->pixels = pixels;
        resolution->pixel_key_kind = THUMBNAIL_KEY_ALBUM;
        resolution->pixel_key = album_key;
        resolution->pixel_song_id = song_id;
        artwork_failure_cache_note_success(song_id);
    } else {
        free(pixels);
    }
}

static artist_thumbnail_result_t artist_thumbnail_from_album_group(
        const char * artist, metadata_db_group_kind_t group_kind,
        artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution) {
    int64_t album_count = metadata_db_count_albums_for_group(group_kind, artist);
    int total = album_count > INT_MAX ? INT_MAX : (int) album_count;
    group_row_t albums[16];
    for (int offset = 0; offset < total; ) {
        if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
        int want = total - offset;
        if (want > 16) want = 16;
        int n = metadata_db_get_albums_for_group(group_kind, artist, offset, want, albums);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
            song_row_t song;
            if (!metadata_db_get_song_by_id(albums[i].first_song_id, &song)) continue;
            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);
            uint64_t album_key = song_album_thumbnail_key(&song);
            char sidecar_path[PATH_MAX];
            bool have_sidecar = false;
            uint32_t thumb_source_mtime = albumart_source_mtime_with_path(&info, sidecar_path,
                                                 sizeof(sidecar_path), &have_sidecar);
            time_t failure_mtime = album_source_mtime_with_art_mtime(&song, thumb_source_mtime);
            char cache_path[PATH_MAX];
            if (albumart_sized_thumb_fresh_with_source_mtime(&info, ALBUM_THUMBNAIL_PX,
                    ALBUM_THUMBNAIL_PX, thumb_source_mtime, cache_path, sizeof(cache_path))) {
                artist_thumbnail_set_alias(album_key, song.id, resolution);
                albumart_artist_alias_store(artist, (unsigned int) group_kind, album_key, song.id);
                if (prio == ARTWORK_PRIO_THUMBNAIL)
                    artist_thumbnail_load_alias_pixels(cache_path, album_key, song.id, prio,
                                                       cancel_cb, cancel_user_data, resolution);
                return ARTIST_THUMB_FOUND;
            }

            artwork_fail_reason_t reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(song.id, failure_mtime, &reason) &&
                reason == ARTWORK_FAIL_PERMANENT)
                continue;

            uint16_t * pixels = NULL;
            bool found = album_thumbnail_load_or_decode_with_mtime(&song, prio,
                            cancel_cb, cancel_user_data,
                            failure_mtime, true, sidecar_path, true, &pixels);
            if (!found || !pixels) {
                free(pixels);
                if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
                if (artwork_failure_cache_is_blocked(song.id, failure_mtime, &reason) &&
                    reason == ARTWORK_FAIL_PERMANENT)
                    continue;
                return ARTIST_THUMB_TEMPORARY;
            }
            artwork_failure_cache_note_success(song.id);
            artist_thumbnail_set_alias(album_key, song.id, resolution);
            if (albumart_sized_thumb_fresh_with_source_mtime(&info, ALBUM_THUMBNAIL_PX,
                    ALBUM_THUMBNAIL_PX, thumb_source_mtime, cache_path, sizeof(cache_path)))
                albumart_artist_alias_store(artist, (unsigned int) group_kind, album_key, song.id);
            resolution->pixels = pixels;
            resolution->pixel_key_kind = THUMBNAIL_KEY_ALBUM;
            resolution->pixel_key = album_key;
            resolution->pixel_song_id = song.id;
            return ARTIST_THUMB_FOUND;
        }
        offset += n;
        if (n < want) break;
    }
    return ARTIST_THUMB_MISSING;
}

artist_thumbnail_result_t artist_thumbnail_load_or_decode(
        int64_t representative_song_id, metadata_db_group_kind_t group_kind, const char * artist_name,
        artwork_priority_t prio, bool load_own_image,
        uint64_t artist_key, artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution) {
    memset(resolution, 0, sizeof(*resolution));
    resolution->pixel_key_kind = THUMBNAIL_KEY_ARTIST;
    resolution->pixel_key = artist_key;
    song_row_t representative;
    if (!metadata_db_get_song_by_id(representative_song_id, &representative)) return ARTIST_THUMB_TEMPORARY;
    const char * artist = artist_name && artist_name[0] ? artist_name :
                          group_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                              ? representative.tags.album_artist : representative.tags.artist;
    if (!artist || !artist[0]) return ARTIST_THUMB_MISSING;

    char artist_dir[PATH_MAX] = "", album_dir[PATH_MAX] = "";
    bool have_artist_dir = artist_thumbnail_folder_for_song(&representative, artist,
                            artist_dir, sizeof(artist_dir), album_dir, sizeof(album_dir));
    char source_path[PATH_MAX];
    bool have_artist_source = have_artist_dir &&
        albumart_find_artist_sidecar(artist_dir, source_path, sizeof(source_path));
    if (have_artist_source && !load_own_image) return ARTIST_THUMB_OWN_IMAGE;
    if (have_artist_source) {
        struct stat source_st;
        uint32_t source_mtime = stat(source_path, &source_st) == 0
                                    ? (uint32_t) source_st.st_mtime : 0;
        bool cache_temporary = false;
        if (artist_thumbnail_load_persistent(artist, source_mtime, prio,
                cancel_cb, cancel_user_data,
                &resolution->pixels, &cache_temporary))
            return ARTIST_THUMB_FOUND;
        if (cache_temporary) return ARTIST_THUMB_TEMPORARY;

        uint8_t * data = NULL;
        uint32_t size = 0;
        albumart_load_result_t load = albumart_load_file_ex(source_path, &data, &size,
                                THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) return ARTIST_THUMB_TEMPORARY;
        if (load == ALBUMART_LOAD_OK) {
            cover_decode_result_t decoded = cover_decode_to_rgb565_ex(data, size,
                    ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, prio,
                    cancel_cb, cancel_user_data, &resolution->pixels);
            free(data);
            if (decoded == COVER_DECODE_OK && resolution->pixels) {
                albumart_artist_store_rgb565(artist, source_mtime, ALBUM_THUMBNAIL_PX,
                                             ALBUM_THUMBNAIL_PX, resolution->pixels);
                return ARTIST_THUMB_FOUND;
            }
            if (decoded == COVER_DECODE_FAIL_CANCELLED) return ARTIST_THUMB_CANCELLED;
            if (cover_decode_result_is_temporary(decoded)) return ARTIST_THUMB_TEMPORARY;
        }
        free(data);
    }

    uint64_t stored_album_key = 0;
    int64_t stored_album_song_id = 0;
    if (albumart_artist_alias_load(artist, (unsigned int) group_kind,
                                   &stored_album_key, &stored_album_song_id)) {
        song_row_t stored_album_song;
        albumart_info_t stored_album_info;
        char found[PATH_MAX];
        bool valid = metadata_db_get_song_by_id(stored_album_song_id, &stored_album_song) &&
                     song_album_thumbnail_key(&stored_album_song) == stored_album_key &&
                     stored_album_song.tags.album[0] &&
                     metadata_db_get_album_for_group_offset(group_kind, artist,
                         stored_album_song.tags.album, stored_album_song.tags.album_artist) >= 0;
        if (valid) {
            albumart_info_from_song_row(&stored_album_song, &stored_album_info);
            valid = albumart_sized_thumb_fresh(&stored_album_info,
                         ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, found, sizeof(found));
        }
        if (valid) {
            artist_thumbnail_set_alias(stored_album_key, stored_album_song_id, resolution);
            if (prio == ARTWORK_PRIO_THUMBNAIL)
                artist_thumbnail_load_alias_pixels(found, stored_album_key, stored_album_song_id,
                                                   prio, cancel_cb, cancel_user_data, resolution);
            return ARTIST_THUMB_FOUND;
        }
        albumart_artist_alias_remove(artist, (unsigned int) group_kind);
    }

    /* The signature stats every album of this artist, so compute it only
     * when a no-art marker exists to validate or is about to be written;
     * artists whose cover resolves never pay for it. */
    const char * signature_dir = have_artist_dir ? artist_dir : "";
    if (albumart_artist_negative_exists(artist) &&
        albumart_artist_negative_fresh(artist, artist_directory_signature(
            artist, group_kind, &representative, signature_dir))) {
        resolution->negative_confirmed = true;
        return ARTIST_THUMB_MISSING;
    }

    artist_thumbnail_result_t result = artist_thumbnail_from_album_group(
        artist, group_kind, prio, cancel_cb, cancel_user_data, resolution);
    if (result == ARTIST_THUMB_FOUND) return result;
    if (result == ARTIST_THUMB_MISSING) {
        albumart_artist_store_negative(artist, artist_directory_signature(
            artist, group_kind, &representative, signature_dir));
        resolution->negative_confirmed = true;
    }
    return result;
}
