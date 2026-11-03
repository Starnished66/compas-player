#include "remote_control_internal.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../../library/artwork/support/library_endian.h"
#include "../../../library/browser/paths/library_paths.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

/* The playlist name becomes a path component (PLAYLISTS_DIR "/" name ".m3u")
 * and is reachable by remote clients, so reject empty names and anything
 * containing a path separator or "..". */
bool playlist_name_is_safe(const char * name) {
    if (!name || name[0] == '\0') return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

/* PLAYLISTS_DIR/<name>.m3u; false when it does not fit. */
static bool playlist_m3u_path(const char * name, char * out, size_t size) {
    int n = snprintf(out, size, "%s/%s.m3u", PLAYLISTS_DIR, name);
    return n >= 0 && (size_t) n < size;
}

static void free_path_list(char ** paths, int count) {
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Fixed bytes of the built-in playlists and the closing syntax, and of one
 * user playlist row besides its escaped name and key. */
#define PLAYLISTS_JSON_BASE 512
#define PLAYLISTS_JSON_ROW_OVERHEAD 64

/* GET /api/playlists: JSON of every playlist, sized to hold all rows so a
 * long list is never cut short. Heap, not stack, to keep the connection
 * frame small. Sends 500 when it cannot be allocated. */
void handle_playlists_get(int cfd) {
    char ** paths = NULL;
    int count = 0;
    playlist_files_scan(PLAYLISTS_DIR, &paths, &count);

    /* Name and key are each at most 6 escaped bytes per path byte. */
    size_t out_size = PLAYLISTS_JSON_BASE;
    for (int i = 0; i < count; i++) out_size += PLAYLISTS_JSON_ROW_OVERHEAD + 12 * strlen(paths[i]);
    char * out = json_alloc_or_500(cfd, out_size);
    if (!out) {
        free_path_list(paths, count);
        return;
    }

    /* Build JSON array of playlists using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json,
                            "{\"playlists\":[{\"name\":\"Favorites\",\"key\":\"@favorites\",\"internal\":true,\"writable\":false},"
                            "{\"name\":\"Most Played\",\"key\":\"@most_played\",\"internal\":true,\"writable\":false},"
                            "{\"name\":\"Recently Played\",\"key\":\"@recently_played\",\"internal\":true,\"writable\":false}");
    size_t playlists_dir_len = strlen(PLAYLISTS_DIR);
    for (int i = 0; i < count && !json.truncated; i++) {
        /* The API routes resolve user playlists as
         * PLAYLISTS_DIR/<key>.m3u. Advertise only files that exact lookup can
         * reach: direct children with the canonical lowercase extension. */
        if (strncmp(paths[i], PLAYLISTS_DIR, playlists_dir_len) != 0 ||
            paths[i][playlists_dir_len] != '/') continue;
        const char * base = paths[i] + playlists_dir_len + 1;
        if (strchr(base, '/') != NULL) continue;
        size_t base_len = strlen(base);
        if (base_len <= 4 || strcmp(base + base_len - 4, ".m3u") != 0) continue;

        size_t key_len = base_len - 4;
        char key[256] = {0};
        /* Playback context and playlist lookup use 128-byte decoded names. */
        if (key_len >= sizeof(key) || key_len > PLAYLIST_NAME_MAX_BYTES) continue;
        memcpy(key, base, key_len);
        if (!playlist_name_is_safe(key)) continue;

        char name_esc[1600] = {0}, key_esc[1600] = {0};
        json_escape_append(name_esc, sizeof(name_esc), base);
        json_escape_append(key_esc, sizeof(key_esc), key);
        if (!json_builder_appendf(&json, ",{\"name\":\"%s\",\"key\":\"%s\",\"internal\":false,\"writable\":true}",
                                  name_esc, key_esc)) break;
    }
    json_builder_appendf(&json, "]}");

    free_path_list(paths, count);
    send_json_free(cfd, out);
}

/* Built-in playlists are derived from the library, never files, so every
 * mutation refuses them. The display-name aliases are the ones the original
 * routes already resolve to built-ins. */
static bool playlist_key_is_builtin(const char * key) {
    return key[0] == '@' || strcmp(key, "Favorites") == 0 || strcmp(key, "Most Played") == 0;
}

/* Opaque playlist revision: FNV-1a over the resolved entry sequence, the
 * same sequence positions index. Comment-only edits keep it; any entry
 * change, including a same-size reorder, changes it. */
static void playlist_revision_of(char * const * paths, int count, char out[20]) {
    uint64_t hash = FNV_OFFSET_BASIS;
    for (int i = 0; i < count; i++) hash = fnv1a_byte(catalog_hash_text(hash, paths[i]), '\n');
    snprintf(out, 20, "p-%016llx", (unsigned long long) hash);
}

/* GET /api/playlists/songs?name=NAME -- one playlist's songs, in file
 * order, in the same {"index":N,"title":...,"artist":...} shape
 * /api/library uses so the phone UI can render both with the same row
 * code. Entries whose path no longer matches anything in the synced
 * library are skipped, as on the player's own playlist screen; each row's
 * "position" is its entry offset in the file (skipped entries still count),
 * which the playlist edit routes take. User playlists also carry the
 * revision those routes check. */
void handle_playlist_songs_request(int cfd, const char * path) {
    char name[128];
    if (query_decoded(path, "name", name, sizeof(name)) != 1 || !playlist_name_is_safe(name)) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    char ** paths = NULL;
    int count = 0;
    bool loaded = false;
    bool user_playlist = false;
    if (strcmp(name, "@favorites") == 0 || strcmp(name, "Favorites") == 0) {
        metadata_db_load_favorite_songs(&paths, &count);
        loaded = true; /* an empty built-in playlist is valid */
    } else if (strcmp(name, "@most_played") == 0 || strcmp(name, "Most Played") == 0) {
        metadata_db_load_top_played_songs(20, &paths, &count);
        loaded = true;
    } else if (strcmp(name, "@recently_played") == 0) {
        metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX, &paths, &count);
        loaded = true;
    } else {
        char m3u_path[512];
        playlist_m3u_path(name, m3u_path, sizeof(m3u_path));
        loaded = playlist_files_read(m3u_path, &paths, &count);
        user_playlist = true;
    }
    if (!loaded) {
        send_response(cfd, "404 Not Found", "text/plain", "Playlist not found");
        return;
    }

    char * json_buf = malloc(65536);
    if (!json_buf) {
        free_path_list(paths, count);
        send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
        return;
    }

    /* Optional offset (an entry position) continues a list that did not fit
     * in one response; next_offset < total says there is more. */
    uint64_t start = 0;
    if (!catalog_query_number(path, "offset", 0, INT_MAX, &start)) {
        free(json_buf);
        free_path_list(paths, count);
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    char revision[20] = "";
    if (user_playlist) playlist_revision_of(paths, count, revision);
    /* Resolve playlist file paths against metadata_db and serialize to JSON
     * using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, json_buf, 65536);
    if (user_playlist)
        json_builder_appendf(&json, "{\"writable\":true,\"revision\":\"%s\",\"songs\":[", revision);
    else
        json_builder_appendf(&json, "{\"writable\":false,\"revision\":null,\"songs\":[");
    int emitted = 0;
    int next = start < (uint64_t) count ? (int) start : count;
    for (; next < count; next++) {
        song_row_t row;
        if (!metadata_db_get_song_by_path(paths[next], &row)) continue;

        char row_json[768], display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&row, display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), row.tags.artist);
        snprintf(row_json, sizeof(row_json), "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\",\"position\":%d}",
                 emitted > 0 ? "," : "", (long long) row.id, title_esc, artist_esc, next);
        if (!json_append_row(&json, row_json, 64)) break;
        emitted++;
    }
    json_builder_appendf(&json, "],\"total\":%d,\"next_offset\":%d}", count, next);

    send_response(cfd, "200 OK", "application/json", json_buf);

    free(json_buf);
    free_path_list(paths, count);
}

/* ---- Playlist management ---- */

/* A new or renamed user playlist name: the playlist file rules, plus what
 * the API needs to address it afterwards as PLAYLISTS_DIR/<name>.m3u
 * (stem only, no "..", 127 bytes, not a built-in key or alias). */
static const char * playlist_new_name_error(const char * name) {
    if (!playlist_files_name_is_valid(name) || !playlist_name_is_safe(name) ||
        strlen(name) > PLAYLIST_NAME_MAX_BYTES || library_is_m3u_file(name)) return "invalid_name";
    if (playlist_key_is_builtin(name)) return "reserved_name";
    return NULL;
}

/* Error replies shared by the playlist routes. */
static void playlist_send_refusal(int cfd, const char * refusal) {
    send_api_error(cfd, strcmp(refusal, "revision_mismatch") == 0 ? "409 Conflict" : "400 Bad Request",
                   "playlist_error", refusal);
}

static void playlist_send_write_error(int cfd, bool exists) {
    send_api_error(cfd, exists ? "409 Conflict" : "500 Internal Server Error", "playlist_error",
                   exists ? "name_exists" : "write_failed");
}

/* revision is NULL when the file could not be reread after a successful
 * change; the client then rereads the playlist. */
static void playlist_send_ok(int cfd, const char * key, const char * revision) {
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 12 + 96];
    json_escape_append(key_esc, sizeof(key_esc), key);
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"name\":\"%s.m3u\",\"revision\":%s%s%s}",
             key_esc, key_esc, revision ? "\"" : "", revision ? revision : "null", revision ? "\"" : "");
    send_response(cfd, "200 OK", "application/json", json);
}

