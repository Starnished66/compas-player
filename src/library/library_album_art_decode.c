#include "library_artwork_internal.h"
#include "library_player_art.h"

#include "albumart.h"
#include "cover_decode.h"
#include "cue_parser.h"
#include "db_log.h"
#include "metadata.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

uint64_t song_album_thumbnail_key(const song_row_t * row) {
    albumart_info_t info = {0};
    albumart_info_from_song_row(row, &info);
    if (info.album[0] && (info.albumartist[0] || info.artist[0]))
        return albumart_thumbnail_key(&info);

    /* Untagged tracks cannot use albumart.c's persistent artist/album name
     * key. Group them by their containing album directory in RAM, skipping a
     * Rockbox-style disc folder when the track is stored one level below it. */
    char directory[sizeof(row->path)];
    snprintf(directory, sizeof(directory), "%s", row->path);
    char * slash = strrchr(directory, '/');
    if (!slash) return albumart_artist_thumbnail_key(row->path);
    *slash = '\0';
    char * base = strrchr(directory, '/');
    base = base ? base + 1 : directory;
    if (albumart_is_disc_folder(base)) {
        slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
    }
    return albumart_artist_thumbnail_key(directory);
}

bool album_thumbnail_sized_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_sized_thumb_fresh(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, found, found_size);
}

bool album_player_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_generated_cache_fresh(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          found, found_size);
}

bool gui_library_album_cover_cached(int64_t song_id, int size_px, char * out, size_t out_size) {
    if (!out || out_size == 0) return false;
    song_row_t row;
    if (!metadata_db_get_song_by_id(song_id, &row)) {
        return false;
    }
    albumart_info_t info;
    albumart_info_from_song_row(&row, &info);
    bool hit = false;
    if (size_px == ALBUM_THUMBNAIL_PX) hit = album_thumbnail_sized_cache_hit(&info, out, out_size);
    else if (size_px == ALBUM_PLAYER_CACHE_PX) hit = album_player_cache_hit(&info, out, out_size);
    char absolute[PATH_MAX];
    if (hit) hit = realpath(out, absolute) != NULL && strlen(absolute) < out_size;
    if (hit) memcpy(out, absolute, strlen(absolute) + 1);
    return hit;
}

#define ALBUM_ART_METADATA_TIMEOUT_MS 5000

/* Bound on how many other songs from the same album to probe for embedded
 * art once the album's own representative song (first_song_id) has none
 * and no sidecar exists either -- covers rips where only some tracks carry
 * embedded art (e.g. a short intro/interlude or bonus track).
 * Kept small: each probe is a full isolated-process extraction. */
#define ALBUM_ART_SIBLING_FALLBACK_MAX 5

time_t album_source_mtime_with_art_mtime(const song_row_t * song, uint32_t art_mtime) {
    time_t max_mtime = (time_t) art_mtime;
    struct stat st;
    if (song && song->path[0] && stat(song->path, &st) == 0) {
        if (st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        /* Parent folder mtime (detects adding or modifying cover.jpg) */
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", song->path);
        char * slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (stat(dir, &st) == 0 && st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        }
    }
    return max_mtime;
}

time_t album_source_mtime(const song_row_t * song, const albumart_info_t * info) {
    return album_source_mtime_with_art_mtime(song, albumart_source_mtime(info));
}

/* The warmer builds the player-sized cache from the original compressed art
 * while it is already resident for the 72px decode.  Keeping this out of the
 * visible thumbnail path prevents scrolling from triggering a second large
 * decode; the player can then open the persistent 480px BMP without any
 * metadata extraction or lazy source decode.
 *
 * When a fresh 480px decode actually runs (warmer, cache miss) and is stored,
 * *out_player_pixels receives those pixels so the caller can derive the 72px
 * thumbnail from them instead of entropy-decoding the same source a second
 * time.  On every skip path (not warmer, cache already present) and every
 * failure, *out_player_pixels is NULL -- caller then does the 72px decode
 * itself, same as before.  Caller owns and must free() a non-NULL return. */
/* Set by the warmer body on its own thread: also materialize the 480px
 * player cache while decoding. Independent of the coordinator priority, so
 * an explicit reload can run at thumbnail priority during playback. */
static __thread bool album_art_materialize_player;
/* Set when the last decode on this thread returned a user's own sized
 * thumbnail (a valid cover.72x72 beside the music) instead of writing a
 * generated 72px copy. */
static __thread bool album_art_thumb_from_user_file;

static cover_decode_result_t album_thumbnail_maybe_store_player_cache(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_player_pixels) {
    if (out_player_pixels) *out_player_pixels = NULL;
    if (!album_art_materialize_player || !info || !data || size == 0)
        return COVER_DECODE_OK;
    char found[PATH_MAX];
    if (album_player_cache_hit(info, found, sizeof(found))) return COVER_DECODE_OK;

    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, prio,
        cancel_cb, user_data, &pixels);
    if (res == COVER_DECODE_OK && pixels &&
        !albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, pixels))
        res = COVER_DECODE_FAIL_ALLOC;
    if (res == COVER_DECODE_OK && pixels && out_player_pixels) {
        *out_player_pixels = pixels;
        pixels = NULL;
    }
    free(pixels);
    return res;
}

