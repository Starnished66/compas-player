#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_download_hash.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/text/utf8_util.h"
#include "../../../core/process/worker_notify.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../network/services/subsonic_client.h"
#include "../../../network/http/client/http_client.h"
#include "../../../network/download/download.h"
#include "../../../core/files/file_integrity.h"
#include "../../../core/system/logging/debug_log.h"
#include "../../../library/artwork/albumart/albumart.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern player_settings_t current_settings;

typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} subsonic_track_download_request_t;

typedef struct {
    subsonic_server_t server;
    subsonic_song_t * songs;
    int song_count;
    char ** album_ids; /* Mode B: our own copies, the album rows may be freed under us */
    int album_to_expand_count;
    char playlist_name[128]; 
    char download_subfolder[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    int download_layout;
} subsonic_library_download_request_t;
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define SUBSONIC_FILE_CONNECT_TIMEOUT_MS 10000U
#define SUBSONIC_FILE_READ_TIMEOUT_MS 30000U

static char download_dest_path[512];
static atomic_bool download_done_flag = false;
static bool download_success_flag = false;
static bool download_active = false;
static gui_busy_handle_t download_token = 0;

static gui_busy_handle_t subsonic_library_download_token = 0;

static pthread_t download_thread;
static http_cancel_token_t download_cancel;

static bool subsonic_download_file(const char *url, bool verify_tls, const char *path,
    http_cancel_token_t *cancel) {
    download_request_t request = {.url = url, .dest_path = path, .max_bytes = UINT64_C(2) << 30,
        .verify_tls = verify_tls, .connect_timeout_ms = SUBSONIC_FILE_CONNECT_TIMEOUT_MS,
        .read_timeout_ms = SUBSONIC_FILE_READ_TIMEOUT_MS};
    download_result_t result;
    download_error_t error = download_to_file(&request, cancel, NULL, NULL, &result);
    if (error == DOWNLOAD_ERR_DIR_SYNC && result.committed) {
        DBG_LOG("subsonic: song committed but directory sync failed: %s\n", path);
        return true;
    }
    return error == DOWNLOAD_OK;
}

static void * download_thread_func(void * arg) {
    subsonic_track_download_request_t * req = (subsonic_track_download_request_t *) arg;
    bool ok = subsonic_download_file(req->url, req->verify_tls, req->dest_path, &download_cancel);
    download_success_flag = ok;
    atomic_store_explicit(&download_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    worker_notify_ui();
    free(req);
    return NULL;
}

void gui_subsonic_download_track(const char * url, bool verify_tls, const char * dest_path, const char * display_title) {
    snprintf(download_dest_path, sizeof(download_dest_path), "%s", dest_path);

    subsonic_track_download_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->dest_path, sizeof(req->dest_path), "%s", dest_path);
    req->verify_tls = verify_tls;

    atomic_store_explicit(&download_done_flag, false, memory_order_relaxed);
    download_success_flag = false;
    http_cancel_token_init(&download_cancel);
    download_active = true;

    download_token = gui_busy_show(TR("Downloading"), display_title);

    if (pthread_create(&download_thread, NULL, download_thread_func, req) != 0) {
        download_active = false;
        http_cancel_token_destroy(&download_cancel);
        free(req);
        gui_busy_hide(download_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_subsonic_download(void) {
    if (!download_active || !atomic_load_explicit(&download_done_flag, memory_order_acquire)) return;

    download_active = false;
    pthread_join(download_thread, NULL);
    bool cancelled = http_cancel_token_is_cancelled(&download_cancel); /* Wi-Fi off, shutdown or the user */
    http_cancel_token_destroy(&download_cancel);
    bool success = download_success_flag;

    /* If playback started and pushed the player screen, remove the downloading
     * screen slot from the navigation stack so navigating back returns directly
     * to the song list rather than the transient downloading screen. */
    int depth_before = gui_navigation_get_depth();
    if (success) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(download_dest_path);
        clear_player_source(); /* a streamed-then-downloaded single track has no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    subsonic_dismiss_busy(depth_before, download_token);
    if (!success && !cancelled) show_error_toast(TR("Download failed"));
}

static void sanitize_path_component(const char * in, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    utf8_truncate_safe_bounded(out, out_size, in ? in : "", in ? strlen(in) : 0);
    for (size_t i = 0; out[i]; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c == '/' || c == '\\' || strchr(":*?\"<>|", c) || c < 0x20 || c == 0x7F ||
            (i == 0 && c == '.')) out[i] = '_';
    }
}

static void subsonic_song_folder_names(const subsonic_song_t * song,
                                        char artist[160], char album[160]) {
    sanitize_path_component(song->album_artist[0] ? song->album_artist :
                            (song->artist[0] ? song->artist : "Unknown Artist"), artist, 160);
    sanitize_path_component(song->album[0] ? song->album : "Unknown Album", album, 160);
}

/* Create/open one component relative to an already-open directory. Refuse
 * symlinks while walking the configured path and album layout. */
static bool subsonic_enter_directory(int * dirfd, char * path, size_t path_size, const char * component) {
    if (!dirfd || *dirfd < 0 || !component || !component[0] || component[0] == '.' ||
        strchr(component, '/') || strlen(component) > 255) return false;
    for (const unsigned char * p = (const unsigned char *)component; *p; p++)
        if (*p < 0x20 || *p == 0x7F || *p == '\\') return false;
    if (mkdirat(*dirfd, component, 0755) != 0 && errno != EEXIST) return false;
    int child = openat(*dirfd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child < 0) return false;
    size_t used = strlen(path);
    if (used >= path_size) { close(child); return false; }
    int n = snprintf(path + used, path_size - used, "/%s", component);
    if (n < 0 || (size_t)n >= path_size - used) { close(child); return false; }
    close(*dirfd);
    *dirfd = child;
    return true;
}

static int subsonic_open_subfolder(const char * subfolder, char * out, size_t out_size) {
    char checked_subfolder[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    if (!settings_validate_subsonic_download_subfolder(subfolder, checked_subfolder,
                                                       sizeof(checked_subfolder))) return -1;
    int dirfd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return -1;
    int n = snprintf(out, out_size, "%s", MUSIC_ROOT_DIR);
    if (n < 0 || (size_t)n >= out_size) { close(dirfd); return -1; }

    for (char * component = checked_subfolder; component[0]; ) {
        char * slash = strchr(component, '/');
        if (slash) *slash = '\0';
        if (!subsonic_enter_directory(&dirfd, out, out_size, component)) { close(dirfd); return -1; }
        if (!slash) break;
        component = slash + 1;
    }
    return dirfd;
}

static bool subsonic_combined_album_folder(const char * safe_artist, const char * safe_album,
                                           char combined[256]) {
    char full_name[1024];
    int n = snprintf(full_name, sizeof(full_name), "%s - %s", safe_artist, safe_album);
    if (n < 0 || (size_t)n >= sizeof(full_name)) return false;
    size_t full_length = (size_t)n;
    if (full_length <= 255) {
        memcpy(combined, full_name, full_length + 1);
    } else {
        char hash_suffix[18], prefix[256];
        snprintf(hash_suffix, sizeof(hash_suffix), "~%016llx",
                 (unsigned long long)subsonic_album_folder_hash(safe_artist, safe_album));
        size_t prefix_budget = 255 - strlen(hash_suffix);
        utf8_truncate_safe(prefix, full_name, prefix_budget + 1);
        int combined_length = snprintf(combined, 256, "%s%s", prefix, hash_suffix);
        if (combined_length < 0 || combined_length > 255) return false;
    }
    return true;
}

static bool subsonic_ensure_library_album_dir(const char * subfolder, int layout,
                                               const char * safe_artist, const char * safe_album,
                                               char * out, size_t out_size) {
    int dirfd = subsonic_open_subfolder(subfolder, out, out_size);
    if (dirfd < 0) return false;
    if (layout == 1) {
        char combined[256];
        if (!subsonic_combined_album_folder(safe_artist, safe_album, combined)) { close(dirfd); return false; }
        if (!subsonic_enter_directory(&dirfd, out, out_size, combined)) { close(dirfd); return false; }
    } else {
        if (!subsonic_enter_directory(&dirfd, out, out_size, safe_artist) ||
            !subsonic_enter_directory(&dirfd, out, out_size, safe_album)) { close(dirfd); return false; }
    }
    close(dirfd);
    return true;
}

static void subsonic_track_number_prefix(const subsonic_song_t * song, char * number_prefix, size_t size) {
    if (song->disc > 1 && song->track > 0)
        snprintf(number_prefix, size, "%d-%02d - ", song->disc, song->track);
    else if (song->disc > 1)
        snprintf(number_prefix, size, "%d - ", song->disc);
    else if (song->track > 0)
        snprintf(number_prefix, size, "%02d - ", song->track);
    else
        number_prefix[0] = '\0';
}

static void subsonic_shorten_download_name(const subsonic_song_t * song, bool distinct_artist,
                                           const char * number_prefix, const char * safe_title,
                                           const char * safe_track_artist, const char * safe_suffix,
                                           char * full_name, size_t size) {
    if (strlen(full_name) <= 255) return;
    char hash_tag[18];
    snprintf(hash_tag, sizeof(hash_tag), "~%016llx", (unsigned long long) subsonic_song_id_hash(song->id));
    /* Reserve separators, extension and ID before budgeting either text
     * field. The ID keeps shortened compilation names distinct. */
    size_t budget = 255 - strlen(number_prefix) - strlen(hash_tag) - strlen(safe_suffix) - 1;
    char short_artist[160] = "";
    if (distinct_artist) {
        utf8_truncate_safe(short_artist, safe_track_artist, budget / 3 + 1);
        budget -= strlen(short_artist) + 3;
    }
    char short_title[160];
    utf8_truncate_safe(short_title, safe_title,
                       budget + 1 < sizeof(short_title) ? budget + 1 : sizeof(short_title));
    if (distinct_artist)
        snprintf(full_name, size, "%s%s - %s%s.%s",
                 number_prefix, short_title, short_artist, hash_tag, safe_suffix);
    else
        snprintf(full_name, size, "%s%s%s.%s",
                 number_prefix, short_title, hash_tag, safe_suffix);
}

/* Build a distinct, filesystem-safe filename while keeping the familiar
 * single-disc artist/album layout. Album-artist folders combine compilation
 * tracks, so include the track artist only when it differs from that folder
 * key; disc numbers also prevent repeated track numbers on multi-disc sets
 * from overwriting each other. */
static void subsonic_build_download_filename(const subsonic_song_t * song, const char * safe_title,
                                              char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    char safe_track_artist[160];
    bool distinct_artist = song->artist[0] && strcmp(song->artist, song->album_artist) != 0;
    if (distinct_artist) sanitize_path_component(song->artist, safe_track_artist, sizeof(safe_track_artist));
    else safe_track_artist[0] = '\0';
    char safe_suffix[32];
    sanitize_path_component(song->suffix, safe_suffix, sizeof(safe_suffix));

    char number_prefix[32];
    subsonic_track_number_prefix(song, number_prefix, sizeof(number_prefix));

    char full_name[512];
    if (distinct_artist)
        snprintf(full_name, sizeof(full_name), "%s%s - %s.%s", number_prefix, safe_title, safe_track_artist, safe_suffix);
    else
        snprintf(full_name, sizeof(full_name), "%s%s.%s", number_prefix, safe_title, safe_suffix);

    subsonic_shorten_download_name(song, distinct_artist, number_prefix, safe_title,
                                   safe_track_artist, safe_suffix, full_name, sizeof(full_name));
    utf8_truncate_safe(out, full_name, out_size);
}

typedef enum {
    SUBSONIC_COVER_FORMAT_UNKNOWN = 0,
    SUBSONIC_COVER_FORMAT_JPEG,
    SUBSONIC_COVER_FORMAT_PNG
} subsonic_cover_format_t;

static subsonic_cover_format_t subsonic_detect_image_format(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return SUBSONIC_COVER_FORMAT_UNKNOWN;
    uint8_t buf[8];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    /* JPEG starts with SOI marker 0xFF 0xD8, followed by marker prefix 0xFF */
    if (n >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
        return SUBSONIC_COVER_FORMAT_JPEG;
    }

    /* PNG starts with standard 8-byte signature */
    static const uint8_t png_magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n >= 8 && memcmp(buf, png_magic, 8) == 0) {
        return SUBSONIC_COVER_FORMAT_PNG;
    }

    return SUBSONIC_COVER_FORMAT_UNKNOWN;
}

/* True when the library would already find art for a song in album_dir: the
 * same sidecar search it uses (album name, cover.*, folder.*), asked about a
 * probe path inside that folder. */
static bool subsonic_album_folder_has_art(const char * album_dir, const subsonic_song_t * song) {
    albumart_info_t info;
    memset(&info, 0, sizeof(info));
    int n = snprintf(info.path, sizeof(info.path), "%s/art_probe.mp3", album_dir);
    if (n < 0 || (size_t) n >= sizeof(info.path)) return true; /* too long to probe: leave the folder alone */
    if (song->album[0]) snprintf(info.album, sizeof(info.album), "%s", song->album);
    if (song->album_artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", song->album_artist);
    else if (song->artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", song->artist);
    char found[PATH_MAX];
    return albumart_search_source_files(&info, "", found, sizeof(found));
}

typedef struct {
    char ** dirs;
    int count;
    int capacity;
} album_dir_tracker_t;

static bool album_dir_tracker_contains(const album_dir_tracker_t * tracker, const char * dir) {
    if (!tracker || !tracker->dirs) return false;
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->dirs[i] && strcmp(tracker->dirs[i], dir) == 0) return true;
    }
    return false;
}

static void album_dir_tracker_add(album_dir_tracker_t * tracker, const char * dir) {
    if (!tracker) return;
    if (tracker->count >= tracker->capacity) {
        int new_capacity = (tracker->capacity == 0) ? 8 : (tracker->capacity * 2);
        char ** new_dirs = realloc(tracker->dirs, sizeof(char *) * (size_t) new_capacity);
        if (!new_dirs) return;
        tracker->dirs = new_dirs;
        tracker->capacity = new_capacity;
    }
    char * copy = strdup(dir);
    if (!copy) return;
    tracker->dirs[tracker->count++] = copy;
}

static void album_dir_tracker_free(album_dir_tracker_t * tracker) {
    if (!tracker) return;
    for (int i = 0; i < tracker->count; i++) {
        free(tracker->dirs[i]);
    }
    free(tracker->dirs);
    tracker->dirs = NULL;
    tracker->count = 0;
    tracker->capacity = 0;
}

static const char * subsonic_album_cover_id(const subsonic_song_t * songs, int song_count,
                                            int current_song_index, const char * safe_artist,
                                            const char * safe_album) {
    const char * cover_art_id = songs[current_song_index].cover_art;
    if (cover_art_id[0] == '\0') {
        for (int k = 0; k < song_count; k++) {
            if (songs[k].cover_art[0] == '\0') continue;
            char s_artist[160], s_album[160];
            subsonic_song_folder_names(&songs[k], s_artist, s_album);
            if (strcmp(s_artist, safe_artist) == 0 && strcmp(s_album, safe_album) == 0) {
                cover_art_id = songs[k].cover_art;
                break;
            }
        }
    }
    return cover_art_id;
}

static bool subsonic_download_cover_temp(const subsonic_server_t * server, const char * cover_art_id,
                                          const char * album_dir, http_cancel_token_t * cancel,
                                          char temp_path[1024]) {
    char cover_url[1536];
    subsonic_build_cover_art_url(server, cover_art_id, 0, cover_url, sizeof(cover_url)); /* saved next to the files: keep the original */


    int n = snprintf(temp_path, 1024, "%s/cover.tmp", album_dir);
    if (n < 0 || (size_t) n >= 1024) return false;
    remove(temp_path);

    download_request_t request = {.url = cover_url, .dest_path = temp_path,
        .max_bytes = 20u << 20, .verify_tls = server->verify_tls, .max_redirects = 3,
        .connect_timeout_ms = SUBSONIC_FILE_CONNECT_TIMEOUT_MS,
        .read_timeout_ms = SUBSONIC_FILE_READ_TIMEOUT_MS, .required_status = 200};
    download_result_t result;
    download_error_t error = download_to_file(&request, cancel, NULL, NULL, &result);
    if (error == DOWNLOAD_ERR_DIR_SYNC && result.committed)
        DBG_LOG("subsonic: cover committed but directory sync failed: %s\n", temp_path);
    bool ok = error == DOWNLOAD_OK || (error == DOWNLOAD_ERR_DIR_SYNC && result.committed);
    if (!ok) {
        remove(temp_path);
        return false;
    }

    return true;
}

static void subsonic_install_cover_file(const char * album_dir, const char * temp_path) {
    char dest_cover[1024];
    subsonic_cover_format_t fmt = subsonic_detect_image_format(temp_path);
    if (fmt == SUBSONIC_COVER_FORMAT_UNKNOWN) {
        remove(temp_path);
        return;
    }

    int n = snprintf(dest_cover, sizeof(dest_cover), "%s/cover.%s", album_dir,
                 fmt == SUBSONIC_COVER_FORMAT_JPEG ? "jpg" : "png");
    if (n < 0 || (size_t) n >= sizeof(dest_cover) || access(dest_cover, F_OK) == 0) {
        remove(temp_path);
        return;
    }

    if (rename(temp_path, dest_cover) != 0) {
        remove(temp_path);
    } else if (!filesystem_sync_parent(dest_cover)) {
        DBG_LOG("subsonic: cover committed but directory sync failed: %s\n", dest_cover);
    }
}

static void subsonic_fetch_album_artwork_if_needed(const subsonic_server_t * server,
                                                   const char * album_dir,
                                                   const subsonic_song_t * songs,
                                                   int song_count,
                                                   int current_song_index,
                                                   const char * safe_artist,
                                                   const char * safe_album,
                                                   http_cancel_token_t * cancel) {
    if (http_cancel_token_is_cancelled(cancel)) return;

    if (subsonic_album_folder_has_art(album_dir, &songs[current_song_index])) return;

    const char * cover_art_id = subsonic_album_cover_id(songs, song_count, current_song_index,
                                                       safe_artist, safe_album);
    if (cover_art_id[0] == '\0') return;

    char temp_path[1024];
    if (!subsonic_download_cover_temp(server, cover_art_id, album_dir, cancel, temp_path)) return;
    subsonic_install_cover_file(album_dir, temp_path);
}


static pthread_t subsonic_library_download_thread;
static http_cancel_token_t subsonic_library_download_cancel;

static bool subsonic_library_download_active = false;

static atomic_bool subsonic_library_download_done_flag = false;

static atomic_int subsonic_library_download_progress = 0; /* songs completed so far */

static atomic_int subsonic_library_download_total = 0;    /* 0 while still expanding an artist's albums (Mode B) -- see poll_subsonic_library_download() */

static int subsonic_library_download_success_count = 0;

static void free_id_list(char ** ids, int count) {
    for (int i = 0; ids && i < count; i++) free(ids[i]);
    free(ids);
}

/* Own copies of the ids: the album rows belong to the screen and can be freed by a reload. */
char ** gui_subsonic_copy_album_ids(const subsonic_album_t * albums, int count) {
    char ** ids = calloc((size_t) count, sizeof(*ids));
    for (int i = 0; ids && i < count; i++) {
        ids[i] = strdup(albums[i].id);
        if (!ids[i]) { free_id_list(ids, count); return NULL; }
    }
    return ids;
}

bool gui_subsonic_append_songs(subsonic_song_t ** songs, int * count, int * capacity,
                                   const subsonic_song_t * album_songs, int album_song_count) {
    if (album_song_count < 0 || *count > INT_MAX - album_song_count ||
        (album_song_count > 0 && !album_songs)) return false;
    if (*count + album_song_count > *capacity) {
        int needed = *count + album_song_count;
        int new_capacity = needed > INT_MAX / 2 ? needed : needed * 2;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(**songs)) return false;
        subsonic_song_t * grown = realloc(*songs, sizeof(**songs) * (size_t)new_capacity);
        if (!grown) return false;
        *songs = grown;
        *capacity = new_capacity;
    }
    if (album_song_count > 0) {
        memcpy(*songs + *count, album_songs, sizeof(subsonic_song_t) * (size_t)album_song_count);
        *count += album_song_count;
    }
    return true;
}

static bool subsonic_expand_albums(subsonic_library_download_request_t * req,
                                    subsonic_song_t ** songs, int * song_count) {
    int capacity = 0, count = 0;
    bool expansion_failed = false;
    for (int i = 0; i < req->album_to_expand_count; i++) {
        if (http_cancel_token_is_cancelled(&subsonic_library_download_cancel)) break;
        subsonic_song_t * album_songs = NULL;
        int album_song_count = 0;
        if (subsonic_get_album_songs(&req->server, req->album_ids[i], &album_songs,
                                      &album_song_count, &subsonic_library_download_cancel))
            expansion_failed = !gui_subsonic_append_songs(songs, &count, &capacity, album_songs, album_song_count);
        free(album_songs);
        if (expansion_failed) break;
    }
    *song_count = expansion_failed ? 0 : count;
    subsonic_library_download_total = *song_count; /* Keep the original plain atomic store. */
    free_id_list(req->album_ids, req->album_to_expand_count);
    req->album_ids = NULL;
    return !expansion_failed;
}

static bool subsonic_download_one_song(const subsonic_library_download_request_t * req,
                                        subsonic_song_t * songs, int song_count, int i,
                                        album_dir_tracker_t * album_tracker, char dest_path[1024]) {
    subsonic_song_t * song = &songs[i];

    char safe_artist[160], safe_album[160], safe_title[160];
    subsonic_song_folder_names(song, safe_artist, safe_album);
    sanitize_path_component(song->title[0] ? song->title : "Unknown Title", safe_title, sizeof(safe_title));

    char album_dir[1024];
    if (!subsonic_ensure_library_album_dir(req->download_subfolder, req->download_layout,
                                           safe_artist, safe_album, album_dir, sizeof(album_dir))) {
        return false;
    }
    char filename[350];
    subsonic_build_download_filename(song, safe_title, filename, sizeof(filename));
    int dest_len = snprintf(dest_path, 1024, "%s/%s", album_dir, filename);
    if (dest_len < 0 || (size_t)dest_len >= 1024) {
        return false;
    }

    char url[1536];
    subsonic_build_stream_url(&req->server, song->id, url, sizeof(url));
    bool ok = subsonic_download_file(url, req->server.verify_tls, dest_path, &subsonic_library_download_cancel);

    if (ok) {
        if (!album_dir_tracker_contains(album_tracker, album_dir)) {
            album_dir_tracker_add(album_tracker, album_dir);
            subsonic_fetch_album_artwork_if_needed(&req->server, album_dir, songs, song_count, i,
                                                   safe_artist, safe_album, &subsonic_library_download_cancel);
        }
    }
    return ok;
}

static void subsonic_playlist_add(const subsonic_library_download_request_t * req,
                                   const char * dest_path, bool * playlist_first,
                                   char playlist_m3u_path[512]) {
    if (req->playlist_name[0] != '\0') {
        if (*playlist_first) {
            if (playlist_files_create(PLAYLISTS_DIR, req->playlist_name, dest_path, playlist_m3u_path,
                                      512)) {
                metadata_db_playlist_insert_one(playlist_m3u_path);
            }
            *playlist_first = false;
        } else if (playlist_m3u_path[0] != '\0') {
            playlist_files_append(playlist_m3u_path, dest_path);
        }
    }
}

static void subsonic_library_download_done(subsonic_library_download_request_t * req,
                                            subsonic_song_t * songs, int success_count,
                                            album_dir_tracker_t * tracker) {
    subsonic_library_download_success_count = success_count;
    album_dir_tracker_free(tracker);
    free(songs);
    free(req);
    atomic_store_explicit(&subsonic_library_download_done_flag, true, memory_order_release); /* written last */
    worker_notify_ui();
}

static void * subsonic_library_download_thread_func(void * arg) {
    subsonic_library_download_request_t * req = arg;
    subsonic_song_t * songs = req->songs;
    int song_count = req->song_count;
    if (req->album_ids && !subsonic_expand_albums(req, &songs, &song_count)) {
        subsonic_library_download_done(req, songs, 0, NULL);
        return NULL;
    }
    char playlist_m3u_path[512] = "";
    bool playlist_first = true;
    int success_count = 0;
    album_dir_tracker_t album_tracker = {0};
    for (int i = 0; i < song_count; i++) {
        if (http_cancel_token_is_cancelled(&subsonic_library_download_cancel)) break;
        char dest_path[1024];
        if (subsonic_download_one_song(req, songs, song_count, i, &album_tracker, dest_path)) {
            success_count++;
            subsonic_playlist_add(req, dest_path, &playlist_first, playlist_m3u_path);
        }
        atomic_store_explicit(&subsonic_library_download_progress, i + 1, memory_order_relaxed);
    }
    subsonic_library_download_done(req, songs, success_count, &album_tracker);
    return NULL;
}

static void subsonic_library_request_free(subsonic_library_download_request_t * req) {
    free(req->songs);
    free_id_list(req->album_ids, req->album_to_expand_count);
    free(req);
}

static subsonic_library_download_request_t * subsonic_library_request_new(
        subsonic_song_t * songs, int song_count, char ** album_ids, int album_to_expand_count,
        const char * playlist_name) {
    subsonic_library_download_request_t * req = malloc(sizeof(*req));
    if (!req) {
        free(songs);
        free_id_list(album_ids, album_to_expand_count);
        show_error_toast(TR("Not enough memory to start download"));
        return NULL;
    }
    req->server = subsonic_server_from_settings();
    req->songs = songs;
    req->song_count = song_count;
    req->album_ids = album_ids;
    req->album_to_expand_count = album_to_expand_count;
    if (!settings_validate_subsonic_download_subfolder(current_settings.subsonic_download_subfolder,
            req->download_subfolder, sizeof(req->download_subfolder))) {
        subsonic_library_request_free(req);
        show_error_toast(TR("Invalid download folder"));
        return NULL;
    }
    req->download_layout = current_settings.subsonic_download_layout == 1 ? 1 : 0;
    snprintf(req->playlist_name, sizeof(req->playlist_name), "%s", playlist_name ? playlist_name : "");

    return req;
}

void gui_subsonic_download_library(subsonic_song_t * songs, int song_count,
                                              char ** album_ids, int album_to_expand_count,
                                              const char * playlist_name, const char * progress_label) {
    subsonic_library_download_request_t * req = subsonic_library_request_new(
        songs, song_count, album_ids, album_to_expand_count, playlist_name);
    if (!req) return;

    atomic_store_explicit(&subsonic_library_download_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&subsonic_library_download_total, album_ids ? 0 : song_count, memory_order_relaxed); /* 0 = "still figuring out the total," see poll_subsonic_library_download() */
    subsonic_library_download_success_count = 0;
    atomic_store_explicit(&subsonic_library_download_done_flag, false, memory_order_relaxed);
    http_cancel_token_init(&subsonic_library_download_cancel);
    subsonic_library_download_active = true;

    subsonic_library_download_token = gui_busy_show(progress_label, "");
    gui_busy_set_progress(subsonic_library_download_token, 0);

    if (pthread_create(&subsonic_library_download_thread, NULL, subsonic_library_download_thread_func, req) != 0) {
        subsonic_library_download_active = false;
        http_cancel_token_destroy(&subsonic_library_download_cancel);
        subsonic_library_request_free(req);
        gui_busy_hide(subsonic_library_download_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_subsonic_library_download(void) {
    if (!subsonic_library_download_active) return;

    if (!atomic_load_explicit(&subsonic_library_download_done_flag, memory_order_acquire)) {
        int total = atomic_load_explicit(&subsonic_library_download_total, memory_order_relaxed);
        if (total > 0) {
            gui_busy_set_progress(subsonic_library_download_token, (atomic_load_explicit(&subsonic_library_download_progress, memory_order_relaxed) * 100) / total);
        }
        return;
    }

    subsonic_library_download_active = false;
    pthread_join(subsonic_library_download_thread, NULL);
    http_cancel_token_destroy(&subsonic_library_download_cancel);

    /* Close the download's shared busy screen before
     * start_library_auto_rescan() opens it for the rescan. */
    nav_pop();

    if (subsonic_library_download_success_count > 0) {
        /* Automatically rescan downloaded files, preserving recovered
         * snapshots and libraries that failed to load. */
        start_library_auto_rescan();
    } else {
        show_error_toast(TR("Download failed"));
    }
}


bool gui_subsonic_downloads_active(void) {
    return download_active || subsonic_library_download_active;
}

void gui_subsonic_downloads_cancel(void) {
    if (download_active) http_cancel_token_cancel(&download_cancel);
    if (subsonic_library_download_active) http_cancel_token_cancel(&subsonic_library_download_cancel);
}

void gui_subsonic_downloads_join(void) {
    if (download_active) {
        pthread_join(download_thread, NULL);
        download_active = false;
        http_cancel_token_destroy(&download_cancel);
        gui_busy_hide(download_token);
    }
    if (subsonic_library_download_active) {
        pthread_join(subsonic_library_download_thread, NULL);
        subsonic_library_download_active = false;
        http_cancel_token_destroy(&subsonic_library_download_cancel);
        gui_busy_hide(subsonic_library_download_token);
    }
}
