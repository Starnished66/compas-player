#include "remote_control_internal.h"
#include "remote_grow_buf.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/artwork/support/library_endian.h"
#include "../../../library/playlists/favorite_writer.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../ui/library/browse/gui_library.h" /* sd_card_root_is_mounted() */
#include "../../../audio/engine/audio.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Rows per page of the extension list routes. */
#define EXT_PAGE_MAX 100

/* Caps both the page size for build_library_json() and, indirectly, the
 * response size. */
#define LIBRARY_JSON_MAX_LIMIT 100

static void build_library_json(const char * query, const char * artist_filter, const char * album_artist_filter,
                                const char * album_filter, const char * genre_filter, int offset, int limit,
                                char * out, size_t out_size) {
    if (limit <= 0 || limit > LIBRARY_JSON_MAX_LIMIT) limit = LIBRARY_JSON_MAX_LIMIT;
    if (offset < 0) offset = 0;

    /* Query filtered songs directly from metadata_db. "index" in the response
     * is the persistent song id (song_row_t.id). */
    int64_t total_matches = metadata_db_count_songs_filtered(query, artist_filter, album_artist_filter, album_filter, genre_filter);

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%lld,\"songs\":[", (long long) total_matches);
    /* Keep room for the closing "]}" and NUL after the last row. */
    if (json.capacity > 2) json.capacity -= 2;

    /* Allocate rows on the heap to keep stack usage bounded. */
    song_row_t * rows = malloc(sizeof(song_row_t) * LIBRARY_JSON_MAX_LIMIT);
    int n = rows ? metadata_db_get_songs_filtered_page(query, artist_filter, album_artist_filter, album_filter,
                                                         genre_filter, offset, limit, rows)
                 : 0;
    for (int i = 0; i < n && !json.truncated; i++) {
        char display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}", i > 0 ? "," : "",
                                  (long long) rows[i].id, title_esc, artist_esc)) break;
    }
    json.capacity = out_size;
    json.truncated = false; /* a rejected item left the prior JSON intact */
    json_builder_appendf(&json, "]}");
    free(rows);
}

/* Rows fetched per database call while streaming a whole group list. */
#define GROUPS_FETCH_PAGE 500

/* Sends every group row (album groups matching album_filter when by_albums,
 * else kind groups) as
 * {"<json_key>":[{"name":..., "count":..., "index":..., "album_artist":...}]}
 * index is a representative first song id from group_row_t.first_song_id.
 * Rows are fetched a page at a time and rendered as they arrive, so memory
 * is the response itself plus one page and a long list is never cut short.
 * Sends 500 when memory runs out. */
static void send_group_list_json(int cfd, bool by_albums, metadata_db_group_kind_t kind, const char * album_filter,
                                 const char * json_key) {
    const bool by_genre = strcmp(json_key, "genres") == 0;
    group_row_t * page = malloc(sizeof(group_row_t) * GROUPS_FETCH_PAGE);
    grow_buf_t out = {0};
    bool ok = page && grow_buf_appendf(&out, "{\"%s\":[", json_key);
    int n = 0;
    bool unavailable = false;
    metadata_db_group_walk_t walk = {0};
    while (ok && !walk.done) {
        int got = metadata_db_group_walk_page(&walk, by_albums, kind, album_filter, GROUPS_FETCH_PAGE, page);
        if (got < 0) {
            unavailable = true;
            break;
        }
        for (int i = 0; i < got && ok; i++) {
            /* json_escape_append() only needs the leading NUL. */
            char name_esc[METADATA_DB_TEXT_MAX * 6], album_artist_esc[300];
            name_esc[0] = '\0';
            album_artist_esc[0] = '\0';
            json_escape_append(name_esc, sizeof(name_esc), by_genre ? page[i].genre_name : page[i].name);
            /* album_artist is populated for album groups. */
            json_escape_append(album_artist_esc, sizeof(album_artist_esc), page[i].album_artist);
            ok = grow_buf_appendf(&out,
                "%s{\"name\":\"%s\",\"count\":%d,\"index\":%lld,\"album_artist\":\"%s\"}",
                n + i > 0 ? "," : "", name_esc, page[i].song_count, (long long) page[i].first_song_id,
                album_artist_esc);
        }
        n += got;
    }
    free(page);
    if (unavailable) {
        free(out.data);
        send_reason_error(cfd, "503 Service Unavailable", "library_unavailable");
        return;
    }
    if (ok && grow_buf_appendf(&out, "]}")) {
        send_json_free(cfd, out.data);
        return;
    }
    free(out.data);
    send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
}

