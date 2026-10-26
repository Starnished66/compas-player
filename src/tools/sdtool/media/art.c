#include "../sdtool.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../output/json_out.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../parse/parse_pool.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../library/tagcache/core/tagcache.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds the album cover caches the player's warmer builds: a 72 px thumbnail
 * and a 480 px player cover per album, from a cover file beside the music or
 * else a track's embedded picture, with the player's keys, BMP format and
 * source-mtime rule (albumart.c). The caches do not depend on the board, so
 * --board is validated and reported but changes nothing written. */

typedef struct {
    long albums;
    long written;
    long unchanged;
    long no_art;
    long failed;
} art_totals_t;

typedef struct {
    uint8_t *data;
    uint32_t size;
} cover_source_t;

static const char *const boards[] = { "r1", "r3proii", "r3ii_2025" };

static bool board_valid(const char *board) {
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++)
        if (strcmp(board, boards[i]) == 0) return true;
    return false;
}

static void info_for_song(const song_row_t *song, albumart_info_t *info) {
    char host[PATH_MAX];
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", storage_stored_path(song->path, host, sizeof(host)));
    snprintf(info->artist, sizeof(info->artist), "%s", song->tags.artist);
    snprintf(info->album, sizeof(info->album), "%s", song->tags.album);
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", song->tags.album_artist);
}

/* Embedded picture of one file, parsed in a worker process of its own. */
static sdtool_art_result_t embedded_cover(const char *host_path, cover_source_t *source) {
    return sdtool_parse_artwork(host_path, &source->data, &source->size);
}

typedef enum { COVER_FOUND, COVER_NONE, COVER_FAILED } cover_search_t;

/* The player's source order: a cover file in the album folder, then the
 * representative track's picture, then a few other tracks of the album. A track
 * whose parser fails ends the search: that album is failed, the rest go on. */