/* Resolves the `name` key of an existing user playlist. Sends the error and
 * returns false for a malformed, built-in, or missing playlist. */
static bool playlist_existing_path(int cfd, const char * path, char * key, size_t key_size,
                                   char * m3u_path, size_t m3u_size) {
    int state = query_decoded(path, "name", key, key_size);
    if (state != 1 || !key[0] || !playlist_name_is_safe(key) || strlen(key) > PLAYLIST_NAME_MAX_BYTES) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", "invalid_name");
        return false;
    }
    if (playlist_key_is_builtin(key)) {
        send_api_error(cfd, "403 Forbidden", "playlist_error", "read_only");
        return false;
    }
    struct stat st;
    if (!playlist_m3u_path(key, m3u_path, m3u_size) ||
        stat(m3u_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_api_error(cfd, "404 Not Found", "playlist_error", "playlist_not_found");
        return false;
    }
    return true;
}

static bool playlist_current_revision(const char * m3u_path, char revision[20]) {
    char ** paths = NULL;
    int count = 0;
    playlist_read_status_t status = playlist_files_read_ex(m3u_path, &paths, &count);
    if (status != PLAYLIST_READ_OK && status != PLAYLIST_READ_EMPTY) return false;
    playlist_revision_of(paths, count, revision);
    free_path_list(paths, count);
    return true;
}