/* Serves a song's embedded cover art as a raw image. Resolves the path from
 * song id or from the currently playing file if no index is provided. */
static bool resolve_art_source_path(bool have_index, int64_t index, char * out_path, size_t out_path_size) {
    if (have_index) {
        song_row_t row;
        if (!metadata_db_get_song_by_id(index, &row)) return false;
        snprintf(out_path, out_path_size, "%s", row.path);
        return true;
    }
    return remote_state_now_playing_path(out_path, out_path_size);
}

void handle_art_request(int cfd, const char * path) {
    int64_t index = -1;
    bool have_index = query_param_int64(path, "index", &index);

    char song_path[600] = {0};
    if (!resolve_art_source_path(have_index, index, song_path, sizeof(song_path))) {
        send_response(cfd, "404 Not Found", "text/plain", "No track");
        return;
    }

    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read(song_path, &meta);

    if (meta.picture_data && meta.picture_size > 0) {
        send_response_binary(cfd, "200 OK", sniff_image_content_type(meta.picture_data, meta.picture_size),
                              meta.picture_data, meta.picture_size);
    } else {
        send_response(cfd, "404 Not Found", "text/plain", "No art");
    }
    free(meta.picture_data);
    free(meta.lyrics); /* only needed picture_data here */
}

/* ---- Folders ---- */

/* The player has a single music storage root, the SD card. */
#define FOLDER_ROOT_ID "sd"

/* A folder path is relative to the root: "" (or "/") is the root itself,
 * otherwise '/'-separated components that are not empty, ".", "..", or
 * hidden (leading '.', which the Files view never lists either), with no
 * backslash or control bytes. Writes the absolute path. */
static bool folder_resolve(const char * path, char * out_rel, size_t rel_size,
                           char * out_abs, size_t abs_size) {
    char root_id[16];
    int root_state = query_decoded(path, "root", root_id, sizeof(root_id));
    if (root_state < 0 || (root_state == 1 && strcmp(root_id, FOLDER_ROOT_ID) != 0)) return false;
    char rel[REMOTE_CONTROL_PATH_MAX];
    if (query_decoded(path, "path", rel, sizeof(rel)) < 0) return false;
    const char * start = rel[0] == '/' ? rel + 1 : rel;
    size_t len = strlen(start);
    if (len && start[len - 1] == '/') return false;
    const char * component = start;
    for (const char * p = start;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t) (p - component);
            if (len && (n == 0 || component[0] == '.')) return false;
            if (*p == '\0') break;
            component = p + 1;
        } else if ((unsigned char) *p < 0x20 || *p == '\\') {
            return false;
        }
    }
    if (snprintf(out_rel, rel_size, "%s", start) >= (int) rel_size) return false;
    int n = len ? snprintf(out_abs, abs_size, "%s/%s", MUSIC_ROOT_DIR, start)
                : snprintf(out_abs, abs_size, "%s", MUSIC_ROOT_DIR);
    return n >= 0 && (size_t) n < abs_size;
}

/* An existing path must still resolve inside the root once symlinks are
 * followed (the SD card's FAT has none; this guards other mounts). */
