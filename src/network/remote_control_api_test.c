/* Host tests for the Remote Control v1 extension routes: folders, recently
 * played, favorites, queue move/play, and playlist management/import.
 *
 * Includes the real remote_control.c and links the real playlist_files.c;
 * section GC (see the Makefile target) drops the server code these tests do
 * not reach. The library and the Files directory index are small fakes, so
 * these tests cover request validation, error codes, and response shapes,
 * not tagcache itself (metadata-catalog-selftest covers Recently Played). */
#include "../network/remote_control.c"

#include <assert.h>
#include <dirent.h>
#include <sys/socket.h>

player_settings_t current_settings;
void settings_save_async(const player_settings_t * settings) { (void) settings; }

/* ---- Fake library: three songs under ./music, ids 7, 8, 9. ---- */
static const char * const LIBRARY_PATHS[] = { "./music/song.flac", "./music/a/two.flac", "./music/a/three.flac" };
static bool library_favorite[3];

static int library_find(const char * path) {
    for (int i = 0; i < 3; i++) if (strcmp(LIBRARY_PATHS[i], path) == 0) return i;
    return -1;
}

static void library_row(int i, song_row_t * out) {
    memset(out, 0, sizeof(*out));
    out->id = 7 + i;
    snprintf(out->path, sizeof(out->path), "%s", LIBRARY_PATHS[i]);
    snprintf(out->tags.title, sizeof(out->tags.title), "Title %d", 7 + i);
    snprintf(out->tags.artist, sizeof(out->tags.artist), "Artist");
}

bool metadata_db_get_song_by_path(const char * path, song_row_t * out) {
    int i = library_find(path);
    if (i < 0) return false;
    library_row(i, out);
    return true;
}

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out) {
    if (id < 7 || id > 9) return false;
    library_row((int) id - 7, out);
    return true;
}

void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out) {
    for (int i = 0; i < count; i++) if (!metadata_db_get_song_by_path(paths[i], &out[i])) out[i].id = -1;
}

void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out) {
    for (int i = 0; i < count; i++) if (!metadata_db_get_song_by_id(ids[i], &out[i])) out[i].id = -1;
}

metadata_db_catalog_result_t metadata_db_catalog_get_song_revision(const char * revision, int64_t id, song_row_t * out) {
    if (strcmp(revision, "lib:1") != 0) return METADATA_DB_CATALOG_STALE;
    return metadata_db_get_song_by_id(id, out) ? METADATA_DB_CATALOG_OK : METADATA_DB_CATALOG_STALE;
}

void metadata_db_song_display_title(const song_row_t * row, char * out, size_t size) {
    snprintf(out, size, "%s", row->tags.title);
}

bool metadata_db_song_favorite_is_set(const char * path) {
    int i = library_find(path);
    return i >= 0 && library_favorite[i];
}

void metadata_db_song_favorite_set(const char * path, bool is_favorite) {
    int i = library_find(path);
    if (i >= 0) library_favorite[i] = is_favorite;
}

static bool favorite_disk_fails;
bool metadata_db_song_favorite_set_durable(const char * path, bool is_favorite) {
    if (favorite_disk_fails) return false;
    metadata_db_song_favorite_set(path, is_favorite);
    return true;
}

static bool card_mounted = true;
bool sd_card_root_is_mounted(void) { return card_mounted; }

static char ** path_list(int count, const int * which) {
    char ** paths = calloc((size_t) count, sizeof(*paths));
    for (int i = 0; i < count; i++) paths[i] = strdup(LIBRARY_PATHS[which[i]]);
    return paths;
}

void metadata_db_load_favorite_songs(char *** out, int * count) {
    int which[3], n = 0;
    for (int i = 0; i < 3; i++) if (library_favorite[i]) which[n++] = i;
    *out = n ? path_list(n, which) : NULL;
    *count = n;
}

void metadata_db_load_top_played_songs(int limit, char *** out, int * count) {
    (void) limit;
    *out = NULL;
    *count = 0;
}

void metadata_db_load_recently_played_songs(int limit, char *** out, int * count) {
    static const int which[] = { 1, 0 };
    (void) limit;
    *out = path_list(2, which);
    *count = 2;
}