/* POST /api/playlists/create?name=NAME -- a new empty user playlist. */
void handle_playlist_create_request(int cfd, const char * path) {
    char name[PLAYLIST_NAME_MAX_BYTES + 2];
    const char * error = query_decoded(path, "name", name, sizeof(name)) == 1 ? playlist_new_name_error(name)
                                                                               : "invalid_name";
    if (error) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", error);
        return;
    }
    char m3u_path[512];
    playlist_m3u_path(name, m3u_path, sizeof(m3u_path));
    errno = 0;
    if (access(m3u_path, F_OK) == 0 ||
        !playlist_files_write_new(PLAYLISTS_DIR, name, NULL, 0, m3u_path, sizeof(m3u_path))) {
        playlist_send_write_error(cfd, errno == EEXIST || access(m3u_path, F_OK) == 0);
        return;
    }
    metadata_db_playlist_insert_one(m3u_path);
    char revision[20];
    playlist_revision_of(NULL, 0, revision);
    playlist_send_ok(cfd, name, revision);
}

/* POST /api/playlists/rename?name=KEY&new_name=NAME */
void handle_playlist_rename_request(int cfd, const char * path) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512], new_name[PLAYLIST_NAME_MAX_BYTES + 2];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    const char * error = query_decoded(path, "new_name", new_name, sizeof(new_name)) == 1
                             ? playlist_new_name_error(new_name) : "invalid_name";
    if (error) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", error);
        return;
    }
    char dest[512], revision[20];
    playlist_m3u_path(new_name, dest, sizeof(dest));
    if (strcmp(dest, m3u_path) != 0 && access(dest, F_OK) == 0) {
        send_api_error(cfd, "409 Conflict", "playlist_error", "name_exists");
        return;
    }
    if (!playlist_files_rename(m3u_path, new_name, dest, sizeof(dest))) {
        playlist_send_write_error(cfd, access(dest, F_OK) == 0 && strcmp(dest, m3u_path) != 0);
        return;
    }
    metadata_db_playlist_delete_one(m3u_path);
    metadata_db_playlist_insert_one(dest);
    playlist_send_ok(cfd, new_name, playlist_current_revision(dest, revision) ? revision : NULL);
}