static bool folder_within_root(const char * abs_path) {
    char root[PATH_MAX], real[PATH_MAX];
    if (!realpath(MUSIC_ROOT_DIR, root) || !realpath(abs_path, real)) return false;
    size_t n = strlen(root);
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

void handle_folder_roots_request(int cfd) {
    char json[160];
    snprintf(json, sizeof(json),
             "{\"roots\":[{\"id\":\"" FOLDER_ROOT_ID "\",\"name\":\"SD card\",\"available\":%s}]}",
             sd_card_root_is_mounted() ? "true" : "false");
    send_response(cfd, "200 OK", "application/json", json);
}

/* Directories sort first in a file_browser index, so the folder count is
 * the first non-directory ordinal. */
static unsigned folder_count_of(const file_browser_index_t * index, unsigned count) {
    unsigned lo = 0, hi = count;
    char name[256];
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        bool is_dir = false;
        if (file_browser_index_entry_name(index, mid, name, sizeof(name), &is_dir) && is_dir) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

#define FOLDER_JSON_CAPACITY (128u * 1024u)

/* GET /api/folders?root=sd&path=REL&offset=N&limit=N -- one page of a
 * folder in the Files view's order (folders, then files by name; hidden
 * entries and symlinks omitted). Tracks carry their library song id, or -1
 * when the file is not in the library. */
void handle_folder_list_request(int cfd, const char * path) {
    char rel[REMOTE_CONTROL_PATH_MAX], abs_path[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR)];
    int offset, limit;
    if (!folder_resolve(path, rel, sizeof(rel), abs_path, sizeof(abs_path))) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_path");
        return;
    }
    if (!query_paging(path, EXT_PAGE_MAX, &offset, &limit)) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_paging");
        return;
    }
    if (!sd_card_root_is_mounted()) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "storage_unavailable");
        return;
    }
    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode) || !folder_within_root(abs_path)) {
        send_api_error(cfd, "404 Not Found", "folder_error", "folder_not_found");
        return;
    }
    file_browser_index_t * index = NULL;
    unsigned count = 0;
    if (!file_browser_index_open(abs_path, &index, &count)) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error",
                       errno == EFBIG ? "folder_too_large" : "folder_unavailable");
        return;
    }
    unsigned folders = folder_count_of(index, count);
    unsigned first = (unsigned) offset < count ? (unsigned) offset : count;
    unsigned page = count - first < (unsigned) limit ? count - first : (unsigned) limit;

    /* Resolve the page's track ids in one batched lookup. */
    char (*names)[256] = page ? calloc(page, sizeof(*names)) : NULL;
    bool * dirs = page ? calloc(page, sizeof(*dirs)) : NULL;
    char ** track_paths = page ? calloc(page, sizeof(*track_paths)) : NULL;
    song_row_t * rows = page ? calloc(page, sizeof(*rows)) : NULL;
    char * json_buf = malloc(FOLDER_JSON_CAPACITY);
    bool ok = json_buf && (!page || (names && dirs && track_paths && rows));
    int track_count = 0;
    int * track_at = page ? calloc(page, sizeof(*track_at)) : NULL;
    if (page && !track_at) ok = false;
    for (unsigned i = 0; ok && i < page; i++) {
        track_at[i] = -1;
        if (!file_browser_index_entry_name(index, first + i, names[i], sizeof(names[i]), &dirs[i])) { ok = false; break; }
        if (dirs[i] || library_is_m3u_file(names[i])) continue;
        char full[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR) + 256];
        snprintf(full, sizeof(full), "%s/%s", abs_path, names[i]);
        track_paths[track_count] = strdup(full);
        if (!track_paths[track_count]) { ok = false; break; }
        track_at[i] = track_count++;
    }
    file_browser_index_close(index);
    if (ok && track_count) metadata_db_get_songs_by_paths((const char * const *) track_paths, track_count, rows);

    unsigned emitted = 0;
    if (ok) {
        json_builder_t json;
        json_builder_init(&json, json_buf, FOLDER_JSON_CAPACITY);
        char rel_esc[REMOTE_CONTROL_PATH_MAX * 6] = {0}, parent_esc[REMOTE_CONTROL_PATH_MAX * 6] = {0};
        json_escape_append(rel_esc, sizeof(rel_esc), rel);
        const char * slash = strrchr(rel, '/');
        if (slash) {
            char parent[REMOTE_CONTROL_PATH_MAX];
            snprintf(parent, sizeof(parent), "%.*s", (int) (slash - rel), rel);
            json_escape_append(parent_esc, sizeof(parent_esc), parent);
        }
        json_builder_appendf(&json,
            "{\"root\":\"" FOLDER_ROOT_ID "\",\"path\":\"%s\",\"parent\":%s%s%s,\"total\":%u,"
            "\"folder_count\":%u,\"offset\":%d,\"entries\":[",
            rel_esc, rel[0] ? "\"" : "null", rel[0] ? parent_esc : "", rel[0] ? "\"" : "",
            count, folders, offset);
        for (unsigned i = 0; i < page && !json.truncated; i++) {
            char row[4096], name_esc[256 * 6] = {0};
            json_escape_append(name_esc, sizeof(name_esc), names[i]);
            if (dirs[i] || track_at[i] < 0) {
                snprintf(row, sizeof(row), "%s{\"type\":\"%s\",\"name\":\"%s\"}", emitted ? "," : "",
                         dirs[i] ? "folder" : "playlist", name_esc);
            } else {
                const song_row_t * song = &rows[track_at[i]];
                bool known = song->id >= 0;
                char title[128] = {0}, title_esc[800] = {0}, artist_esc[800] = {0};
                if (known) metadata_db_song_display_title(song, title, sizeof(title));
                json_escape_append(title_esc, sizeof(title_esc), known ? title : names[i]);
                json_escape_append(artist_esc, sizeof(artist_esc), known ? song->tags.artist : "");
                snprintf(row, sizeof(row),
                         "%s{\"type\":\"track\",\"name\":\"%s\",\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                         emitted ? "," : "", name_esc, known ? (long long) song->id : -1LL, title_esc, artist_esc);
            }
            if (!json_append_row(&json, row, 64)) break;
            emitted++;
        }
        json.truncated = false;
        json_builder_appendf(&json, "],\"next_offset\":%u}", first + emitted);
        send_response(cfd, "200 OK", "application/json", json_buf);
    } else {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "temporary_unavailable");
    }
    for (int i = 0; i < track_count; i++) free(track_paths[i]);
    free(track_paths);
    free(track_at);
    free(names);
    free(dirs);
    free(rows);
    free(json_buf);
}