int metadata_db_get_recently_played_page(int offset, int max_rows, song_row_t * rows, int64_t * played, int * total) {
    *total = 2;
    int n = 0;
    for (int i = offset; i < 2 && n < max_rows; i++, n++) {
        library_row(i == 0 ? 1 : 0, &rows[n]);
        if (played) played[n] = i == 0 ? 2000 : 1000;
    }
    return n;
}

static int playlist_cache_inserts, playlist_cache_deletes;
void metadata_db_playlist_insert_one(const char * path) { (void) path; playlist_cache_inserts++; }
void metadata_db_playlist_delete_one(const char * path) { (void) path; playlist_cache_deletes++; }

/* ---- Fake Files index: the browser's filter and order over readdir. ---- */
struct file_browser_index {
    char names[32][256];
    bool dirs[32];
    unsigned count;
};

bool file_browser_is_playable_name(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".flac") == 0 || strcasecmp(ext, ".mp3") == 0);
}

static int fake_entry_cmp(const void * a, const void * b) {
    const char * na = a, * nb = b;
    bool da = na[255] == 1, db = nb[255] == 1;
    if (da != db) return da ? -1 : 1;
    return strcasecmp(na, nb);
}

bool file_browser_index_open(const char * directory, file_browser_index_t ** out, unsigned * out_count) {
    DIR * dir = opendir(directory);
    if (!dir) return false;
    file_browser_index_t * index = calloc(1, sizeof(*index));
    char rows[32][256];
    struct dirent * de;
    while ((de = readdir(dir)) && index->count < 32) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", directory, de->d_name);
        if (lstat(full, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !library_is_m3u_file(de->d_name) && !file_browser_is_playable_name(de->d_name)) continue;
        memset(rows[index->count], 0, 256);
        snprintf(rows[index->count], 255, "%.254s", de->d_name);
        rows[index->count][255] = is_dir ? 1 : 0;
        index->count++;
    }
    closedir(dir);
    qsort(rows, index->count, 256, fake_entry_cmp);
    for (unsigned i = 0; i < index->count; i++) {
        index->dirs[i] = rows[i][255] == 1;
        rows[i][255] = 0;
        snprintf(index->names[i], sizeof(index->names[i]), "%s", rows[i]);
    }
    *out = index;
    if (out_count) *out_count = index->count;
    return true;
}

bool file_browser_index_entry_name(const file_browser_index_t * index, unsigned ordinal, char * name,
                                   size_t size, bool * is_dir) {
    if (ordinal >= index->count) return false;
    snprintf(name, size, "%s", index->names[ordinal]);
    if (is_dir) *is_dir = index->dirs[ordinal];
    return true;
}

void file_browser_index_close(file_browser_index_t * index) { free(index); }

/* ---- Request driver ---- */
static char response[256 * 1024];

/* Sends one request through handle_extension_route() (or the playlist songs
 * handler) and returns the HTTP status; response holds the body. */
static int request_body(const char * method, const char * target, const char * body, long long length) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    if (body) assert(write(fds[0], body, strlen(body)) == (ssize_t) strlen(body));
    char path_only[REQUEST_LINE_MAX];
    snprintf(path_only, sizeof(path_only), "%s", target);
    char * q = strchr(path_only, '?');
    if (q) *q = '\0';
    if (strcmp(path_only, "/api/playlists/songs") == 0) handle_playlist_songs_request(fds[1], target);
    else assert(handle_extension_route(fds[1], method, path_only, target, length));
    shutdown(fds[1], SHUT_WR);
    size_t used = 0;
    ssize_t n;
    while ((n = read(fds[0], response + used, sizeof(response) - 1 - used)) > 0) used += (size_t) n;
    response[used] = '\0';
    close(fds[0]);
    close(fds[1]);
    int status = 0;
    assert(sscanf(response, "HTTP/1.1 %d", &status) == 1);
    char * split = strstr(response, "\r\n\r\n");
    assert(split);
    memmove(response, split + 4, strlen(split + 4) + 1);
    return status;
}

static int request(const char * method, const char * target) {
    return request_body(method, target, NULL, -1);
}

static bool has(const char * text) { return strstr(response, text) != NULL; }