/* POST /api/playlists/delete?name=KEY */
void handle_playlist_delete_request(int cfd, const char * path) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    if (!playlist_files_delete(m3u_path)) {
        send_api_error(cfd, "500 Internal Server Error", "playlist_error", "write_failed");
        return;
    }
    metadata_db_playlist_delete_one(m3u_path);
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 6 + 48];
    json_escape_append(key_esc, sizeof(key_esc), key);
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"deleted\":true}", key_esc);
    send_response(cfd, "200 OK", "application/json", json);
}

typedef struct {
    const char * expected_revision;
    uint64_t from, to;
    bool move;
    const char * refusal;
} playlist_entry_check_t;

/* Runs under the playlist lock with the entries as they are right now. */
static bool playlist_entry_check(void * context, char * const * paths, int count) {
    playlist_entry_check_t * check = context;
    char revision[20];
    playlist_revision_of(paths, count, revision);
    if (strcmp(revision, check->expected_revision) != 0) check->refusal = "revision_mismatch";
    else if (check->from >= (uint64_t) count || (check->move && check->to >= (uint64_t) count))
        check->refusal = "invalid_position";
    return check->refusal == NULL;
}

/* POST /api/playlists/remove?name=KEY&position=N&revision=R and
 * POST /api/playlists/move?name=KEY&from=N&to=N&revision=R. Positions are
 * entry offsets from GET /playlists/songs of that revision; the revision
 * is checked under the same lock as the edit. */
void handle_playlist_entry_request(int cfd, const char * path, bool move) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512], expected[24], revision[20];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    playlist_entry_check_t check = { .expected_revision = expected, .move = move };
    bool valid = query_decoded(path, "revision", expected, sizeof(expected)) == 1 &&
                 (move ? query_required_number(path, "from", INT_MAX, &check.from) &&
                         query_required_number(path, "to", INT_MAX, &check.to)
                       : query_required_number(path, "position", INT_MAX, &check.from));
    if (!valid) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", "invalid_request");
        return;
    }
    if (move && check.from == check.to) {
        /* Nothing to write; still report a stale revision or bad position. */
        char ** paths = NULL;
        int count = 0;
        playlist_read_status_t read = playlist_files_read_ex(m3u_path, &paths, &count);
        bool readable = read == PLAYLIST_READ_OK || read == PLAYLIST_READ_EMPTY;
        bool accepted = readable && playlist_entry_check(&check, paths, count);
        free_path_list(paths, count);
        if (!readable) {
            send_api_error(cfd, "500 Internal Server Error", "playlist_error", "read_failed");
            return;
        }
        if (!accepted) {
            playlist_send_refusal(cfd, check.refusal);
            return;
        }
        playlist_send_ok(cfd, key, expected);
        return;
    }
    playlist_edit_status_t status = playlist_files_edit_entry_checked(
        m3u_path, (int) check.from, move ? (int) check.to : -1, playlist_entry_check, &check);
    if (status == PLAYLIST_EDIT_REFUSED) {
        playlist_send_refusal(cfd, check.refusal);
        return;
    }
    if (status != PLAYLIST_EDIT_OK) {
        send_api_error(cfd, "500 Internal Server Error", "playlist_error", "write_failed");
        return;
    }
    playlist_send_ok(cfd, key, playlist_current_revision(m3u_path, revision) ? revision : NULL);
}