/* 72px pixels: cover-fit from a just-decoded 480px buffer when the warmer
 * actually produced one, otherwise a single decode at thumbnail size. */
static cover_decode_result_t album_thumbnail_pixels_from_source(
        const uint8_t * data, uint32_t size, const uint16_t * player_pixels,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_pixels) {
    if (player_pixels) {
        *out_pixels = cover_resize_rgb565(player_pixels,
                                          ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX);
        return *out_pixels ? COVER_DECODE_OK : COVER_DECODE_FAIL_ALLOC;
    }
    return cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                     prio, cancel_cb, user_data, out_pixels);
}

/* On-demand fallback for the Playing Now page when the warmer hasn't
 * produced the player-sized cache for this track yet: same source order as
 * album_thumbnail_load_or_decode_ex() (external sidecar, then embedded
 * picture via the bounded/isolated extraction below -- never a plain
 * metadata_read(), which can OOM on a huge APIC/covr tag), decodes and
 * persists the cache, and returns display-sized pixels so the caller has
 * something to show immediately instead of waiting for the warmer.
 *
 * *out_no_art_confirmed is set true only when this track was actually
 * determined to have no art anywhere (safe for the caller to stop asking
 * about it), and left false on every transient failure (coordinator/
 * memory-pressure admission refused, isolated-helper fork/pipe/timeout
 * failure, cancellation) -- a caller keying a negative cache off this must
 * not do so on a transient false, or a passing resource hiccup would
 * permanently hide art that is actually there. */
/* Shared by both sources below: decodes+stores the 480px player cache from
 * BORROWED `data`/`size` (caller still owns and must free it) and, on
 * success, resizes it into *out_pixels for immediate display. Deliberately
 * makes no negative-cache/"no art confirmed" decision of its own -- source
 * selection, input ownership, and confirmation semantics all stay the
 * caller's responsibility, since a permanent failure decoding ONE source
 * must not by itself confirm "no art anywhere" while another source still
 * has a chance (see gui_library_generate_player_cover()'s own comment). */
static cover_decode_result_t generate_player_cover_from_data(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels) {
    uint16_t * cache_pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
        ARTWORK_PRIO_PLAYER, cancel_cb, user_data, &cache_pixels);
    if (res == COVER_DECODE_OK && cache_pixels) {
        if (cancel_cb && cancel_cb(user_data)) {
            free(cache_pixels);
            return COVER_DECODE_FAIL_CANCELLED;
        }
        albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, cache_pixels);
        *out_pixels = cover_resize_rgb565(cache_pixels, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          COVER_ART_WIDTH, COVER_ART_HEIGHT);
        if (!*out_pixels) res = COVER_DECODE_FAIL_ALLOC;
    }
    free(cache_pixels);
    return res;
}

bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed) {
    *out_pixels = NULL;
    if (out_no_art_confirmed) *out_no_art_confirmed = false;
    if (!track_path || !track_path[0]) return false;

    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", track_path);
    if (cue_reference_is(track_path) &&
        !cue_reference_audio_path(track_path, info.path, sizeof(info.path))) return false;
    snprintf(info.artist, sizeof(info.artist), "%s", artist ? artist : "");
    snprintf(info.album, sizeof(info.album), "%s", album ? album : "");
    snprintf(info.albumartist, sizeof(info.albumartist), "%s", album_artist ? album_artist : "");

    /* Step 1: try an external sidecar file first. A permanent DECODE failure
     * here (corrupt/oversized/unsupported -- as opposed to merely failing
     * to LOAD, or a transient/cancelled result) falls through to embedded
     * extraction below instead of giving up -- this function used to keep
     * whatever sidecar bytes loaded and never attempt embedded art at all
     * once ALBUMART_LOAD_OK was seen, permanently hiding real embedded art
     * behind one corrupt sidecar file. */
    char found[PATH_MAX];
    if (albumart_search_files(&info, "", found, sizeof(found))) {
        uint8_t * sidecar_data = NULL;
        uint32_t sidecar_size = 0;
        albumart_load_result_t load = albumart_load_file_ex(found, &sidecar_data, &sidecar_size,
                                                             THUMBNAIL_SIDECAR_MAX_BYTES, ARTWORK_PRIO_PLAYER);
        if (load == ALBUMART_LOAD_TEMPORARY) return false; /* transient -- do not confirm "no art" */
        if (load == ALBUMART_LOAD_OK) {
            DB_LOG("ART_PLAYER", "generate_source path=%s source=sidecar", track_path);
            cover_decode_result_t res = generate_player_cover_from_data(
                &info, sidecar_data, sidecar_size, cancel_cb, user_data, out_pixels);
            free(sidecar_data);
            if (res == COVER_DECODE_OK && *out_pixels) return true;
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) return false; /* transient -- do not confirm "no art" */
            /* Permanent sidecar decode failure -- fall through to embedded
             * extraction. Must NOT confirm "no art" here on its own; only
             * the fully-exhausted embedded path below may do that. */
        }
        /* ALBUMART_LOAD_INVALID (missing/unreadable/oversized) also falls
         * through here with no special-casing needed. */
    }

    /* Step 2: embedded picture from the audio file's own tags. Reached when
     * there's no sidecar, the sidecar failed to load, or it loaded but
     * failed to decode permanently. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        ARTWORK_PRIO_PLAYER, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission != ARTWORK_ACQUIRE_OK) return false; /* transient -- coordinator busy/cancelled */
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t artwork_result = metadata_read_artwork_isolated(
        track_path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, ARTWORK_PRIO_PLAYER);
    artwork_coordinator_release(ARTWORK_PRIO_PLAYER);
    uint8_t * embedded_data = meta.picture_data;
    uint32_t embedded_size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (cancel_cb && cancel_cb(user_data)) {
        free(embedded_data);
        return false;
    }

    if (!embedded_data || embedded_size == 0) {
        free(embedded_data);
        /* Confirmed empty only if the embedded-picture helper actually ran
         * to completion and found nothing (or found something invalid) --
         * not if it merely failed to run (fork/pipe/timeout). Reaching here
         * already means embedded extraction had its chance regardless of
         * whether a sidecar file existed or how it failed. */
        if (out_no_art_confirmed && (artwork_result == METADATA_ARTWORK_NOT_FOUND ||
                                     artwork_result == METADATA_ARTWORK_INVALID ||
                                     artwork_result == METADATA_ARTWORK_TOO_LARGE))
            *out_no_art_confirmed = true;
        return false;
    }

    DB_LOG("ART_PLAYER", "generate_source path=%s source=embedded", track_path);
    cover_decode_result_t res = generate_player_cover_from_data(
        &info, embedded_data, embedded_size, cancel_cb, user_data, out_pixels);
    free(embedded_data);
    if (res != COVER_DECODE_OK && out_no_art_confirmed && cover_decode_result_is_permanent(res))
        *out_no_art_confirmed = true; /* corrupt/oversized source -- won't change until re-tagged */
    return res == COVER_DECODE_OK;
}

/* Result of one embedded-art probe against a single sibling song file --
 * distinguishes "this song genuinely has no picture" (keep trying other
 * siblings) from every transient/cancelled outcome (stop probing without
 * ever letting the caller record a permanent album-level failure over a
 * passing interruption -- mirrors how Step 3 above treats the primary
 * song's own cancelled/temporary outcomes). */