/* POST /api/folders/play?root=sd&path=REL_FILE -- plays a file with its
 * folder as the queue, as tapping it in Files does. */
void handle_folder_play_request(int cfd, const char * path) {
    char rel[REMOTE_CONTROL_PATH_MAX], abs_path[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR)];
    if (!folder_resolve(path, rel, sizeof(rel), abs_path, sizeof(abs_path)) || !rel[0] ||
        strlen(abs_path) >= REMOTE_CONTROL_PATH_MAX) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_path");
        return;
    }
    const char * base = strrchr(abs_path, '/');
    struct stat st;
    if (!sd_card_root_is_mounted()) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "storage_unavailable");
        return;
    }
    if (lstat(abs_path, &st) != 0 || !S_ISREG(st.st_mode) || !folder_within_root(abs_path)) {
        send_api_error(cfd, "404 Not Found", "folder_error", "track_not_found");
        return;
    }
    if (!file_browser_is_playable_name(base ? base + 1 : abs_path)) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "not_playable");
        return;
    }
    remote_state_post_folder_play(abs_path); /* length checked above */
    send_response(cfd, "200 OK", "text/plain", "OK");
}

/* ---- Recently played ---- */

void handle_recent_request(int cfd, const char * path) {
    int offset, limit;
    if (!query_paging(path, EXT_PAGE_MAX, &offset, &limit)) {
        send_api_error(cfd, "400 Bad Request", "recent_error", "invalid_paging");
        return;
    }
    song_row_t * rows = calloc((size_t) limit, sizeof(*rows));
    int64_t * played = calloc((size_t) limit, sizeof(*played));
    char * json_buf = malloc(65536);
    int total = 0;
    int n = rows && played && json_buf ? metadata_db_get_recently_played_page(offset, limit, rows, played, &total) : -1;
    if (n < 0) {
        free(rows); free(played); free(json_buf);
        send_api_error(cfd, "503 Service Unavailable", "recent_error", "temporary_unavailable");
        return;
    }
    json_builder_t json;
    json_builder_init(&json, json_buf, 65536);
    json_builder_appendf(&json, "{\"total\":%d,\"offset\":%d,\"songs\":[", total, offset);
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char row[1024], title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&rows[i], title, sizeof(title));
        json_escape_append(title_esc, sizeof(title_esc), title);
        json_escape_append(artist_esc, sizeof(artist_esc), rows[i].tags.artist);
        snprintf(row, sizeof(row), "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\",\"last_played\":%lld}",
                 emitted ? "," : "", (long long) rows[i].id, title_esc, artist_esc, (long long) played[i]);
        if (!json_append_row(&json, row, 64)) break;
        emitted++;
    }
    json_builder_appendf(&json, "],\"next_offset\":%d}", offset + emitted);
    send_response(cfd, "200 OK", "application/json", json_buf);
    free(rows); free(played); free(json_buf);
}