/* Largest uploaded .m3u/.m3u8 body accepted by POST /api/playlists/import. */
#define PLAYLIST_IMPORT_MAX_BYTES (256u * 1024u)

typedef struct {
    int fd;
    size_t consumed;
} body_reader_t;

static ssize_t body_read(void * context, void * buffer, size_t size) {
    body_reader_t * body = context;
    for (;;) {
        struct pollfd pfd = { .fd = body->fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t n = read(body->fd, buffer, size);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) body->consumed += (size_t) n;
        return n;
    }
}

/* Upper bounds for reading the rest of a rejected upload after replying. */
#define IMPORT_LINGER_MAX_BYTES (8u * 1024u * 1024u)
#define IMPORT_LINGER_MAX_SECONDS 10

/* Error reply for an upload whose body was not fully read. Closing a TCP
 * socket with unread data can reset the connection and discard the reply,
 * so send it, half-close, and read what is left of the body (at most
 * `remaining` bytes, bounded in bytes and time) before the caller closes. */
static void import_send_error(int cfd, const char * status, const char * code, size_t remaining) {
    send_api_error(cfd, status, "playlist_error", code);
    if (remaining == 0) return;
    shutdown(cfd, SHUT_WR);
    if (remaining > IMPORT_LINGER_MAX_BYTES) remaining = IMPORT_LINGER_MAX_BYTES;
    time_t give_up = time(NULL) + IMPORT_LINGER_MAX_SECONDS;
    body_reader_t body = { cfd, 0 };
    char buffer[1024];
    while (remaining > 0 && time(NULL) < give_up) {
        ssize_t n = body_read(&body, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (n <= 0) return;
        remaining -= (size_t) n;
    }
}

/* POST /api/playlists/import?name=NAME with the playlist file as the body
 * (Content-Length required). The bytes are stored unchanged as
 * PLAYLISTS_DIR/NAME.m3u; entries keep their own paths, so "matched" counts
 * those that resolve to library songs on this player. */
void handle_playlist_import_request(int cfd, const char * path, long long content_length) {
    if (content_length < 0) {
        /* No length (or chunked): drain whatever follows, bounded. */
        import_send_error(cfd, "411 Length Required", "length_required", IMPORT_LINGER_MAX_BYTES);
        return;
    }
    size_t length = (unsigned long long) content_length > IMPORT_LINGER_MAX_BYTES ? IMPORT_LINGER_MAX_BYTES
                                                                                   : (size_t) content_length;
    if ((unsigned long long) content_length > PLAYLIST_IMPORT_MAX_BYTES) {
        import_send_error(cfd, "413 Content Too Large", "playlist_too_large", length);
        return;
    }
    char name[PLAYLIST_NAME_MAX_BYTES + 2];
    const char * error = query_decoded(path, "name", name, sizeof(name)) == 1 ? playlist_new_name_error(name)
                                                                               : "invalid_name";
    if (error) {
        import_send_error(cfd, "400 Bad Request", error, length);
        return;
    }
    body_reader_t body = { cfd, 0 };
    char m3u_path[512];
    char ** paths = NULL;
    int count = 0;
    playlist_import_status_t status = playlist_files_import_stream(PLAYLISTS_DIR, name, length,
        body_read, &body, m3u_path, sizeof(m3u_path), &paths, &count);
    size_t unread = length - body.consumed;
    switch (status) {
        case PLAYLIST_IMPORT_OK: break;
        case PLAYLIST_IMPORT_EXISTS:
            import_send_error(cfd, "409 Conflict", "name_exists", unread);
            return;
        case PLAYLIST_IMPORT_INVALID_NAME:
            import_send_error(cfd, "400 Bad Request", "invalid_name", unread);
            return;
        case PLAYLIST_IMPORT_INVALID_CONTENT:
            import_send_error(cfd, "400 Bad Request", "invalid_playlist", unread);
            return;
        case PLAYLIST_IMPORT_READ_ERROR:
            import_send_error(cfd, "400 Bad Request", "incomplete_body", unread);
            return;
        case PLAYLIST_IMPORT_IO_ERROR:
        default:
            import_send_error(cfd, "500 Internal Server Error", "write_failed", unread);
            return;
    }
    metadata_db_playlist_insert_one(m3u_path);
    char revision[20];
    playlist_revision_of(paths, count, revision);
    int matched = 0;
    enum { MATCH_BATCH = 32 };
    song_row_t * rows = malloc(sizeof(*rows) * MATCH_BATCH);
    for (int i = 0; rows && i < count; i += MATCH_BATCH) {
        int batch = count - i < MATCH_BATCH ? count - i : MATCH_BATCH;
        metadata_db_get_songs_by_paths((const char * const *) paths + i, batch, rows);
        for (int j = 0; j < batch; j++) if (rows[j].id >= 0) matched++;
    }
    free(rows);
    free_path_list(paths, count);
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 12 + 160];
    json_escape_append(key_esc, sizeof(key_esc), name);
    snprintf(json, sizeof(json),
             "{\"key\":\"%s\",\"name\":\"%s.m3u\",\"revision\":\"%s\",\"entries\":%d,\"matched\":%d}",
             key_esc, key_esc, revision, count, matched);
    send_response(cfd, "200 OK", "application/json", json);
}