static void write_file(const char * path, const char * text) {
    FILE * f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static void test_folders(void) {
    assert(request("GET", "/api/folders/roots") == 200);
    assert(has("\"id\":\"sd\"") && has("\"available\":true"));
    card_mounted = false;
    assert(request("GET", "/api/folders/roots") == 200 && has("\"available\":false"));
    assert(request("GET", "/api/folders") == 503 && has("storage_unavailable"));
    card_mounted = true;

    assert(request("GET", "/api/folders") == 200);
    assert(has("\"path\":\"\",\"parent\":null,\"total\":6,\"folder_count\":3"));
    /* Folders first, then files by name; hidden and non-audio entries omitted. */
    assert(strstr(response, "\"name\":\"a\"") < strstr(response, "\"name\":\"B\""));
    assert(strstr(response, "\"name\":\"B\"") < strstr(response, "\"name\":\"Playlists\""));
    assert(strstr(response, "\"name\":\"Playlists\"") < strstr(response, "list.m3u"));
    assert(has("{\"type\":\"playlist\",\"name\":\"list.m3u\"}"));
    assert(has("{\"type\":\"track\",\"name\":\"song.flac\",\"index\":7,\"title\":\"Title 7\",\"artist\":\"Artist\"}"));
    assert(has("{\"type\":\"track\",\"name\":\"x.mp3\",\"index\":-1,\"title\":\"x.mp3\",\"artist\":\"\"}"));
    assert(!has("hidden") && !has("notes.txt"));
    assert(has("\"next_offset\":6"));

    assert(request("GET", "/api/folders?root=sd&path=%2F&offset=1&limit=2") == 200);
    assert(has("\"offset\":1,\"entries\":[{\"type\":\"folder\",\"name\":\"B\"}") && has("\"next_offset\":3"));
    assert(request("GET", "/api/folders?path=a") == 200);
    assert(has("\"path\":\"a\",\"parent\":\"\"") && has("\"index\":9") && has("\"index\":8"));
    assert(request("GET", "/api/folders?path=a/sub%20dir") == 200 && has("\"parent\":\"a\""));
    assert(request("GET", "/api/folders?offset=99") == 200 && has("\"entries\":[]") && has("\"next_offset\":6"));

    const char * bad[] = { "?path=..", "?path=a/../..", "?path=.hidden", "?path=a//b", "?path=a/",
                           "?root=usb", "?path=%zz", "?path=a%00b", "?path=a%5Cb", "?limit=0", "?limit=101",
                           "?offset=-1" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char target[64];
        snprintf(target, sizeof(target), "/api/folders%s", bad[i]);
        assert(request("GET", target) == 400);
        assert(has("\"error\":\"folder_error\""));
    }
    assert(request("GET", "/api/folders?path=missing") == 404 && has("folder_not_found"));
    assert(request("GET", "/api/folders?path=song.flac") == 404);
    assert(symlink("..", "music/escape") == 0);
    assert(request("GET", "/api/folders?path=escape") == 404 && has("folder_not_found"));
    write_file("outside.flac", "");
    assert(request("POST", "/api/folders/play?path=escape/outside.flac") == 404);
    assert(unlink("music/escape") == 0);

    assert(request("POST", "/api/folders/play?path=a/two.flac") == 200);
    char played[REMOTE_CONTROL_PATH_MAX];
    assert(remote_control_consume_folder_play(played, sizeof(played)) && strcmp(played, "./music/a/two.flac") == 0);
    assert(!remote_control_consume_folder_play(played, sizeof(played)));
    assert(request("POST", "/api/folders/play?path=list.m3u") == 400 && has("not_playable"));
    assert(request("POST", "/api/folders/play?path=gone.flac") == 404 && has("track_not_found"));
    assert(request("POST", "/api/folders/play?path=a") == 404);
    assert(request("POST", "/api/folders/play?path=") == 400 && has("invalid_path"));
}

static void test_recent(void) {
    assert(request("GET", "/api/recent") == 200);
    assert(has("{\"total\":2,\"offset\":0,\"songs\":[{\"index\":8,\"title\":\"Title 8\",\"artist\":\"Artist\",\"last_played\":2000}"));
    assert(has("\"next_offset\":2"));
    assert(request("GET", "/api/recent?offset=1&limit=1") == 200 && has("\"index\":7") && has("\"next_offset\":2"));
    assert(request("GET", "/api/recent?limit=500") == 400 && has("recent_error") && has("invalid_paging"));
    assert(request("GET", "/api/playlists/songs?name=%40recently_played") == 200);
    assert(has("\"writable\":false,\"revision\":null") && strstr(response, "\"index\":8") < strstr(response, "\"index\":7"));
    assert(has("\"total\":2,\"next_offset\":2"));
    assert(request("GET", "/api/playlists/songs?name=%40recently_played&offset=1") == 200);
    assert(!has("\"index\":8") && has("\"index\":7,\"title\":\"Title 7\",\"artist\":\"Artist\",\"position\":1"));
    assert(request("GET", "/api/playlists/songs?name=%40recently_played&offset=-1") == 400);
}

static void test_favorites(void) {
    memset(library_favorite, 0, sizeof(library_favorite));
    /* A player heart tap still inside its debounce window is superseded by
     * the later remote write instead of landing after it. */
    favorite_writer_submit("./music/a/two.flac", false);
    assert(request("POST", "/api/favorites/add?index=8") == 200 && has("{\"index\":8,\"favorite\":true}"));
    assert(library_favorite[1]);
    usleep(400 * 1000); /* past the heart's debounce window */
    assert(library_favorite[1] && favorite_writer_is_set("./music/a/two.flac"));
    assert(remote_control_consume_favorite_changed() && !remote_control_consume_favorite_changed());
    /* @favorites is derived from the same flag. */
    assert(request("GET", "/api/playlists/songs?name=%40favorites") == 200 && has("\"index\":8"));
    assert(request("GET", "/api/favorites/state?ids=7,8,99") == 200);
    assert(has("{\"index\":7,\"found\":true,\"favorite\":false}") &&
           has("{\"index\":8,\"found\":true,\"favorite\":true}") &&
           has("{\"index\":99,\"found\":false,\"favorite\":false}"));
    const char * bad_ids[] = { "", "?ids=", "?ids=7,,8", "?ids=7,", "?ids=-1", "?ids=0", "?ids=abc", "?ids=9999999999" };
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        char target[64];
        snprintf(target, sizeof(target), "/api/favorites/state%s", bad_ids[i]);
        assert(request("GET", target) == 400 && has("invalid_ids"));
    }
    assert(request("POST", "/api/favorites/remove?index=8&catalog_revision=lib%3A1") == 200 &&
           has("\"favorite\":false") && !library_favorite[1]);
    assert(request("POST", "/api/favorites/add?index=8&catalog_revision=lib%3A0") == 409 && has("revision_mismatch"));
    assert(request("POST", "/api/favorites/add?index=42") == 404 && has("song_not_found"));
    assert(request("POST", "/api/favorites/add?index=0") == 400 && has("invalid_index"));
    assert(request("POST", "/api/favorites/add") == 400 && has("invalid_target"));
    assert(request("POST", "/api/favorites/add?index=7&current=1") == 400 && has("invalid_target"));

    snprintf(status_path, sizeof(status_path), "%s", "");
    assert(request("POST", "/api/favorites/add?current=1") == 404 && has("nothing_playing"));
    snprintf(status_path, sizeof(status_path), "%s", "./music/song.flac");
    assert(request("POST", "/api/favorites/add?current=1") == 200 && has("{\"index\":7,\"favorite\":true}"));
    assert(status_favorite && library_favorite[0]);
    assert(remote_control_consume_favorite_changed());
    favorite_disk_fails = true;
    assert(request("POST", "/api/favorites/remove?current=1") == 503 && has("write_failed"));
    favorite_disk_fails = false;
    status_path[0] = '\0';
    status_favorite = false;
}