/* ---- Favorites ---- */

#define FAVORITE_STATE_MAX_IDS 100

/* GET /api/favorites/state?ids=1,2,3 */
void handle_favorite_state_request(int cfd, const char * path) {
    char ids_text[FAVORITE_STATE_MAX_IDS * 12];
    int64_t ids[FAVORITE_STATE_MAX_IDS];
    int count = 0;
    if (query_decoded(path, "ids", ids_text, sizeof(ids_text)) != 1 || !ids_text[0]) {
        send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
        return;
    }
    for (char * p = ids_text; *p;) {
        char * end = NULL;
        errno = 0;
        long long v = (*p >= '0' && *p <= '9') ? strtoll(p, &end, 10) : -1;
        if (v <= 0 || errno == ERANGE || v > INT32_MAX || (*end != ',' && *end != '\0') ||
            count >= FAVORITE_STATE_MAX_IDS) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
            return;
        }
        ids[count++] = (int64_t) v;
        p = *end ? end + 1 : end;
        if (*end == ',' && !*p) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
            return;
        }
    }
    song_row_t * rows = calloc((size_t) count, sizeof(*rows));
    char * json_buf = malloc(8192);
    if (!rows || !json_buf) {
        free(rows); free(json_buf);
        send_api_error(cfd, "503 Service Unavailable", "favorite_error", "temporary_unavailable");
        return;
    }
    metadata_db_get_songs_by_ids(ids, count, rows);
    json_builder_t json;
    json_builder_init(&json, json_buf, 8192);
    json_builder_appendf(&json, "{\"songs\":[");
    for (int i = 0; i < count; i++) {
        bool found = rows[i].id >= 0;
        bool favorite = found && metadata_db_song_favorite_is_set(rows[i].path);
        json_builder_appendf(&json, "%s{\"index\":%lld,\"found\":%s,\"favorite\":%s}", i ? "," : "",
                             (long long) ids[i], found ? "true" : "false", favorite ? "true" : "false");
    }
    json_builder_appendf(&json, "]}");
    send_response(cfd, "200 OK", "application/json", json_buf);
    free(rows);
    free(json_buf);
}

/* POST /api/favorites/add or /remove with index=ID (optional
 * catalog_revision) or current=1 for the now-playing file. Favorites are the
 * library's rating flag, which also backs the @favorites playlist, so that
 * playlist follows without a separate write. */
void handle_favorite_edit_request(int cfd, const char * path, bool is_favorite) {
    bool current = query_param_present(path, "current");
    bool have_index = query_param_present(path, "index");
    if (current == have_index) {
        send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_target");
        return;
    }
    song_row_t row;
    char song_path[REMOTE_CONTROL_PATH_MAX] = {0};
    int64_t index = -1;
    if (current) {
        uint64_t flag = 0;
        if (!catalog_query_number(path, "current", 0, 1, &flag) || flag != 1) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_target");
            return;
        }
        if (!remote_state_now_playing_path(song_path, sizeof(song_path))) {
            send_api_error(cfd, "404 Not Found", "favorite_error", "nothing_playing");
            return;
        }
        if (metadata_db_get_song_by_path(song_path, &row)) index = row.id;
    } else {
        uint64_t id = 0;
        char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
        if (!query_required_number(path, "index", INT32_MAX, &id) || id == 0 ||
            !catalog_query_revision_named(path, "catalog_revision", false, revision)) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_index");
            return;
        }
        metadata_db_catalog_result_t lookup = METADATA_DB_CATALOG_OK;
        bool found;
        if (revision[0]) {
            lookup = metadata_db_catalog_get_song_revision(revision, (int64_t) id, &row);
            found = lookup == METADATA_DB_CATALOG_OK;
        } else {
            found = metadata_db_get_song_by_id((int64_t) id, &row);
        }
        if (lookup == METADATA_DB_CATALOG_STALE) {
            send_api_error(cfd, "409 Conflict", "favorite_error", "revision_mismatch");
            return;
        }
        if (lookup == METADATA_DB_CATALOG_UNAVAILABLE) {
            send_api_error(cfd, "503 Service Unavailable", "favorite_error", "temporary_unavailable");
            return;
        }
        if (!found) {
            send_api_error(cfd, "404 Not Found", "favorite_error", "song_not_found");
            return;
        }
        index = row.id;
        snprintf(song_path, sizeof(song_path), "%s", row.path);
    }
    /* Ordered with the player's own heart taps, and confirmed on disk. */
    if (!favorite_writer_write_now(song_path, is_favorite)) {
        send_api_error(cfd, "503 Service Unavailable", "favorite_error", "write_failed");
        return;
    }
    remote_state_favorite_written(song_path, is_favorite);
    char json[96];
    snprintf(json, sizeof(json), "{\"index\":%lld,\"favorite\":%s}", (long long) index, is_favorite ? "true" : "false");
    send_response(cfd, "200 OK", "application/json", json);
}