/* POST /api/playlists (create with one song) and /api/playlists/add. */
void handle_legacy_playlist_post(int cfd, const char * path_only, const char * path) {
    /* Playlist mutation does real file I/O (playlist_files_create()/_append()/
     * _contains(), potentially slow SD card writes), so it runs outside any
     * status_mutex section: holding that lock would stall gui.c's own
     * per-tick status-push/consume calls on the main thread for the
     * duration of that I/O. The "which song, by index" lookup below is a
     * metadata_db call made before any status_mutex use. */
    char raw_name[REQUEST_LINE_MAX] = {0};
    char name[REQUEST_LINE_MAX] = {0};
    int64_t index = -1;
    bool have_name = query_param_str(path, "name", raw_name, sizeof(raw_name));
    if (have_name) {
        url_decode(raw_name, name, sizeof(name));
        /* A decoded NUL would make validation see only a prefix. */
        for (const char * p = raw_name; p[0] && p[1] && p[2]; p++) {
            if (p[0] == '%' && p[1] == '0' && p[2] == '0') {
                have_name = false;
                break;
            }
        }
    }
    bool have_index = query_param_int64(path, "index", &index);

    char song_path[600] = {0};
    bool have_song = false;
    if (have_index) {
        song_row_t row;
        if (metadata_db_get_song_by_id(index, &row)) {
            snprintf(song_path, sizeof(song_path), "%s", row.path);
            have_song = true;
        }
    }

    if (have_name && playlist_key_is_builtin(name)) {
        /* Built-ins are library views; a file by that name would be unreachable. */
        send_response(cfd, "403 Forbidden", "text/plain", "Playlist is read-only");
    } else if (!have_name || strlen(name) > PLAYLIST_NAME_MAX_BYTES ||
        !playlist_name_is_safe(name) || !have_song) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
    } else if (strcmp(path_only, "/api/playlists") == 0) {
        char created_path[512];
        bool created = playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path));
        if (created) metadata_db_playlist_insert_one(created_path);
        send_response(cfd, created ? "200 OK" : "500 Internal Server Error", "text/plain",
                       created ? "OK" : "Failed to create playlist");
    } else {
        char m3u_path[512];
        if (!playlist_m3u_path(name, m3u_path, sizeof(m3u_path))) {
            send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        } else if (playlist_files_contains(m3u_path, song_path)) {
            send_response(cfd, "200 OK", "text/plain", "Song already in playlist");
        } else {
            bool added = playlist_files_append(m3u_path, song_path);
            send_response(cfd, added ? "200 OK" : "500 Internal Server Error", "text/plain",
                           added ? "OK" : "Failed to add to playlist");
        }
    }
}