static cover_search_t find_cover(const albumart_info_t *info, const song_row_t *song, cover_source_t *source) {
    char found[PATH_MAX];
    if (albumart_search_files(info, "", found, sizeof(found)) &&
        albumart_load_file_ex(found, &source->data, &source->size, ALBUMART_SIDECAR_MAX_BYTES,
                              ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_OK)
        return COVER_FOUND;
    sdtool_art_result_t embedded = embedded_cover(info->path, source);
    if (embedded != SDTOOL_ART_NONE) return embedded == SDTOOL_ART_FOUND ? COVER_FOUND : COVER_FAILED;

    song_row_t siblings[ALBUMART_SIBLING_FALLBACK_MAX];
    int count = metadata_db_get_album_songs(info->album, info->albumartist, 0, siblings,
                                            ALBUMART_SIBLING_FALLBACK_MAX);
    for (int i = 0; i < count; i++) {
        char host[PATH_MAX];
        if (siblings[i].id == song->id) continue;
        embedded = embedded_cover(storage_stored_path(siblings[i].path, host, sizeof(host)), source);
        if (embedded != SDTOOL_ART_NONE) return embedded == SDTOOL_ART_FOUND ? COVER_FOUND : COVER_FAILED;
    }
    return COVER_NONE;
}

static bool store_caches(const albumart_info_t *info, const cover_source_t *source,
                         bool need_thumbnail, bool need_player) {
    uint16_t *player = NULL, *thumbnail = NULL;
    bool ok = true;
    if (need_player) {
        ok = cover_decode_to_rgb565_ex(source->data, source->size, ALBUMART_PLAYER_CACHE_SIZE,
                                       ALBUMART_PLAYER_CACHE_SIZE, ARTWORK_PRIO_WARMER, NULL, NULL,
                                       &player) == COVER_DECODE_OK && player &&
             albumart_store_rgb565(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE, player);
    }
    if (ok && need_thumbnail) {
        /* Derived from the player-size decode when there is one, as the warmer does. */
        thumbnail = player ? cover_resize_rgb565(player, ALBUMART_PLAYER_CACHE_SIZE,
                                                 ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_THUMBNAIL_SIZE,
                                                 ALBUMART_THUMBNAIL_SIZE)
                           : NULL;
        if (!thumbnail)
            ok = cover_decode_to_rgb565_ex(source->data, source->size, ALBUMART_THUMBNAIL_SIZE,
                                           ALBUMART_THUMBNAIL_SIZE, ARTWORK_PRIO_WARMER, NULL, NULL,
                                           &thumbnail) == COVER_DECODE_OK && thumbnail;
        ok = ok && albumart_store_rgb565(info, ALBUMART_THUMBNAIL_SIZE, ALBUMART_THUMBNAIL_SIZE, thumbnail);
    }
    free(player);
    free(thumbnail);
    return ok;
}

static void build_album(int64_t song_id, art_totals_t *totals) {
    song_row_t song;
    albumart_info_t info;
    char found[PATH_MAX];
    if (!metadata_db_get_song_by_id(song_id, &song)) return;
    info_for_song(&song, &info);
    /* The cache is keyed by album and artist, so an untagged track has no name to store under. */
    if (!info.album[0] || !(info.albumartist[0] || info.artist[0])) {
        totals->no_art++;
        return;
    }
    bool need_thumbnail = !albumart_sized_thumb_fresh(&info, ALBUMART_THUMBNAIL_SIZE,
                                                      ALBUMART_THUMBNAIL_SIZE, found, sizeof(found));
    bool need_player = !albumart_generated_cache_fresh(&info, ALBUMART_PLAYER_CACHE_SIZE,
                                                       ALBUMART_PLAYER_CACHE_SIZE, found, sizeof(found));
    if (!need_thumbnail && !need_player) {
        totals->unchanged++;
        return;
    }
    cover_source_t source = {0};
    cover_search_t search = find_cover(&info, &song, &source);
    if (search != COVER_FOUND) {
        if (search == COVER_FAILED) {
            sdtool_log("could not read the embedded cover of %s", info.path);
            totals->failed++;
        } else {
            totals->no_art++;
        }
        return;
    }
    if (store_caches(&info, &source, need_thumbnail, need_player)) totals->written++;
    else totals->failed++;
    free(source.data);
}

static bool resolve_board(const sdtool_options_t *options, char *board, size_t size) {
    if (options->board) {
        snprintf(board, size, "%s", options->board);
    } else if (!sdtool_root_device_model(board, size)) {
        snprintf(board, size, "%s", SDTOOL_DEFAULT_BOARD);
    }
    if (board_valid(board)) return true;
    json_error("bad_board", "the board must be r1, r3proii or r3ii_2025");
    return false;
}

int sdtool_cmd_art(const sdtool_options_t *options) {
    char board[32];
    sdtool_database_t db;
    if (!resolve_board(options, board, sizeof(board)) || !sdtool_open_database(&db)) return SDTOOL_FAILED;
    sdtool_mtime_calibrate();
    bool migrated = tagcache_indexes_migrated();

    enum { PAGE = 64 };
    group_row_t rows[PAGE];
    int64_t *ids = NULL;
    long count = 0, capacity = 0;
    for (int offset = 0;; offset += PAGE) {
        int got = metadata_db_get_albums_page_filtered(NULL, offset, PAGE, rows);
        for (int i = 0; i < got; i++) {
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 256;
                int64_t *grown = realloc(ids, sizeof(*ids) * (size_t) capacity);
                if (!grown) {
                    free(ids);
                    json_error("out_of_memory", "not enough memory to list the albums");
                    metadata_db_close();
                    return SDTOOL_FAILED;
                }
                ids = grown;
            }
            ids[count++] = rows[i].first_song_id;
        }
        if (got < PAGE) break;
    }

    art_totals_t totals = { .albums = count };
    json_progress("art", 0, count);
    for (long i = 0; i < count; i++) {
        build_album(ids[i], &totals);
        json_progress("art", i + 1, count);
    }
    free(ids);

    json_line_t line;
    json_begin(&line);
    json_string(&line, "event", "done");
    json_string(&line, "command", "art");
    json_string(&line, "board", board);
    json_integer(&line, "albums", totals.albums);
    json_integer(&line, "written", totals.written);
    json_integer(&line, "unchanged", totals.unchanged);
    json_integer(&line, "no_art", totals.no_art);
    json_integer(&line, "failed", totals.failed);
    json_boolean(&line, "migrated", migrated);
    json_integer(&line, "mtime_offset", sdtool_mtime_extra());
    json_emit(&line);
    metadata_db_close();
    /* The counts come first; a cover that could not be written (one bad source,
     * a full card) is then reported without losing them. */
    if (totals.failed) {
        char message[96];
        snprintf(message, sizeof(message), "%ld album covers could not be written", totals.failed);
        json_error("art_failed", message);
        return SDTOOL_FAILED;
    }
    return SDTOOL_OK;
}