/* GET /api/library/artists, /album_artists and /genres. Returns false for
 * any other path. The album-artist list also uses the "artists" key. */
bool handle_group_list_request(int cfd, const char * path_only) {
    static const struct {
        const char * route;
        metadata_db_group_kind_t kind;
        const char * json_key;
    } routes[] = {
        { "/api/library/artists", METADATA_DB_GROUP_ARTIST, "artists" },
        { "/api/library/album_artists", METADATA_DB_GROUP_ALBUM_ARTIST, "artists" },
        { "/api/library/genres", METADATA_DB_GROUP_GENRE, "genres" },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (strcmp(path_only, routes[i].route) != 0) continue;
        send_group_list_json(cfd, false, routes[i].kind, NULL, routes[i].json_key);
        return true;
    }
    return false;
}

/* GET /api/library with offset/limit, q and artist/album_artist/album/genre
 * filters. */
void handle_library_get(int cfd, const char * path) {
    int offset = 0, limit = 50;
    char query[128], artist_filter[128], album_artist_filter[128], album_filter[128];
    char genre_raw[METADATA_DB_TEXT_MAX * 3] = {0};
    char genre_filter[METADATA_DB_TEXT_MAX] = {0};
    query_param_int_saturating(path, "offset", &offset);
    query_param_int_saturating(path, "limit", &limit);
    if (query_loose(path, "q", query, sizeof(query)) < 0 ||
        query_loose(path, "artist", artist_filter, sizeof(artist_filter)) < 0 ||
        query_loose(path, "album_artist", album_artist_filter, sizeof(album_artist_filter)) < 0 ||
        query_loose(path, "album", album_filter, sizeof(album_filter)) < 0) {
        send_reason_error(cfd, "400 Bad Request", "query_parameter_too_long");
        return;
    }
    if (query_param_present(path, "genre") &&
        (!query_param_str(path, "genre", genre_raw, sizeof(genre_raw)) ||
         !url_decode_checked(genre_raw, genre_filter, sizeof(genre_filter)))) {
        send_reason_error(cfd, "400 Bad Request", "invalid_genre_filter");
        return;
    }
    char * json = json_alloc_or_500(cfd, 65536);
    if (json) {
        build_library_json(query, artist_filter, album_artist_filter, album_filter, genre_filter, offset, limit, json,
                            65536);
        send_json_free(cfd, json);
    }
}

/* GET /api/library/albums?artist=NAME or ?album_artist=NAME */
void handle_albums_get(int cfd, const char * path) {
    char artist_filter[128], album_artist_filter[128];
    if (query_loose(path, "artist", artist_filter, sizeof(artist_filter)) < 0 ||
        query_loose(path, "album_artist", album_artist_filter, sizeof(album_artist_filter)) < 0) {
        send_reason_error(cfd, "400 Bad Request", "query_parameter_too_long");
        return;
    }
    send_group_list_json(cfd, true, METADATA_DB_GROUP_ARTIST,
                         artist_filter[0] != '\0' ? artist_filter : album_artist_filter, "albums");
}