typedef enum {
    SIBLING_ART_FOUND,      /* success -- *out_pixels populated */
    SIBLING_ART_NOT_FOUND,  /* probe completed, this song has no usable picture */
    SIBLING_ART_TEMPORARY,  /* busy/low-memory/timeout/temporary decode failure */
    SIBLING_ART_CANCELLED,  /* cancelled or suspended -- record nothing */
} sibling_art_result_t;

/* One bounded attempt at embedded-picture extraction from a single sibling
 * song file, used only by the Step 3b fallback below (the primary
 * representative song still goes through the separately inlined Step 3
 * above, which this mirrors). On success, also persists the sized
 * thumbnail/player caches keyed by `info`, i.e. by album identity, not by
 * which song file supplied the bytes. */
static sibling_art_result_t album_thumbnail_try_embedded_art_from_path(
        const albumart_info_t * info, const char * path, artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels) {
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        prio, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission == ARTWORK_ACQUIRE_CANCELLED || admission == ARTWORK_ACQUIRE_SUSPENDED)
        return SIBLING_ART_CANCELLED;
    if (admission != ARTWORK_ACQUIRE_OK)
        return SIBLING_ART_TEMPORARY;

    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t metadata_result =
        metadata_read_artwork_isolated(path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, prio);
    artwork_coordinator_release(prio);
    uint8_t * data = meta.picture_data;
    uint32_t size = meta.picture_size;
    free(meta.lyrics);

    if (!data || size == 0) {
        free(data);
        return metadata_result == METADATA_ARTWORK_TEMPORARY_FAILURE
            ? SIBLING_ART_TEMPORARY : SIBLING_ART_NOT_FOUND;
    }

    uint16_t * player_pixels = NULL;
    cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
        info, data, size, prio, cancel_cb, user_data, &player_pixels);
    if (player_res == COVER_DECODE_FAIL_CANCELLED) {
        free(player_pixels);
        free(data);
        return SIBLING_ART_CANCELLED;
    }
    cover_decode_result_t res = album_thumbnail_pixels_from_source(
        data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
    free(player_pixels);
    free(data);
    if (res == COVER_DECODE_OK && *out_pixels) {
        albumart_store_rgb565(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
        return SIBLING_ART_FOUND;
    }
    if (res == COVER_DECODE_FAIL_CANCELLED) return SIBLING_ART_CANCELLED;
    if (cover_decode_result_is_temporary(res)) return SIBLING_ART_TEMPORARY;
    return SIBLING_ART_NOT_FOUND;
}

/* Rockbox albumart search, then embedded picture. A successful decode is
 * written as MUSIC_ROOT_DIR/.compas/albumart/<artist>-<album>.72x72.bmp
 * so the next pass is a small BMP load instead of a JPEG/PNG decode.
 * Albums without artist+album tags still decode, but cannot be stored.
 * Checks the negative failure cache first to prevent repeated failed decodes.
 * If sized thumbnail or sidecar is invalid/unreadable, falls through to embedded art. */
bool album_thumbnail_load_or_decode_with_mtime(const song_row_t * song, artwork_priority_t prio,
                                                       artwork_cancel_fn cancel_cb, void * user_data,
                                                       time_t source_mtime, bool sized_cache_checked,
                                                       const char * sidecar_path, bool sidecar_checked,
                                                       uint16_t ** out_pixels) {
    *out_pixels = NULL;
    if (!song || !song->path[0]) return false;

    albumart_info_t info;
    albumart_info_from_song_row(song, &info);

    uint64_t mtime_t0 = db_log_now_ms();
    DB_LOG("ART_LAZY", "source_mtime_ms=%llu song=%lld", (unsigned long long) (db_log_now_ms() - mtime_t0),
           (long long) song->id);

    artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
    if (artwork_failure_cache_is_blocked(song->id, source_mtime, &fail_reason)) return false;

    char found[PATH_MAX];
    char thumbnail_found[PATH_MAX];
    uint8_t * data = NULL;
    uint32_t size = 0;
    bool have_cached_thumbnail = false;
    album_art_thumb_from_user_file = false;

    /* Step 1: Try sized Rockbox thumbnail cache (.72x72.bmp) */
    bool step1_hit = !sized_cache_checked &&
                     album_thumbnail_sized_cache_hit(&info, thumbnail_found, sizeof(thumbnail_found));
    DB_LOG("ART_LAZY", "step1 song=%lld artist=%s albumartist=%s album=%s key=%016llx hit=%d found=%s",
           (long long) song->id, info.artist, info.albumartist, info.album,
           (unsigned long long) albumart_debug_thumbnail_key(&info), step1_hit,
           step1_hit ? thumbnail_found : "");
    if (step1_hit) {
        albumart_load_result_t load = albumart_load_file_ex(thumbnail_found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false; /* Do not delete a valid cache file under memory pressure. */
        }
        if (load == ALBUMART_LOAD_OK) {
            cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                                                  prio, cancel_cb, user_data, out_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                /* A warmer pass must continue to the original source when
                 * only the 72px cache exists, so it can materialize 480px.
                 * Keep the fresh 72px pixels while doing that work; do not
                 * decode or rewrite the thumbnail a second time. */
                if (!album_art_materialize_player || album_player_cache_hit(&info, found, sizeof(found)))
                    return true;
                have_cached_thumbnail = true;
                album_art_thumb_from_user_file = !albumart_is_generated_cache_file(thumbnail_found);
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt sized thumbnail -> drop it (only if the app generated it;
             * the search also finds user images) and fall through to sources */
            if (res != COVER_DECODE_OK && albumart_is_generated_cache_file(thumbnail_found))
                unlink(thumbnail_found);
        } else if (albumart_is_generated_cache_file(thumbnail_found)) {
            unlink(thumbnail_found);
        }
    }

    /* Step 2: Try external sidecar file (cover.jpg, folder.jpg, etc.) */
    bool sidecar_searched = false;
    if (sidecar_checked) {
        if (sidecar_path && sidecar_path[0]) {
            int sidecar_len = snprintf(found, sizeof(found), "%s", sidecar_path);
            sidecar_searched = sidecar_len > 0 && (size_t) sidecar_len < sizeof(found);
        }
    } else {
        sidecar_searched = albumart_search_files(&info, "", found, sizeof(found));
    }
    if (sidecar_searched) {
        albumart_load_result_t load = albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            if (have_cached_thumbnail) return true;
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
        if (load == ALBUMART_LOAD_OK) {
            uint16_t * player_pixels = NULL;
            cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
                &info, data, size, prio, cancel_cb, user_data, &player_pixels);
            if (player_res == COVER_DECODE_FAIL_CANCELLED) {
                free(player_pixels);
                free(data);
                if (have_cached_thumbnail) {
                    free(*out_pixels);
                    *out_pixels = NULL;
                }
                return false;
            }
            if (have_cached_thumbnail) {
                free(player_pixels);
                free(data);
                return true;
            }
            /* Any OTHER temporary player-cache failure (memory admission,
             * coordinator busy) must not abort the whole attempt --
             * player_pixels is already NULL on any player_res failure (see
             * album_thumbnail_maybe_store_player_cache()'s own contract), so
             * falling through to the much smaller thumbnail-only decode
             * below costs nothing extra and can very plausibly still
             * succeed under the same pressure that blocked the larger 480px
             * attempt. Only the outcome of THAT decode (checked below) gets
             * recorded/returned. */
            cover_decode_result_t res = album_thumbnail_pixels_from_source(
                data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
            free(player_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
                return true;
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt or oversized sidecar falls through to embedded art */
        }
    }

    /* Step 3: Try embedded picture from audio file */
    /* Serialize extraction with decoding too. The helper bounds parser
     * allocations, and the parent admits the actual returned picture size. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        prio, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission == ARTWORK_ACQUIRE_CANCELLED || admission == ARTWORK_ACQUIRE_SUSPENDED) return false;
    if (admission != ARTWORK_ACQUIRE_OK) {
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t metadata_result =
        metadata_read_artwork_isolated(song->path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, prio);
    artwork_coordinator_release(prio);
    data = meta.picture_data;
    size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (data && size > 0) {
        uint16_t * player_pixels = NULL;
        cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
            &info, data, size, prio, cancel_cb, user_data, &player_pixels);
        if (player_res == COVER_DECODE_FAIL_CANCELLED) {
            free(player_pixels);
            free(data);
            if (have_cached_thumbnail) {
                free(*out_pixels);
                *out_pixels = NULL;
            }
            return false;
        }
        if (have_cached_thumbnail) {
            free(player_pixels);
            free(data);
            return true;
        }
        /* See the mirrored sidecar-path comment above (Step 2) -- any OTHER
         * temporary player-cache failure falls through to the smaller
         * thumbnail-only decode instead of aborting immediately. */
        cover_decode_result_t res = album_thumbnail_pixels_from_source(
            data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
        free(player_pixels);
        free(data);
        data = NULL;
        size = 0;
        if (res == COVER_DECODE_OK && *out_pixels) {
            albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
            return true;
        }
        if (res == COVER_DECODE_FAIL_CANCELLED) return false;
        if (cover_decode_result_is_temporary(res)) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
    }

    if (metadata_result == METADATA_ARTWORK_TEMPORARY_FAILURE) {
        if (have_cached_thumbnail) return true;
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }

    if (have_cached_thumbnail) return true;

    /* Step 3b: the representative song for this album may simply be a track
     * with no embedded picture of its own (e.g. a short intro/interlude or
     * bonus track) even though sibling tracks in the same album do carry
     * art -- and Step 2 already searched the whole folder for a sidecar, so
     * a missing sidecar here means there truly isn't one. Try a bounded
     * number of other songs from the same album before giving up on the album
     * entirely. A cancelled/suspended or merely transient outcome on any
     * probe stops the loop immediately without recording anything permanent --
     * only a probe that actually completed and found nothing counts toward
     * "try the next sibling". */
    if (info.album[0] && info.albumartist[0]) {
        song_row_t siblings[ALBUM_ART_SIBLING_FALLBACK_MAX];
        int sibling_count = metadata_db_get_album_songs(info.album, info.albumartist, 0,
                                                         siblings, ALBUM_ART_SIBLING_FALLBACK_MAX);
        for (int i = 0; i < sibling_count; i++) {
            if (siblings[i].id == song->id || !siblings[i].path[0]) continue;
            if (cancel_cb && cancel_cb(user_data)) return false;
            sibling_art_result_t sr = album_thumbnail_try_embedded_art_from_path(
                &info, siblings[i].path, prio, cancel_cb, user_data, out_pixels);
            if (sr == SIBLING_ART_FOUND) return true;
            if (sr == SIBLING_ART_CANCELLED) return false;
            if (sr == SIBLING_ART_TEMPORARY) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* SIBLING_ART_NOT_FOUND -- try the next sibling */
        }
    }

    /* Step 4: No valid artwork found or permanent decode failure */
    artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_PERMANENT);
    return false;
}

bool album_thumbnail_load_or_decode_ex(const song_row_t * song, artwork_priority_t prio,
                                              artwork_cancel_fn cancel_cb, void * user_data,
                                              uint16_t ** out_pixels) {
    if (!song || !song->path[0]) {
        *out_pixels = NULL;
        return false;
    }
    albumart_info_t info;
    albumart_info_from_song_row(song, &info);
    time_t source_mtime = album_source_mtime(song, &info);
    return album_thumbnail_load_or_decode_with_mtime(song, prio, cancel_cb, user_data,
                                                      source_mtime, false, NULL, false, out_pixels);
}

bool album_thumbnail_reload_persisted(const song_row_t * song) {
    albumart_info_t info;
    albumart_info_from_song_row(song, &info);
    if (!info.album[0] || !(info.albumartist[0] || info.artist[0])) return true;
    /* Generated files, or a user's cover.72x72 that this decode actually
     * read successfully; a user file's mere presence proves nothing. */
    char found[PATH_MAX];
    return (album_art_thumb_from_user_file ||
            albumart_generated_cache_fresh(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                           found, sizeof(found))) &&
           albumart_generated_cache_fresh(&info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          found, sizeof(found));
}
void album_art_enable_player_cache_materialization(void) {
    album_art_materialize_player = true;
}