static void test_queue(void) {
    const char * queued[] = { "./music/song.flac", "./music/a/two.flac", "./music/a/three.flac" };
    remote_control_sync_queue(queued, 3, 5);
    int from = -1, to = -1;
    uint64_t revision = 0;
    assert(request("POST", "/api/queue/move?from=0&to=2&revision=5") == 200);
    assert(remote_control_consume_queue_move(&from, &to, &revision) && from == 0 && to == 2 && revision == 5);
    assert(!remote_control_consume_queue_move(&from, &to, &revision));
    assert(request("POST", "/api/queue/move?from=0&to=2&revision=4") == 409 && has("revision_mismatch"));
    assert(request("POST", "/api/queue/move?from=3&to=0&revision=5") == 400 && has("invalid_offset"));
    assert(request("POST", "/api/queue/move?from=0&revision=5") == 400 && has("invalid_request"));
    assert(request("POST", "/api/queue/move?from=1&to=0&revision=5") == 200);
    assert(request("POST", "/api/queue/play?offset=1&revision=5") == 409 && has("edit_pending"));
    assert(remote_control_consume_queue_move(&from, &to, &revision) && from == 1 && to == 0);
    assert(request("POST", "/api/queue/play?offset=1&revision=5") == 200);
    assert(remote_control_consume_queue_play(&from, &revision) && from == 1 && revision == 5);
    assert(request("POST", "/api/queue/play?offset=-1&revision=5") == 400);
    assert(request("POST", "/api/queue/play?offset=0") == 400);
    assert(!remote_control_consume_queue_play(&from, &revision));
}

static void read_revision(char out[24]) {
    const char * r = strstr(response, "\"revision\":\"");
    assert(r);
    snprintf(out, 20, "%.18s", r + 12);
}

static void test_playlists(void) {
    char revision[24], target[256];
    assert(request("POST", "/api/playlists/create?name=Gym") == 200);
    assert(has("\"key\":\"Gym\",\"name\":\"Gym.m3u\"") && access("./music/Playlists/Gym.m3u", F_OK) == 0);
    assert(playlist_cache_inserts == 1);
    assert(request("POST", "/api/playlists/create?name=Gym") == 409 && has("name_exists"));
    assert(request("POST", "/api/playlists/create?name=%40mine") == 400 && has("reserved_name"));
    assert(request("POST", "/api/playlists/create?name=Favorites") == 400 && has("reserved_name"));
    const char * bad_names[] = { "", "?name=", "?name=a.m3u", "?name=a%2Fb", "?name=..x", "?name=.x", "?name=a%3Ab",
                                 "?name=trail." };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        snprintf(target, sizeof(target), "/api/playlists/create%s", bad_names[i]);
        assert(request("POST", target) == 400 && has("invalid_name"));
    }

    /* Built-ins are read only on every mutation route, including the old add route's name check. */
    assert(request("POST", "/api/playlists/delete?name=%40favorites") == 403 && has("read_only"));
    assert(request("POST", "/api/playlists/rename?name=Most%20Played&new_name=X") == 403);
    assert(request("POST", "/api/playlists/remove?name=%40recently_played&position=0&revision=p-0") == 403);
    assert(request("POST", "/api/playlists/delete?name=Nope") == 404 && has("playlist_not_found"));

    write_file("./music/Playlists/Gym.m3u", "#EXTM3U\n../song.flac\n../missing.flac\n../a/two.flac\n");
    assert(request("GET", "/api/playlists/songs?name=Gym") == 200);
    assert(has("\"writable\":true") && has("\"index\":7,\"title\":\"Title 7\",\"artist\":\"Artist\",\"position\":0"));
    assert(has("\"index\":8,\"title\":\"Title 8\",\"artist\":\"Artist\",\"position\":2") && !has("missing"));
    read_revision(revision);

    assert(request("POST", "/api/playlists/move?name=Gym&from=2&to=0&revision=p-0000000000000000") == 409 &&
           has("revision_mismatch"));
    snprintf(target, sizeof(target), "/api/playlists/move?name=Gym&from=3&to=0&revision=%s", revision);
    assert(request("POST", target) == 400 && has("invalid_position"));
    snprintf(target, sizeof(target), "/api/playlists/move?name=Gym&from=2&to=0&revision=%s", revision);
    assert(request("POST", target) == 200);
    char moved[24];
    read_revision(moved);
    assert(strcmp(moved, revision) != 0);
    /* The old revision is now stale even though the file size is unchanged. */
    assert(request("POST", target) == 409);
    assert(request("GET", "/api/playlists/songs?name=Gym") == 200 && has("\"index\":8,\"title\":\"Title 8\",\"artist\":\"Artist\",\"position\":0"));
    snprintf(target, sizeof(target), "/api/playlists/remove?name=Gym&position=1&revision=%s", moved);
    assert(request("POST", target) == 200);
    assert(request("GET", "/api/playlists/songs?name=Gym") == 200 && !has("\"index\":7"));
    assert(request("POST", "/api/playlists/remove?name=Gym&position=0") == 400 && has("invalid_request"));

    FILE * big = fopen("./music/Playlists/Big.m3u", "w");
    assert(big);
    for (int i = 0; i < 2000; i++) fputs("../song.flac\n", big);
    fclose(big);
    assert(request("GET", "/api/playlists/songs?name=Big") == 200);
    int total = 0, next = 0;
    const char * tail = strstr(response, "],\"total\":");
    assert(tail && sscanf(tail, "],\"total\":%d,\"next_offset\":%d}", &total, &next) == 2);
    assert(total == 2000 && next > 0 && next < total && response[strlen(response) - 1] == '}');
    char more[128];
    snprintf(more, sizeof(more), "/api/playlists/songs?name=Big&offset=%d", next);
    assert(request("GET", more) == 200);
    char first_position[32];
    snprintf(first_position, sizeof(first_position), "\"position\":%d}", next);
    assert(has(first_position));
    assert(unlink("./music/Playlists/Big.m3u") == 0);
    assert(request("POST", "/api/playlists/rename?name=Gym&new_name=Run") == 200 && has("\"key\":\"Run\""));
    assert(access("./music/Playlists/Run.m3u", F_OK) == 0 && access("./music/Playlists/Gym.m3u", F_OK) != 0);
    assert(request("POST", "/api/playlists/create?name=Walk") == 200);
    assert(request("POST", "/api/playlists/rename?name=Run&new_name=Walk") == 409 && has("name_exists"));
    assert(request("POST", "/api/playlists/rename?name=Run&new_name=a%2Fb") == 400 && has("invalid_name"));
    assert(request("POST", "/api/playlists/delete?name=Walk") == 200 && has("{\"key\":\"Walk\",\"deleted\":true}"));
    assert(access("./music/Playlists/Walk.m3u", F_OK) != 0 && playlist_cache_deletes == 2);

    const char * upload = "#EXTM3U\r\n../song.flac\r\n/phone/only.flac\r\n";
    assert(request_body("POST", "/api/playlists/import?name=Phone", upload, (long long) strlen(upload)) == 200);
    assert(has("\"key\":\"Phone\",\"name\":\"Phone.m3u\"") && has("\"entries\":2,\"matched\":1"));
    assert(request_body("POST", "/api/playlists/import?name=Phone", upload, (long long) strlen(upload)) == 409 &&
           has("name_exists"));
    assert(request_body("POST", "/api/playlists/import?name=%40x", upload, (long long) strlen(upload)) == 400 &&
           has("reserved_name"));
    assert(request("POST", "/api/playlists/import?name=NoLength") == 411 && has("length_required"));
    assert(request_body("POST", "/api/playlists/import?name=Big", NULL, PLAYLIST_IMPORT_MAX_BYTES + 1) == 413);
    assert(request_body("POST", "/api/playlists/import?name=Short", "abc", 10) == 400 && has("incomplete_body"));
    assert(access("./music/Playlists/Short.m3u", F_OK) != 0);
}

int main(void) {
    char root[] = "/tmp/compas-remote-api-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    assert(mkdir("music", 0755) == 0 && mkdir("music/a", 0755) == 0 && mkdir("music/B", 0755) == 0 &&
           mkdir("music/.hidden", 0755) == 0 && mkdir("music/a/sub dir", 0755) == 0 &&
           mkdir("music/Playlists", 0755) == 0);
    write_file("music/song.flac", "");
    write_file("music/x.mp3", "");
    write_file("music/list.m3u", "song.flac\n");
    write_file("music/notes.txt", "");
    write_file("music/a/two.flac", "");
    write_file("music/a/three.flac", "");
    test_folders();
    test_recent();
    test_favorites();
    test_queue();
    test_playlists();
    printf("Remote Control API extension tests passed (%s)\n", root);
    return 0;
}
