#include "gui.h"
#include "i18n.h"
#include "gui_subsonic.h"
#include "settings.h"
#include "utf8_util.h"
#include "screen_builders.h"
#include "gui_text_input.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include "assets.h"
#include "metadata.h"
#include "subsonic_client.h"
#include "http_client.h"
#include "device_config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "audio.h"
#include "wifi_status.h"
#include "albumart.h"

void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, const char * (*name_of)(int), const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind, compact_list_fetch_page_cb_t restore_fetch_page);


extern player_settings_t current_settings;
#define SUBSONIC_STREAM_CACHE_DIR MUSIC_ROOT_DIR "/.subsonic_cache"
#define TITLE_LABEL_LEFT_INSET 76
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20
#define SUBSONIC_REQUEST_TIMEOUT_MS 30000U

extern bool playlist_files_append(const char * path, const char * dest_path);
extern int search_remap_index(search_binding_id_t id, int list_index);
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
extern void start_library_auto_rescan(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern lv_color_t accent_lv_color(void);


#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"


typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;

#define SUBSONIC_FILE_CONNECT_TIMEOUT_MS 10000U
#define SUBSONIC_FILE_READ_TIMEOUT_MS 30000U

typedef struct {
    subsonic_server_t server;
    subsonic_song_t * songs;
    int song_count;
    subsonic_album_t * albums_to_expand;
    int album_to_expand_count;
    char playlist_name[128]; 
    char download_subfolder[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    int download_layout;
} subsonic_library_download_request_t;

extern bool playlist_files_create(const char * dir, const char * name, const char * initial_file, const char * out_m3u_path, size_t out_m3u_path_size);

typedef struct {
    subsonic_server_t server;
} subsonic_connect_request_t;






typedef enum {
    SUBSONIC_BROWSE_ARTISTS,
    SUBSONIC_BROWSE_ALBUM_SONGS,
    SUBSONIC_BROWSE_ARTIST_ALBUMS,
    SUBSONIC_BROWSE_PLAYLIST_SONGS,
    SUBSONIC_BROWSE_PLAYLISTS,
    SUBSONIC_BROWSE_ALL_ALBUMS,
} subsonic_browse_kind_t;

typedef struct {
    subsonic_browse_kind_t kind;
    subsonic_server_t server;
    char id[64];
    char title[128];
} subsonic_browse_request_t;


typedef enum { SUBSONIC_DOWNLOAD_PENDING_NONE, SUBSONIC_DOWNLOAD_PENDING_SONGS, SUBSONIC_DOWNLOAD_PENDING_ARTIST } subsonic_download_pending_t;


#include <sys/stat.h>

extern void clear_player_source(void);
extern void on_file_selected(char ** files, int count, int index);
extern void nav_remove_stack_slot(int depth);

static char download_dest_path[512];
static atomic_bool download_done_flag = false;
static bool download_success_flag = false;
static bool download_active = false;
static gui_busy_handle_t download_token = 0;

static gui_busy_handle_t subsonic_library_download_token = 0;

static gui_busy_handle_t subsonic_browse_token = 0;

static gui_busy_handle_t subsonic_connect_token = 0;

static lv_obj_t * subsonic_entry_screen;

subsonic_stream_song_meta_t * subsonic_stream_meta = NULL; /* parallel array, NULL when no Subsonic stream queue is loaded */

int subsonic_stream_meta_count = 0;

void poll_subsonic_download(void);

void poll_subsonic_library_download(void);

void poll_subsonic_connect(void);

void poll_subsonic_browse(void);

bool subsonic_library_download_active;

bool subsonic_connect_active;


static subsonic_server_t subsonic_server_from_settings(void) {
    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", current_settings.subsonic_url);
    snprintf(server.username, sizeof(server.username), "%s", current_settings.subsonic_username);
    snprintf(server.password, sizeof(server.password), "%s", current_settings.subsonic_password);
    server.verify_tls = current_settings.subsonic_verify_tls;
    return server;
}

static pthread_t download_thread;
static http_cancel_token_t download_cancel;

static void * download_thread_func(void * arg) {
    download_request_t * req = (download_request_t *) arg;
    bool ok = http_get_to_file_cancelable(req->url, req->verify_tls, req->dest_path, NULL, NULL,
                                           SUBSONIC_FILE_CONNECT_TIMEOUT_MS, SUBSONIC_FILE_READ_TIMEOUT_MS,
                                           &download_cancel);
    download_success_flag = ok;
    atomic_store_explicit(&download_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    free(req);
    return NULL;
}

static void start_subsonic_download(const char * url, bool verify_tls, const char * dest_path, const char * display_title) {
    snprintf(download_dest_path, sizeof(download_dest_path), "%s", dest_path);

    download_request_t * req = malloc(sizeof(*req));
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
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        gui_busy_hide(download_token);
    }
    /* else (download failed, or play_track_at_from() bailed out early via
     * external_dac_block_reason() without navigating): silently stays
     * wherever nav_pop() landed -- no error-toast UI exists yet to explain
     * a failed download (network drop, server error mid-stream, disk
     * full, ...), same gap already noted for subsonic_connect_row_cb
     * above. */
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

static uint64_t subsonic_album_folder_hash(const char * artist, const char * album) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *)(artist ? artist : ""); *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= 0xFFu;
    hash *= UINT64_C(1099511628211);
    for (const unsigned char * p = (const unsigned char *)(album ? album : ""); *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
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

static bool subsonic_ensure_library_album_dir(const char * subfolder, int layout,
                                               const char * safe_artist, const char * safe_album,
                                               char * out, size_t out_size) {
    char checked_subfolder[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    if (!settings_validate_subsonic_download_subfolder(subfolder, checked_subfolder,
                                                       sizeof(checked_subfolder))) return false;
    int dirfd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return false;
    int n = snprintf(out, out_size, "%s", MUSIC_ROOT_DIR);
    if (n < 0 || (size_t)n >= out_size) { close(dirfd); return false; }

    for (char * component = checked_subfolder; component[0]; ) {
        char * slash = strchr(component, '/');
        if (slash) *slash = '\0';
        if (!subsonic_enter_directory(&dirfd, out, out_size, component)) { close(dirfd); return false; }
        if (!slash) break;
        component = slash + 1;
    }

    if (layout == 1) {
        char full_name[1024], combined[256];
        n = snprintf(full_name, sizeof(full_name), "%s - %s", safe_artist, safe_album);
        if (n < 0 || (size_t)n >= sizeof(full_name)) { close(dirfd); return false; }
        size_t full_length = (size_t)n;
        if (full_length <= 255) {
            memcpy(combined, full_name, full_length + 1);
        } else {
            char hash_suffix[18], prefix[256];
            snprintf(hash_suffix, sizeof(hash_suffix), "~%016llx",
                     (unsigned long long)subsonic_album_folder_hash(safe_artist, safe_album));
            size_t prefix_budget = 255 - strlen(hash_suffix);
            utf8_truncate_safe(prefix, full_name, prefix_budget + 1);
            int combined_length = snprintf(combined, sizeof(combined), "%s%s", prefix, hash_suffix);
            if (combined_length < 0 || combined_length > 255) { close(dirfd); return false; }
        }
        if (!subsonic_enter_directory(&dirfd, out, out_size, combined)) { close(dirfd); return false; }
    } else {
        if (!subsonic_enter_directory(&dirfd, out, out_size, safe_artist) ||
            !subsonic_enter_directory(&dirfd, out, out_size, safe_album)) { close(dirfd); return false; }
    }
    close(dirfd);
    return true;
}

static uint64_t subsonic_song_id_hash(const char * id) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) id; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
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
    if (song->disc > 1 && song->track > 0)
        snprintf(number_prefix, sizeof(number_prefix), "%d-%02d - ", song->disc, song->track);
    else if (song->disc > 1)
        snprintf(number_prefix, sizeof(number_prefix), "%d - ", song->disc);
    else if (song->track > 0)
        snprintf(number_prefix, sizeof(number_prefix), "%02d - ", song->track);
    else
        number_prefix[0] = '\0';

    char full_name[512];
    if (distinct_artist)
        snprintf(full_name, sizeof(full_name), "%s%s - %s.%s", number_prefix, safe_title, safe_track_artist, safe_suffix);
    else
        snprintf(full_name, sizeof(full_name), "%s%s.%s", number_prefix, safe_title, safe_suffix);

    if (strlen(full_name) > 255) {
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
            snprintf(full_name, sizeof(full_name), "%s%s - %s%s.%s",
                     number_prefix, short_title, short_artist, hash_tag, safe_suffix);
        else
            snprintf(full_name, sizeof(full_name), "%s%s%s.%s",
                     number_prefix, short_title, hash_tag, safe_suffix);
    }
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

    const char * cover_art_id = songs[current_song_index].cover_art;
    if (cover_art_id[0] == '\0') {
        for (int k = 0; k < song_count; k++) {
            if (songs[k].cover_art[0] == '\0') continue;
            char s_artist[160], s_album[160];
            sanitize_path_component(songs[k].album_artist[0] ? songs[k].album_artist :
                                    (songs[k].artist[0] ? songs[k].artist : "Unknown Artist"), s_artist, sizeof(s_artist));
            sanitize_path_component(songs[k].album[0] ? songs[k].album : "Unknown Album", s_album, sizeof(s_album));
            if (strcmp(s_artist, safe_artist) == 0 && strcmp(s_album, safe_album) == 0) {
                cover_art_id = songs[k].cover_art;
                break;
            }
        }
    }
    if (cover_art_id[0] == '\0') return;

    char cover_url[1536];
    subsonic_build_cover_art_url(server, cover_art_id, cover_url, sizeof(cover_url));

    char temp_path[1024], dest_cover[1024];
    int n = snprintf(temp_path, sizeof(temp_path), "%s/cover.tmp", album_dir);
    if (n < 0 || (size_t) n >= sizeof(temp_path)) return;
    remove(temp_path);

    /* Capped: a cover is a few MB at most, and a misbehaving server must
     * not be able to fill the card. */
    int status = 0;
    bool ok = http_get_to_file_redirects(cover_url, server->verify_tls, temp_path, 20u << 20, NULL, NULL,
                                         SUBSONIC_FILE_CONNECT_TIMEOUT_MS, SUBSONIC_FILE_READ_TIMEOUT_MS,
                                         cancel, 3, &status) &&
              status == 200;
    if (!ok) {
        remove(temp_path);
        return;
    }

    subsonic_cover_format_t fmt = subsonic_detect_image_format(temp_path);
    if (fmt == SUBSONIC_COVER_FORMAT_UNKNOWN) {
        remove(temp_path);
        return;
    }

    n = snprintf(dest_cover, sizeof(dest_cover), "%s/cover.%s", album_dir,
                 fmt == SUBSONIC_COVER_FORMAT_JPEG ? "jpg" : "png");
    if (n < 0 || (size_t) n >= sizeof(dest_cover) || access(dest_cover, F_OK) == 0) {
        remove(temp_path);
        return;
    }

    if (rename(temp_path, dest_cover) != 0) {
        remove(temp_path);
    }
}

static pthread_t subsonic_library_download_thread;
static http_cancel_token_t subsonic_library_download_cancel;

bool subsonic_library_download_active = false;

static atomic_bool subsonic_library_download_done_flag = false;

static atomic_int subsonic_library_download_progress = 0; /* songs completed so far */

static atomic_int subsonic_library_download_total = 0;    /* 0 while still expanding an artist's albums (Mode B) -- see poll_subsonic_library_download() */

static int subsonic_library_download_success_count = 0;

static void * subsonic_library_download_thread_func(void * arg) {
    subsonic_library_download_request_t * req = (subsonic_library_download_request_t *) arg;

    subsonic_song_t * songs = req->songs; /* Mode A: the caller's owned copy; Mode B: NULL, built below */
    int song_count = req->song_count;

    bool expansion_failed = false;
    if (req->albums_to_expand) {
        int capacity = 0;
        int count = 0;
        for (int i = 0; i < req->album_to_expand_count; i++) {
            if (http_cancel_token_is_cancelled(&subsonic_library_download_cancel)) break;
            subsonic_song_t * album_songs = NULL;
            int album_song_count = 0;
            if (subsonic_get_album_songs(&req->server, req->albums_to_expand[i].id, &album_songs,
                                          &album_song_count, &subsonic_library_download_cancel)) {
                if (album_song_count < 0 || count > INT_MAX - album_song_count ||
                    (album_song_count > 0 && !album_songs)) {
                    expansion_failed = true;
                } else if (count + album_song_count > capacity) {
                    int needed = count + album_song_count;
                    int new_capacity = needed > INT_MAX / 2 ? needed : needed * 2;
                    if ((size_t)new_capacity > SIZE_MAX / sizeof(*songs)) expansion_failed = true;
                    else {
                        subsonic_song_t * grown = realloc(songs, sizeof(*songs) * (size_t)new_capacity);
                        if (!grown) expansion_failed = true;
                        else { songs = grown; capacity = new_capacity; }
                    }
                }
                if (!expansion_failed && album_song_count > 0) {
                    memcpy(songs + count, album_songs, sizeof(subsonic_song_t) * (size_t) album_song_count);
                    count += album_song_count;
                }
            }
            free(album_songs);
            if (expansion_failed) break;
        }
        song_count = expansion_failed ? 0 : count;
        subsonic_library_download_total = song_count; /* was 0 until this expansion finished -- unblocks poll_subsonic_library_download()'s progress display */
        free(req->albums_to_expand);
        req->albums_to_expand = NULL;
        if (expansion_failed) {
            free(songs);
            free(req);
            subsonic_library_download_success_count = 0;
            atomic_store_explicit(&subsonic_library_download_done_flag, true, memory_order_release);
            return NULL;
        }
    }

    char playlist_m3u_path[512] = "";
    bool playlist_first = true;
    int success_count = 0;
    album_dir_tracker_t album_tracker = {0};

    for (int i = 0; i < song_count; i++) {
        if (http_cancel_token_is_cancelled(&subsonic_library_download_cancel)) break;
        subsonic_song_t * song = &songs[i];

        char safe_artist[160], safe_album[160], safe_title[160];
        sanitize_path_component(song->album_artist[0] ? song->album_artist :
                                (song->artist[0] ? song->artist : "Unknown Artist"), safe_artist, sizeof(safe_artist));
        sanitize_path_component(song->album[0] ? song->album : "Unknown Album", safe_album, sizeof(safe_album));
        sanitize_path_component(song->title[0] ? song->title : "Unknown Title", safe_title, sizeof(safe_title));

        char album_dir[1024], dest_path[1024];
        if (!subsonic_ensure_library_album_dir(req->download_subfolder, req->download_layout,
                                               safe_artist, safe_album, album_dir, sizeof(album_dir))) {
            atomic_store_explicit(&subsonic_library_download_progress, i + 1, memory_order_relaxed);
            continue;
        }
        char filename[350];
        subsonic_build_download_filename(song, safe_title, filename, sizeof(filename));
        int dest_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", album_dir, filename);
        if (dest_len < 0 || (size_t)dest_len >= sizeof(dest_path)) {
            atomic_store_explicit(&subsonic_library_download_progress, i + 1, memory_order_relaxed);
            continue;
        }

        char url[1536];
        subsonic_build_stream_url(&req->server, song->id, url, sizeof(url));
        bool ok = http_get_to_file_cancelable(url, req->server.verify_tls, dest_path, NULL, NULL,
                                               SUBSONIC_FILE_CONNECT_TIMEOUT_MS, SUBSONIC_FILE_READ_TIMEOUT_MS,
                                               &subsonic_library_download_cancel);

        if (ok) {
            success_count++;
            if (!album_dir_tracker_contains(&album_tracker, album_dir)) {
                album_dir_tracker_add(&album_tracker, album_dir);
                subsonic_fetch_album_artwork_if_needed(&req->server, album_dir, songs, song_count, i,
                                                       safe_artist, safe_album, &subsonic_library_download_cancel);
            }
            if (req->playlist_name[0] != '\0') {
                if (playlist_first) {
                    if (playlist_files_create(PLAYLISTS_DIR, req->playlist_name, dest_path, playlist_m3u_path,
                                              sizeof(playlist_m3u_path))) {
                        metadata_db_playlist_insert_one(playlist_m3u_path);
                    }
                    playlist_first = false;
                } else if (playlist_m3u_path[0] != '\0') {
                    playlist_files_append(playlist_m3u_path, dest_path);
                }
            }
        }

        atomic_store_explicit(&subsonic_library_download_progress, i + 1, memory_order_relaxed);
    }

    subsonic_library_download_success_count = success_count;
    album_dir_tracker_free(&album_tracker);
    free(songs);
    free(req);
    atomic_store_explicit(&subsonic_library_download_done_flag, true, memory_order_release); /* written last -- poll_subsonic_library_download() only checks this flag */
    return NULL;
}

static void start_subsonic_library_download(subsonic_song_t * songs, int song_count,
                                              subsonic_album_t * albums_to_expand, int album_to_expand_count,
                                              const char * playlist_name, const char * progress_label) {
    subsonic_library_download_request_t * req = malloc(sizeof(*req));
    if (!req) {
        free(songs);
        free(albums_to_expand);
        show_error_toast(TR("Not enough memory to start download"));
        return;
    }
    req->server = subsonic_server_from_settings();
    req->songs = songs;
    req->song_count = song_count;
    req->albums_to_expand = albums_to_expand;
    req->album_to_expand_count = album_to_expand_count;
    if (!settings_validate_subsonic_download_subfolder(current_settings.subsonic_download_subfolder,
            req->download_subfolder, sizeof(req->download_subfolder))) {
        free(songs);
        free(albums_to_expand);
        free(req);
        show_error_toast(TR("Invalid download folder"));
        return;
    }
    req->download_layout = current_settings.subsonic_download_layout == 1 ? 1 : 0;
    snprintf(req->playlist_name, sizeof(req->playlist_name), "%s", playlist_name ? playlist_name : "");

    atomic_store_explicit(&subsonic_library_download_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&subsonic_library_download_total, albums_to_expand ? 0 : song_count, memory_order_relaxed); /* 0 = "still figuring out the total," see poll_subsonic_library_download() */
    subsonic_library_download_success_count = 0;
    atomic_store_explicit(&subsonic_library_download_done_flag, false, memory_order_relaxed);
    http_cancel_token_init(&subsonic_library_download_cancel);
    subsonic_library_download_active = true;

    subsonic_library_download_token = gui_busy_show(progress_label, "");
    gui_busy_set_progress(subsonic_library_download_token, 0);

    if (pthread_create(&subsonic_library_download_thread, NULL, subsonic_library_download_thread_func, req) != 0) {
        subsonic_library_download_active = false;
        http_cancel_token_destroy(&subsonic_library_download_cancel);
        free(req->songs);
        free(req->albums_to_expand);
        free(req);
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

static void populate_indexed_list(lv_obj_t * list, int count, const char * (*label_of)(int), lv_event_cb_t click_cb) {
    lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        /* One lv_label via the shared list_row_style, not a container +
         * child label each with their own local style properties -- see
         * list_row_style's own doc comment (screen_builders.h). */
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(row, label_of(i));

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static subsonic_artist_t * subsonic_artists_cache = NULL;

static int subsonic_artists_count = 0;

/* A successful empty response is a loaded cache too. */
static bool subsonic_artists_loaded = false;

static subsonic_album_t * subsonic_albums_cache = NULL;

static int subsonic_albums_count = 0;

static subsonic_song_t * subsonic_songs_cache = NULL;

static int subsonic_songs_count = 0;

static subsonic_playlist_t * subsonic_playlists_cache = NULL;

static int subsonic_playlists_count = 0;

static lv_obj_t * subsonic_menu_screen;

static lv_obj_t * subsonic_menu_title_label;

static lv_obj_t * subsonic_menu_list;
static lv_obj_t * subsonic_quality_screen;
static lv_obj_t * subsonic_quality_list;
static lv_obj_t * subsonic_quality_options[4];
static lv_obj_t * subsonic_quality_row;
static lv_obj_t * subsonic_download_settings_screen;
static lv_obj_t * subsonic_download_settings_list;
static lv_obj_t * subsonic_download_folder_row;
static lv_obj_t * subsonic_download_layout_options[2];
static lv_obj_t * subsonic_download_settings_row;

static const char * subsonic_quality_name(int quality) {
    static const char * names[] = { N_("Original"), N_("Low"), N_("Medium"), N_("High") };
    return TR(quality >= 0 && quality < 4 ? names[quality] : names[0]);
}

static void subsonic_quality_update_menu_label(void) {
    char label[64];
    snprintf(label, sizeof(label), TR("Stream quality: %s"),
             subsonic_quality_name(current_settings.subsonic_stream_quality));
    if (subsonic_quality_row) lv_label_set_text(lv_obj_get_child(subsonic_quality_row, 0), label);
}

static void subsonic_quality_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.subsonic_stream_quality = (int) (intptr_t) lv_event_get_user_data(e);
    settings_save_async(&current_settings);
    for (int i = 0; i < 4; i++) {
        if (subsonic_quality_options[i])
            lv_obj_set_style_border_width(subsonic_quality_options[i], i == current_settings.subsonic_stream_quality ? 3 : 0, 0);
    }
    subsonic_quality_update_menu_label();
    nav_pop();
}

static void subsonic_quality_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(subsonic_quality_screen);
}

static lv_obj_t * build_subsonic_quality_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Stream Quality"), &title, &subsonic_quality_list);
    (void) title;
    add_section_header(subsonic_quality_list, TR("Applies to new streaming queues"));
    static const char * labels[] = { N_("Original"), N_("Low (96 kbps)"), N_("Medium (192 kbps)"), N_("High (320 kbps)") };
    for (int i = 0; i < 4; i++) {
        subsonic_quality_options[i] = add_pill_option_row(subsonic_quality_list, TR(labels[i]),
                                                          current_settings.subsonic_stream_quality == i,
                                                          subsonic_quality_option_cb, (void *) (intptr_t) i);
    }
    return screen;
}

static void subsonic_download_folder_update_row(void) {
    if (!subsonic_download_folder_row) return;
    char label[112];
    if (current_settings.subsonic_download_subfolder[0]) {
        char preview[49];
        utf8_truncate_safe(preview, current_settings.subsonic_download_subfolder, sizeof(preview));
        snprintf(label, sizeof(label), TR("Download folder: %s"), preview);
    } else {
        snprintf(label, sizeof(label), "%s", TR("Download folder: SD root"));
    }
    lv_label_set_text(lv_obj_get_child(subsonic_download_folder_row, 0), label);
}

static void subsonic_download_folder_done(const char * text, void * user_data) {
    (void)user_data;
    char checked[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    if (!settings_validate_subsonic_download_subfolder(text ? text : "", checked, sizeof(checked))) {
        show_error_toast(TR("Invalid download folder name"));
        return;
    }
    snprintf(current_settings.subsonic_download_subfolder,
             sizeof(current_settings.subsonic_download_subfolder), "%s", checked);
    settings_save_async(&current_settings);
    subsonic_download_folder_update_row();
}

static void subsonic_download_folder_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Download subfolder"), current_settings.subsonic_download_subfolder,
                    false, false, subsonic_download_folder_done, NULL);
}

static void subsonic_download_layout_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.subsonic_download_layout = (int)(intptr_t)lv_event_get_user_data(e) == 1 ? 1 : 0;
    settings_save_async(&current_settings);
    for (int i = 0; i < 2; i++)
        if (subsonic_download_layout_options[i])
            lv_obj_set_style_border_width(subsonic_download_layout_options[i],
                i == current_settings.subsonic_download_layout ? 3 : 0, 0);
}

static void subsonic_download_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(subsonic_download_settings_screen);
}

static lv_obj_t * build_subsonic_download_settings_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Download Settings"), &title,
                                                    &subsonic_download_settings_list);
    (void)title;
    subsonic_download_folder_row = add_pill_chevron_row(subsonic_download_settings_list,
        TR("Subfolder: SD root"), subsonic_download_folder_row_cb);
    subsonic_download_folder_update_row();
    lv_obj_t * folder_help = add_section_header(subsonic_download_settings_list,
        TR("Subfolder is relative to SD root (example: Music/Offline); empty uses SD root"));
    lv_obj_set_width(folder_help, lv_pct(100));
    lv_obj_set_style_pad_right(folder_help, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(folder_help, LV_LABEL_LONG_WRAP);
    add_section_header(subsonic_download_settings_list, TR("Folder layout for downloaded albums"));
    static const char * labels[] = { N_("Album Artist / Album"), N_("Album Artist - Album") };
    for (int i = 0; i < 2; i++)
        subsonic_download_layout_options[i] = add_pill_option_row(subsonic_download_settings_list,
            TR(labels[i]), current_settings.subsonic_download_layout == i,
            subsonic_download_layout_option_cb, (void *)(intptr_t)i);
    return screen;
}

static lv_obj_t * subsonic_artists_screen;

static lv_obj_t * subsonic_artists_title_label;

static lv_obj_t * subsonic_artists_list;

static lv_obj_t * subsonic_albums_screen;

static lv_obj_t * subsonic_albums_title_label;

static lv_obj_t * subsonic_albums_list;

static lv_obj_t * subsonic_albums_download_btn;

static lv_obj_t * subsonic_songs_screen;

static lv_obj_t * subsonic_songs_title_label;

static lv_obj_t * subsonic_songs_list;

static lv_obj_t * subsonic_songs_download_btn;

static lv_obj_t * subsonic_playlists_screen;

static lv_obj_t * subsonic_playlists_title_label;

static lv_obj_t * subsonic_playlists_list;

static lv_obj_t * subsonic_saved_servers_screen;

static lv_obj_t * subsonic_saved_servers_list;

static lv_obj_t * subsonic_new_connection_screen;

static char subsonic_albums_context_artist[128] = "";

static bool subsonic_songs_context_is_playlist = false;

static char subsonic_songs_context_playlist_name[128] = "";

static const char * subsonic_artist_label_of(int i) { return subsonic_artists_cache[i].name; }

static bool populate_subsonic_artists(void) {
    if (subsonic_artists_count == 0) {
        compact_list_set_items(subsonic_artists_list, NULL, 0);
        return true;
    }
    compact_list_item_t * items =
        malloc(sizeof(*items) * (size_t) subsonic_artists_count);
    if (!items) return false;
    for (int i = 0; i < subsonic_artists_count; i++) items[i] = (compact_list_item_t){ subsonic_artist_label_of(i) };
    compact_list_set_items(subsonic_artists_list, items, subsonic_artists_count);
    free(items);
    return true;
}

static void subsonic_copy_request_error(char * dest, size_t size, const char * fallback) {
    const char * error = subsonic_last_error();
    snprintf(dest, size, "%s", error && error[0] ? error : fallback);
}

static const char * subsonic_album_label_of(int i) { return subsonic_albums_cache[i].name; }

static const char * subsonic_song_label_of(int i) { return subsonic_songs_cache[i].title; }

static const char * subsonic_playlist_label_of(int i) { return subsonic_playlists_cache[i].name; }

static void subsonic_fill_stream_queue_entry(const subsonic_server_t * server, const subsonic_song_t * song,
                                              int quality_kbps, char ** new_playlist,
                                              subsonic_stream_song_meta_t * new_meta, int slot) {
    char url[1536];
    subsonic_build_stream_url_quality(server, song->id, quality_kbps, url, sizeof(url));
    /* The "#.<suffix>" appended here is a local-only hint consumed by
     * audio.c's decoder_open() (stream_format_hint()); http_conn_parse_url()
     * strips it before it ever reaches the actual HTTP request, so it has
     * no effect on the server-facing URL. */
    size_t len = strlen(url);
    const char * stream_suffix = quality_kbps > 0 ? "mp3" : song->suffix;
    snprintf(url + len, sizeof(url) - len, "#.%s", stream_suffix);

    new_playlist[slot] = strdup(url);

    subsonic_stream_song_meta_t * m = &new_meta[slot];
    snprintf(m->url, sizeof(m->url), "%s", url);
    snprintf(m->title, sizeof(m->title), "%s", song->title);
    snprintf(m->artist, sizeof(m->artist), "%s", song->artist);
    snprintf(m->album, sizeof(m->album), "%s", song->album);
    snprintf(m->suffix, sizeof(m->suffix), "%s", stream_suffix);
    m->track = song->track;
    m->disc = song->disc;
    m->duration_seconds = song->duration_seconds;
    /* Source format metadata describes the library file, not a transcoded MP3. */
    m->sample_rate = quality_kbps > 0 ? 0 : song->sample_rate;
    m->bit_depth = quality_kbps > 0 ? 0 : song->bit_depth;
    m->channels = quality_kbps > 0 ? 0 : song->channels;
    m->bitrate_kbps = quality_kbps > 0 ? 0 : song->bitrate_kbps;
    if (song->cover_art[0]) {
        subsonic_build_cover_art_url(server, song->cover_art, m->cover_url, sizeof(m->cover_url));
    } else {
        m->cover_url[0] = '\0';
    }
    m->verify_tls = server->verify_tls;
}

static void subsonic_play_song_and_queue_rest(int index) {
    subsonic_song_t * song = &subsonic_songs_cache[index];

    subsonic_server_t server = subsonic_server_from_settings();

    /* mp3/flac plays directly off the stream URL -- no download, no wait --
     * and queues the rest of this album/playlist (subsonic_songs_cache is
     * already the full song list either way, see subsonic_songs_context_is_
     * playlist's own comment above) that's ALSO mp3/flac, in original order,
     * starting at the tapped song, so Prev/Next/auto-advance/Repeat/Shuffle
     * all work across the whole thing exactly like a local-library playlist.
     * A song in some other format is simply left out of this queue -- there's
     * no good way to background-download it without either stalling the
     * queue right there or building a much bigger hybrid stream+download
     * pipeline, and skipping it keeps every other song in the album/playlist
     * reachable instead of the queue silently dead-ending on it. See this
     * section's own top comment for why a non-streamable format tapped
     * directly still downloads first, as a single track, same as before. */
    int quality_kbps = current_settings.subsonic_stream_quality == 1 ? 96 :
                       current_settings.subsonic_stream_quality == 2 ? 192 :
                       current_settings.subsonic_stream_quality == 3 ? 320 : 0;
    if (quality_kbps > 0 || strcasecmp(song->suffix, "mp3") == 0 || strcasecmp(song->suffix, "flac") == 0) {
        char ** new_playlist = malloc(sizeof(char *) * (size_t) subsonic_songs_count);
        subsonic_stream_song_meta_t * new_meta = malloc(sizeof(subsonic_stream_song_meta_t) * (size_t) subsonic_songs_count);
        int count = 0;
        int start_index = -1;

        for (int i = 0; i < subsonic_songs_count; i++) {
            subsonic_song_t * s = &subsonic_songs_cache[i];
            if (quality_kbps <= 0 && strcasecmp(s->suffix, "mp3") != 0 && strcasecmp(s->suffix, "flac") != 0) continue;
            subsonic_fill_stream_queue_entry(&server, s, quality_kbps, new_playlist, new_meta, count);
            if (i == index) start_index = count;
            count++;
        }

        if (start_index < 0) {
            /* Shouldn't happen -- the tapped song itself was mp3/flac, so it
             * must have been included above -- but fail safely rather than
             * play the wrong track if this invariant is ever violated. */
            for (int i = 0; i < count; i++) free(new_playlist[i]);
            free(new_playlist);
            free(new_meta);
            return;
        }

        free(subsonic_stream_meta);
        subsonic_stream_meta = new_meta;
        subsonic_stream_meta_count = count;

        clear_player_source(); /* a streamed queue has no on-device list to go back to */
        on_file_selected(new_playlist, count, start_index);
        return;
    }

    char url[1536];
    subsonic_build_stream_url(&server, song->id, url, sizeof(url));
    mkdir(SUBSONIC_STREAM_CACHE_DIR, 0755); /* no-op (EEXIST) if it's already there */
    char dest[256];
    snprintf(dest, sizeof(dest), SUBSONIC_STREAM_CACHE_DIR "/stream.%s", song->suffix);

    start_subsonic_download(url, server.verify_tls, dest, song->title);
}

static void subsonic_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_play_song_and_queue_rest(index);
}

static void subsonic_album_row_click_cb(int index);

static void subsonic_playlist_row_click_cb(lv_event_t * e);

static pthread_t subsonic_browse_thread;
static http_cancel_token_t subsonic_browse_cancel;

static bool subsonic_browse_active = false;
static bool subsonic_browse_timed_out = false;
static uint32_t subsonic_browse_started_at = 0;

static atomic_bool subsonic_browse_done_flag = false;

static volatile bool subsonic_browse_success_flag = false;

static subsonic_browse_kind_t subsonic_browse_result_kind;

static char subsonic_browse_result_title[128];

static char subsonic_browse_result_error[256];

static subsonic_artist_t * subsonic_browse_result_artists = NULL;

static subsonic_song_t * subsonic_browse_result_songs = NULL;

static subsonic_album_t * subsonic_browse_result_albums = NULL;

static subsonic_playlist_t * subsonic_browse_result_playlists = NULL;

static int subsonic_browse_result_count = 0;

static void * subsonic_browse_thread_func(void * arg) {
    subsonic_browse_request_t * req = (subsonic_browse_request_t *) arg;
    bool ok = false;
    int count = 0;

    switch (req->kind) {
        case SUBSONIC_BROWSE_ARTISTS:
            ok = subsonic_get_artists(&req->server, &subsonic_browse_result_artists, &count,
                                      &subsonic_browse_cancel);
            break;
        case SUBSONIC_BROWSE_ALBUM_SONGS:
            ok = subsonic_get_album_songs(&req->server, req->id, &subsonic_browse_result_songs, &count,
                                           &subsonic_browse_cancel);
            break;
        case SUBSONIC_BROWSE_ARTIST_ALBUMS:
            ok = subsonic_get_artist_albums(&req->server, req->id, &subsonic_browse_result_albums, &count,
                                             &subsonic_browse_cancel);
            break;
        case SUBSONIC_BROWSE_PLAYLIST_SONGS:
            ok = subsonic_get_playlist_songs(&req->server, req->id, &subsonic_browse_result_songs, &count,
                                              &subsonic_browse_cancel);
            break;
        case SUBSONIC_BROWSE_PLAYLISTS:
            ok = subsonic_get_playlists(&req->server, &subsonic_browse_result_playlists, &count,
                                         &subsonic_browse_cancel);
            break;
        case SUBSONIC_BROWSE_ALL_ALBUMS:
            ok = subsonic_get_all_albums(&req->server, &subsonic_browse_result_albums, &count,
                                          &subsonic_browse_cancel);
            break;
    }

    if (!ok) {
        subsonic_copy_request_error(subsonic_browse_result_error, sizeof(subsonic_browse_result_error),
                                    TR("Failed to load from server"));
    }
    subsonic_browse_result_kind = req->kind;
    snprintf(subsonic_browse_result_title, sizeof(subsonic_browse_result_title), "%s", req->title);
    subsonic_browse_result_count = count;
    subsonic_browse_success_flag = ok;
    atomic_store_explicit(&subsonic_browse_done_flag, true, memory_order_release); free(req);
    return NULL;
}

static void start_subsonic_browse(subsonic_browse_kind_t kind, const char * id, const char * title) {
    if (subsonic_browse_active || subsonic_connect_active) {
        show_info_toast(TR("Previous request still finishing"));
        return;
    }

    subsonic_browse_request_t * req = calloc(1, sizeof(*req));
    if (!req) {
        show_error_toast(TR("Not enough memory to load from server"));
        return;
    }
    req->kind = kind;
    req->server = subsonic_server_from_settings();
    if (id) snprintf(req->id, sizeof(req->id), "%s", id);
    if (title) snprintf(req->title, sizeof(req->title), "%s", title);

    subsonic_browse_result_artists = NULL;
    subsonic_browse_result_error[0] = '\0';
    subsonic_browse_result_songs = NULL;
    subsonic_browse_result_albums = NULL;
    subsonic_browse_result_playlists = NULL;
    subsonic_browse_result_count = 0;
    atomic_store_explicit(&subsonic_browse_done_flag, false, memory_order_relaxed);
    subsonic_browse_success_flag = false;
    subsonic_browse_timed_out = false;
    subsonic_browse_started_at = lv_tick_get();
    http_cancel_token_init(&subsonic_browse_cancel);
    subsonic_browse_active = true;

    subsonic_browse_token = gui_busy_show(TR("Loading from server..."), "");
    if (pthread_create(&subsonic_browse_thread, NULL, subsonic_browse_thread_func, req) != 0) {
        subsonic_browse_active = false;
        http_cancel_token_destroy(&subsonic_browse_cancel);
        free(req);
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_subsonic_browse(void) {
    if (!subsonic_browse_active) return;
    if (!subsonic_browse_timed_out &&
        lv_tick_elaps(subsonic_browse_started_at) >= SUBSONIC_REQUEST_TIMEOUT_MS) {
        subsonic_browse_timed_out = true;
        http_cancel_token_cancel(&subsonic_browse_cancel);
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
        show_error_toast(TR("Server request timed out after 30 seconds"));
        /* Keep the worker and its buffers alive; a later poll reaps it. */
        return;
    }
    if (!atomic_load_explicit(&subsonic_browse_done_flag, memory_order_acquire)) return;

    subsonic_browse_active = false;
    pthread_join(subsonic_browse_thread, NULL);
    http_cancel_token_destroy(&subsonic_browse_cancel);
    bool success = subsonic_browse_success_flag && !subsonic_browse_timed_out;
    int depth_before = gui_navigation_get_depth();

    if (success) {
        switch (subsonic_browse_result_kind) {
            case SUBSONIC_BROWSE_ARTISTS:
                compact_list_set_items(subsonic_artists_list, NULL, 0);
                free(subsonic_artists_cache);
                subsonic_artists_cache = subsonic_browse_result_artists;
                subsonic_browse_result_artists = NULL;
                subsonic_artists_count = subsonic_browse_result_count;
                subsonic_artists_loaded = populate_subsonic_artists();
                if (subsonic_artists_loaded) {
                    nav_push(subsonic_artists_screen);
                } else {
                    success = false;
                    snprintf(subsonic_browse_result_error, sizeof(subsonic_browse_result_error),
                             TR("Not enough memory to load artists"));
                }
                break;
            case SUBSONIC_BROWSE_ALBUM_SONGS:
            case SUBSONIC_BROWSE_PLAYLIST_SONGS:
                free(subsonic_songs_cache);
                subsonic_songs_cache = subsonic_browse_result_songs;
                subsonic_songs_count = subsonic_browse_result_count;
                subsonic_songs_context_is_playlist =
                    subsonic_browse_result_kind == SUBSONIC_BROWSE_PLAYLIST_SONGS;
                if (subsonic_songs_context_is_playlist) {
                    snprintf(subsonic_songs_context_playlist_name, sizeof(subsonic_songs_context_playlist_name), "%s",
                             subsonic_browse_result_title);
                }
                lv_label_set_text(subsonic_songs_title_label, subsonic_browse_result_title);
                lv_obj_clear_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
                populate_indexed_list(subsonic_songs_list, subsonic_songs_count, subsonic_song_label_of,
                                      subsonic_song_row_click_cb);
                nav_push(subsonic_songs_screen);
                break;
            case SUBSONIC_BROWSE_ARTIST_ALBUMS:
            case SUBSONIC_BROWSE_ALL_ALBUMS:
                free(subsonic_albums_cache);
                subsonic_albums_cache = subsonic_browse_result_albums;
                subsonic_albums_count = subsonic_browse_result_count;
                if (subsonic_browse_result_kind == SUBSONIC_BROWSE_ARTIST_ALBUMS) {
                    snprintf(subsonic_albums_context_artist, sizeof(subsonic_albums_context_artist), "%s",
                             subsonic_browse_result_title);
                    lv_obj_clear_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
                } else {
                    subsonic_albums_context_artist[0] = '\0';
                    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
                }
                lv_label_set_text(subsonic_albums_title_label, subsonic_browse_result_title);
                {
                    /* getAlbumList2.view's own "every album" browse can return
                     * up to 500 rows (see subsonic_client.c's own size=500) --
                     * real widget-explosion risk populate_indexed_list() used
                     * to hit here, same class as the local library's pre-
                     * virtualization All Songs/Artists/Albums screens. */
                    compact_list_item_t * items =
                        malloc(sizeof(compact_list_item_t) * (size_t) (subsonic_albums_count > 0 ? subsonic_albums_count : 1));
                    for (int i = 0; i < subsonic_albums_count; i++) items[i] = (compact_list_item_t){ subsonic_album_label_of(i) };
                    compact_list_set_items(subsonic_albums_list, items, subsonic_albums_count);
                    free(items);
                }
                nav_push(subsonic_albums_screen);
                break;
            case SUBSONIC_BROWSE_PLAYLISTS:
                free(subsonic_playlists_cache);
                subsonic_playlists_cache = subsonic_browse_result_playlists;
                subsonic_playlists_count = subsonic_browse_result_count;
                populate_indexed_list(subsonic_playlists_list, subsonic_playlists_count, subsonic_playlist_label_of,
                                      subsonic_playlist_row_click_cb);
                nav_push(subsonic_playlists_screen);
                break;
        }
    } else {
        free(subsonic_browse_result_artists);
        subsonic_browse_result_artists = NULL;
        free(subsonic_browse_result_songs);
        subsonic_browse_result_songs = NULL;
        free(subsonic_browse_result_albums);
        subsonic_browse_result_albums = NULL;
        free(subsonic_browse_result_playlists);
        subsonic_browse_result_playlists = NULL;
    }

    /* Timeout already dismissed busy and notified the user. */
    if (subsonic_browse_timed_out) return;
    if (gui_navigation_get_depth() > depth_before) nav_remove_stack_slot(depth_before - 1);
    else gui_busy_hide(subsonic_browse_token);
    subsonic_browse_token = 0;
    if (!success) show_error_toast(subsonic_browse_result_error);
}

static void subsonic_album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ALBUMS, index);

    if (index < 0 || index >= subsonic_albums_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_ALBUM_SONGS, subsonic_albums_cache[index].id,
                          subsonic_albums_cache[index].name);
}

static void subsonic_artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, index);

    if (index < 0 || index >= subsonic_artists_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_ARTIST_ALBUMS, subsonic_artists_cache[index].id,
                          subsonic_artists_cache[index].name);
}

static void subsonic_playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    if (index < 0 || index >= subsonic_playlists_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_PLAYLIST_SONGS, subsonic_playlists_cache[index].id,
                          subsonic_playlists_cache[index].name);
}

static void subsonic_menu_artists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_connect_active || subsonic_browse_active) {
        show_info_toast(TR("Previous request still finishing"));
        return;
    }
    if (subsonic_artists_loaded) {
        nav_push(subsonic_artists_screen);
    } else {
        start_subsonic_browse(SUBSONIC_BROWSE_ARTISTS, NULL, TR("Artists"));
    }
}

static void subsonic_menu_playlists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    start_subsonic_browse(SUBSONIC_BROWSE_PLAYLISTS, NULL, TR("Playlists"));
}

static void subsonic_menu_albums_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    start_subsonic_browse(SUBSONIC_BROWSE_ALL_ALBUMS, NULL, TR("Albums"));
}

static subsonic_download_pending_t subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;

static gui_popup_t subsonic_download_confirm_popup;

static lv_obj_t * subsonic_download_confirm_title;

static void hide_subsonic_download_confirm_popup(void) {
    gui_popup_hide(&subsonic_download_confirm_popup);
    subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;
}

static void subsonic_download_confirm_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void subsonic_download_confirm_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void show_subsonic_download_confirm_popup(subsonic_download_pending_t kind, const char * msg) {
    subsonic_download_pending = kind;
    lv_label_set_text(subsonic_download_confirm_title, msg);
    gui_popup_show(&subsonic_download_confirm_popup);
}

static void subsonic_download_songs_now(void) {
    if (subsonic_songs_count == 0) return;

    subsonic_song_t * songs_copy = malloc(sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);
    if (!songs_copy) { show_error_toast(TR("Not enough memory to start download")); return; }
    memcpy(songs_copy, subsonic_songs_cache, sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);

    const char * playlist_name = subsonic_songs_context_is_playlist ? subsonic_songs_context_playlist_name : NULL;
    char label[192];
    snprintf(label, sizeof(label), TR("Downloading\n%s..."), lv_label_get_text(subsonic_songs_title_label));
    start_subsonic_library_download(songs_copy, subsonic_songs_count, NULL, 0, playlist_name, label);
}

static void subsonic_download_songs_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_songs_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), TR("Download \"%s\"?"), lv_label_get_text(subsonic_songs_title_label));
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_SONGS, msg);
}

static void subsonic_download_artist_now(void) {
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    subsonic_album_t * albums_copy = malloc(sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);
    if (!albums_copy) { show_error_toast(TR("Not enough memory to start download")); return; }
    memcpy(albums_copy, subsonic_albums_cache, sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);

    char label[192];
    snprintf(label, sizeof(label), TR("Downloading\n%s..."), subsonic_albums_context_artist);
    start_subsonic_library_download(NULL, 0, albums_copy, subsonic_albums_count, NULL, label);
}

static void subsonic_download_artist_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), TR("Download every album from \"%s\"?"), subsonic_albums_context_artist);
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_ARTIST, msg);
}

static void subsonic_download_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_download_pending_t kind = subsonic_download_pending;
    hide_subsonic_download_confirm_popup();
    if (kind == SUBSONIC_DOWNLOAD_PENDING_SONGS) subsonic_download_songs_now();
    else if (kind == SUBSONIC_DOWNLOAD_PENDING_ARTIST) subsonic_download_artist_now();
}

static void build_subsonic_download_confirm_popup(void) {
    subsonic_download_confirm_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &subsonic_download_confirm_title, NULL, TR("Download"), accent_lv_color(),
        subsonic_download_confirm_cb, NULL, TR("Cancel"), lv_color_make(160, 160, 160), subsonic_download_confirm_cancel_cb,
        NULL, subsonic_download_confirm_backdrop_cb, &subsonic_download_confirm_popup.backdrop);
}

static pthread_t subsonic_connect_thread;
static http_cancel_token_t subsonic_connect_cancel;

bool subsonic_connect_active = false;
static bool subsonic_connect_timed_out = false;
static uint32_t subsonic_connect_started_at = 0;

static atomic_bool subsonic_connect_done_flag = false;

static volatile bool subsonic_connect_success_flag = false;

static bool subsonic_connect_artists_success = false;
static subsonic_artist_t * subsonic_connect_result_artists = NULL;
static int subsonic_connect_result_artists_count = 0;
static char subsonic_connect_result_error[256];

static subsonic_server_t subsonic_connect_pending_server;

static void * subsonic_connect_thread_func(void * arg) {
    subsonic_connect_request_t * req = (subsonic_connect_request_t *) arg;

    bool authenticated = subsonic_ping(&req->server, &subsonic_connect_cancel);
    if (authenticated) {
        /* Authentication and initial browsing are separate operations. A
         * Navidrome account with an empty or temporarily unavailable artist
         * index is still a valid connection. */
        subsonic_connect_artists_success =
            subsonic_get_artists(&req->server, &subsonic_connect_result_artists,
                                 &subsonic_connect_result_artists_count, &subsonic_connect_cancel);
        if (!subsonic_connect_artists_success) {
            subsonic_copy_request_error(subsonic_connect_result_error, sizeof(subsonic_connect_result_error),
                                        TR("Failed to load artists"));
        }
    } else {
        subsonic_copy_request_error(subsonic_connect_result_error, sizeof(subsonic_connect_result_error),
                                    TR("Failed to connect to server"));
    }

    subsonic_connect_success_flag = authenticated;
    atomic_store_explicit(&subsonic_connect_done_flag, true, memory_order_release); /* written last -- poll_subsonic_connect only checks this flag */
    free(req);
    return NULL;
}

void poll_subsonic_connect(void) {
    if (!subsonic_connect_active) return;
    if (!subsonic_connect_timed_out &&
        lv_tick_elaps(subsonic_connect_started_at) >= SUBSONIC_REQUEST_TIMEOUT_MS) {
        subsonic_connect_timed_out = true;
        http_cancel_token_cancel(&subsonic_connect_cancel);
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
        show_error_toast(TR("Connection timed out after 30 seconds"));
        /* Do not join a request that may still be blocked in the network. */
        return;
    }
    if (!atomic_load_explicit(&subsonic_connect_done_flag, memory_order_acquire)) return;

    subsonic_connect_active = false;
    pthread_join(subsonic_connect_thread, NULL);
    http_cancel_token_destroy(&subsonic_connect_cancel);
    if (subsonic_connect_timed_out) {
        free(subsonic_connect_result_artists);
        subsonic_connect_result_artists = NULL;
        subsonic_connect_result_artists_count = 0;
        return; /* Discard even successful late authentication without navigation. */
    }
    bool success = subsonic_connect_success_flag;

    /* Same nav_pop()-races-a-navigating-callback issue already fixed for
     * Wi-Fi manual SSID entry and the Subsonic download-then-play path --
     * see text_entry_kb_event_cb's own comment for the full mechanism, and
     * poll_subsonic_download()'s own comment for why skipping nav_pop()
     * alone isn't enough either (it leaves this screen's own stack slot
     * stuck underneath the artists screen forever) -- nav_remove_stack_
     * slot() splices it out instead, once it's clear the artists screen
     * already took its place. */
    int depth_before = gui_navigation_get_depth();
    if (success) {
        /* Becomes the active connection everything else in this app's
         * Subsonic browsing/download flow reads via subsonic_server_from_
         * settings() -- Saved Servers and New Connection both funnel
         * through this one function, so this is the one place that needs
         * to update it, regardless of which screen the user connected
         * from. Also persisted to the Saved Servers list itself (an
         * upsert, so reconnecting to an already-saved server with updated
         * credentials just refreshes it rather than duplicating it). */
        snprintf(current_settings.subsonic_url, sizeof(current_settings.subsonic_url), "%s",
                 subsonic_connect_pending_server.base_url);
        snprintf(current_settings.subsonic_username, sizeof(current_settings.subsonic_username), "%s",
                 subsonic_connect_pending_server.username);
        snprintf(current_settings.subsonic_password, sizeof(current_settings.subsonic_password), "%s",
                 subsonic_connect_pending_server.password);
        current_settings.subsonic_verify_tls = subsonic_connect_pending_server.verify_tls;
        metadata_db_subsonic_server_save(subsonic_connect_pending_server.base_url, subsonic_connect_pending_server.username,
                                          subsonic_connect_pending_server.password, subsonic_connect_pending_server.verify_tls);
        settings_subsonic_server_upsert(&current_settings, subsonic_connect_pending_server.base_url,
                                        subsonic_connect_pending_server.username, subsonic_connect_pending_server.password,
                                        subsonic_connect_pending_server.verify_tls);
        settings_save(&current_settings);

        /* Only commit the new connection's cache on the UI thread. Failed
         * preload remains retryable; a successful empty list stays cached. */
        subsonic_artists_loaded = false;
        compact_list_set_items(subsonic_artists_list, NULL, 0);
        free(subsonic_artists_cache);
        subsonic_artists_cache = NULL;
        subsonic_artists_count = 0;
        if (subsonic_connect_artists_success) {
            subsonic_artists_cache = subsonic_connect_result_artists;
            subsonic_connect_result_artists = NULL;
            subsonic_artists_count = subsonic_connect_result_artists_count;
            subsonic_artists_loaded = populate_subsonic_artists();
            if (!subsonic_artists_loaded) {
                snprintf(subsonic_connect_result_error, sizeof(subsonic_connect_result_error),
                         TR("Not enough memory to load artists"));
            }
        }
        nav_push(subsonic_menu_screen);
    }
    free(subsonic_connect_result_artists);
    subsonic_connect_result_artists = NULL;
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        gui_busy_hide(subsonic_connect_token);
    }
    subsonic_connect_token = 0;
    if (!success) {
        show_error_toast(subsonic_connect_result_error);
    } else if (!subsonic_artists_loaded) {
        char message[320];
        snprintf(message, sizeof(message), TR("Failed to load artists: %s"), subsonic_connect_result_error);
        show_error_toast(message);
    }
}

/* Bug report: opening Subsonic (or attempting a connection from within it)
 * with no real Wi-Fi connection established just sat in the "Connecting to
 * server..." busy overlay until the underlying socket failed, then
 * -- per this file's own documented gap just above -- silently landed back
 * wherever nav_pop() left off, with no indication anything failed. Unlike
 * the four Wireless tiles' own guard (gui_network.c's wifi_feature_guard(),
 * which only requires the RADIO to be on -- Wi-Fi ON but disconnected is
 * fine for those, since they're local-network features with their own
 * "connect first" in-screen state), Subsonic talks to a remote server, so
 * radio-on alone isn't enough: this checks wifi_get_status(), the same
 * "wpa_state=COMPLETED" real-association check the topbar's own Wi-Fi icon
 * uses, already an accepted synchronous-on-the-UI-thread call elsewhere in
 * this codebase (refresh_wifi_icon(), gui_shell.c) since it's a single fast
 * subprocess call, not a network round-trip of its own. Checked at the
 * tile (don't even open the entry screen) AND here in start_subsonic_
 * connect() (the actual single choke point both Saved Servers and New
 * Connection's own "Connect & Browse" already funnel through) -- the
 * screen can already be open from before Wi-Fi dropped, same reasoning as
 * every other Wi-Fi-dependent feature's defensive enable guard. */
static bool subsonic_wifi_connected_guard(void) {
    int level;
    if (wifi_get_status(&level)) return true;
    show_error_toast(TR("Connect to Wi-Fi first"));
    return false;
}

static void start_subsonic_connect(const subsonic_server_t * server) {
    if (subsonic_connect_active || subsonic_browse_active) {
        show_info_toast(TR("Previous request still finishing"));
        return;
    }
    if (!subsonic_wifi_connected_guard()) return;
    subsonic_connect_pending_server = *server;

    subsonic_connect_request_t * req = malloc(sizeof(*req));
    if (!req) {
        show_error_toast(TR("Not enough memory to connect"));
        return;
    }
    req->server = *server;

    atomic_store_explicit(&subsonic_connect_done_flag, false, memory_order_relaxed);
    subsonic_connect_success_flag = false;
    subsonic_connect_timed_out = false;
    subsonic_connect_started_at = lv_tick_get();
    subsonic_connect_artists_success = false;
    subsonic_connect_result_artists = NULL;
    subsonic_connect_result_artists_count = 0;
    subsonic_connect_result_error[0] = '\0';
    http_cancel_token_init(&subsonic_connect_cancel);
    subsonic_connect_active = true;

    subsonic_connect_token = gui_busy_show(TR("Connecting to server..."), "");

    if (pthread_create(&subsonic_connect_thread, NULL, subsonic_connect_thread_func, req) != 0) {
        free(req);
        subsonic_connect_active = false;
        http_cancel_token_destroy(&subsonic_connect_cancel);
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
        show_error_toast(TR("Failed to start connection"));
    }
}

static subsonic_server_row_t * subsonic_saved_servers = NULL;

static int subsonic_saved_server_count = 0;

static void subsonic_saved_server_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_server_row_t * row = &subsonic_saved_servers[index];

    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", row->url);
    snprintf(server.username, sizeof(server.username), "%s", row->username);
    snprintf(server.password, sizeof(server.password), "%s", row->password);
    server.verify_tls = row->verify_tls;
    start_subsonic_connect(&server);
}

static const char * subsonic_saved_server_label_of(int i) { return subsonic_saved_servers[i].url; }

static void populate_subsonic_saved_servers_screen(void) {
    free(subsonic_saved_servers);
    subsonic_saved_servers = NULL;
    subsonic_saved_server_count = 0;
    metadata_db_load_subsonic_servers(&subsonic_saved_servers, &subsonic_saved_server_count);

    lv_obj_clean(subsonic_saved_servers_list);
    if (subsonic_saved_server_count == 0) {
        lv_obj_t * title = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(title, TR("No saved servers"));
        lv_obj_add_style(title, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_width(title, lv_pct(100));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t * hint = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(hint, TR("Go back and choose New Connection to add one."));
        lv_obj_add_style(hint, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_width(hint, lv_pct(88));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    populate_indexed_list(subsonic_saved_servers_list, subsonic_saved_server_count, subsonic_saved_server_label_of,
                          subsonic_saved_server_row_cb);
}

static lv_obj_t * build_subsonic_saved_servers_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Saved Servers"), &title_label, &subsonic_saved_servers_list);
}

static void subsonic_saved_servers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_subsonic_saved_servers_screen();
    nav_push(subsonic_saved_servers_screen);
}

static subsonic_server_t subsonic_new_conn_form = { .verify_tls = true };

static lv_obj_t * subsonic_new_connection_list;

static void populate_subsonic_new_connection_screen(void); /* defined below, after the row callbacks it references */

static void subsonic_new_conn_url_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.base_url, sizeof(subsonic_new_conn_form.base_url), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_url_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char title[96];
    snprintf(title, sizeof(title), TR("Server URL (e.g. %s)"), "https://myserver:4040");
    show_text_entry(title, subsonic_new_conn_form.base_url, false, false,
                    subsonic_new_conn_url_entry_done, NULL);
}

static void subsonic_new_conn_username_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.username, sizeof(subsonic_new_conn_form.username), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_username_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Username"), subsonic_new_conn_form.username, false, false, subsonic_new_conn_username_entry_done, NULL);
}

static void subsonic_new_conn_password_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.password, sizeof(subsonic_new_conn_form.password), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_password_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Password"), subsonic_new_conn_form.password, true, false, subsonic_new_conn_password_entry_done, NULL);
}

static void subsonic_new_conn_verify_tls_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_new_conn_form.verify_tls = !subsonic_new_conn_form.verify_tls;
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_connect_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_subsonic_connect(&subsonic_new_conn_form);
}

static void populate_subsonic_new_connection_screen(void) {
    lv_obj_clean(subsonic_new_connection_list);

    char url_text[300];
    snprintf(url_text, sizeof(url_text), TR("Server URL: %s"),
             subsonic_new_conn_form.base_url[0] ? subsonic_new_conn_form.base_url : TR("Not set"));
    add_pill_chevron_row(subsonic_new_connection_list, url_text, subsonic_new_conn_url_row_cb);

    add_pill_toggle_row(subsonic_new_connection_list, TR("Verify server certificate"), subsonic_new_conn_form.verify_tls,
                        subsonic_new_conn_verify_tls_toggle_cb);

    char username_text[160];
    snprintf(username_text, sizeof(username_text), TR("Username: %s"),
             subsonic_new_conn_form.username[0] ? subsonic_new_conn_form.username : TR("Not set"));
    add_pill_chevron_row(subsonic_new_connection_list, username_text, subsonic_new_conn_username_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, subsonic_new_conn_form.password[0] ? TR("Password: Set") : TR("Password: Not set"),
                         subsonic_new_conn_password_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, TR("Connect & Browse"), subsonic_new_conn_connect_row_cb);
}

static lv_obj_t * build_subsonic_new_connection_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("New Connection"), &title_label, &subsonic_new_connection_list);
}

static void subsonic_new_connection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Only the text fields reset -- verify_tls deliberately carries over
     * between visits (no widget-desync risk here anymore, unlike the old
     * build_pill_list_screen()-based version -- this screen's rows are
     * fully rebuilt by populate_subsonic_new_connection_screen() below on
     * every visit, so there's nothing stale left to desync from). */
    subsonic_new_conn_form.base_url[0] = '\0';
    subsonic_new_conn_form.username[0] = '\0';
    subsonic_new_conn_form.password[0] = '\0';
    populate_subsonic_new_connection_screen();
    nav_push(subsonic_new_connection_screen);
}

static lv_obj_t * build_subsonic_entry_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ TR("Saved Servers"), PILL_ACCESSORY_CHEVRON, false, subsonic_saved_servers_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("New Connection"), PILL_ACCESSORY_CHEVRON, false, subsonic_new_connection_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("Subsonic"), generic_back_cb, items, 2, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void subsonic_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!subsonic_wifi_connected_guard()) return;
    nav_push(subsonic_entry_screen);
}

void gui_subsonic_init(void) {
    subsonic_entry_screen = build_subsonic_entry_screen();
    subsonic_saved_servers_screen = build_subsonic_saved_servers_screen();
    subsonic_new_connection_screen = build_subsonic_new_connection_screen();

    /* Subsonic screen redesign: the menu (Artists/Playlists/Albums) that's
     * now the first screen after connecting -- see poll_subsonic_connect().
     * Rows built once here, not repopulated per visit, since this list
     * never changes. */
    subsonic_menu_screen = build_subsonic_list_screen(TR("Subsonic"), &subsonic_menu_title_label, &subsonic_menu_list);
    subsonic_quality_screen = build_subsonic_quality_screen();
    subsonic_download_settings_screen = build_subsonic_download_settings_screen();
    {
        char quality_label[64];
        snprintf(quality_label, sizeof(quality_label), TR("Stream quality: %s"),
                 subsonic_quality_name(current_settings.subsonic_stream_quality));
        subsonic_quality_row = add_pill_chevron_row(subsonic_menu_list, quality_label, subsonic_quality_row_cb);
        lv_obj_set_style_text_font(lv_obj_get_child(subsonic_quality_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);

        subsonic_download_settings_row = add_pill_chevron_row(subsonic_menu_list,
            TR("Download settings"), subsonic_download_settings_row_cb);
        lv_obj_set_style_text_font(lv_obj_get_child(subsonic_download_settings_row, 0),
                                   gui_theme_font(GUI_FONT_ROLE_BODY), 0);

        lv_obj_t * artists_row = add_pill_row_base(subsonic_menu_list, TR("Artists"));
        lv_obj_set_style_text_font(lv_obj_get_child(artists_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(artists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(artists_row, subsonic_menu_artists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * playlists_row = add_pill_row_base(subsonic_menu_list, TR("Playlists"));
        lv_obj_set_style_text_font(lv_obj_get_child(playlists_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(playlists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(playlists_row, subsonic_menu_playlists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * albums_row = add_pill_row_base(subsonic_menu_list, TR("Albums"));
        lv_obj_set_style_text_font(lv_obj_get_child(albums_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(albums_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(albums_row, subsonic_menu_albums_row_cb, LV_EVENT_CLICKED, NULL);
    }

    subsonic_artists_screen = build_compact_list_screen(TR("Artists"), generic_back_cb, NULL, 0, subsonic_artist_row_click_cb,
                                                          NULL, &subsonic_artists_list, &subsonic_artists_title_label,
                                                          LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    subsonic_albums_screen = build_compact_list_screen(TR("Albums"), generic_back_cb, NULL, 0, subsonic_album_row_click_cb,
                                                         NULL, &subsonic_albums_list, &subsonic_albums_title_label,
                                                         LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    /* Finalize navigation handlers and gesture support for swipe-back navigation. */
    finalize_screen_navigation(subsonic_artists_screen);
    finalize_screen_navigation(subsonic_albums_screen);
    subsonic_songs_screen = build_subsonic_list_screen(TR("Songs"), &subsonic_songs_title_label, &subsonic_songs_list);
    subsonic_playlists_screen = build_subsonic_list_screen(TR("Playlists"), &subsonic_playlists_title_label, &subsonic_playlists_list);

    subsonic_albums_download_btn = lv_image_create(subsonic_albums_screen);
    lv_image_set_src(subsonic_albums_download_btn, asset_path("stream_media/download.png"));
    lv_obj_set_style_image_recolor(subsonic_albums_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_image_recolor_opa(subsonic_albums_download_btn, LV_OPA_COVER, 0);
    align_screen_header_action(subsonic_albums_download_btn, BOARD_SCALE_PX(87));
    lv_obj_set_ext_click_area(subsonic_albums_download_btn, BOARD_SCALE_PX(16));
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_albums_download_btn, subsonic_download_artist_btn_cb, LV_EVENT_CLICKED, NULL);
    reserve_title_width_before(subsonic_albums_title_label, subsonic_albums_download_btn);

    subsonic_songs_download_btn = lv_image_create(subsonic_songs_screen);
    lv_image_set_src(subsonic_songs_download_btn, asset_path("stream_media/download.png"));
    lv_obj_set_style_image_recolor(subsonic_songs_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_image_recolor_opa(subsonic_songs_download_btn, LV_OPA_COVER, 0);
    align_screen_header_action(subsonic_songs_download_btn, BOARD_SCALE_PX(20));
    lv_obj_set_ext_click_area(subsonic_songs_download_btn, BOARD_SCALE_PX(16));
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_songs_download_btn, subsonic_download_songs_btn_cb, LV_EVENT_CLICKED, NULL);
    reserve_title_width_before(subsonic_songs_title_label, subsonic_songs_download_btn);

    build_subsonic_download_confirm_popup();

    register_search(SEARCH_BINDING_SUBSONIC_ARTISTS, subsonic_artists_screen, subsonic_artists_list, subsonic_artist_label_of,
                     &subsonic_artists_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
    register_search(SEARCH_BINDING_SUBSONIC_ALBUMS, subsonic_albums_screen, subsonic_albums_list, subsonic_album_label_of,
                     &subsonic_albums_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_subsonic_init() can rebuild them from a clean slate
 * without leaking the old objects. subsonic_download_confirm_popup and its
 * backdrop are built directly on lv_layer_top() (see build_confirm_popup()'s
 * own comment), not as children of any of these screens, so they need their
 * own explicit deletion. re-running gui_subsonic_init() also re-runs
 * register_search() for the two virtualized lists below, which already
 * frees its own prior state on re-registration (gui_library.c) -- nothing
 * extra needed here for that. */
void gui_subsonic_teardown(void) {
    subsonic_artists_loaded = false;
    gui_popup_teardown(&subsonic_download_confirm_popup);
    if (subsonic_entry_screen) { lv_obj_delete(subsonic_entry_screen); subsonic_entry_screen = NULL; }
    if (subsonic_saved_servers_screen) { lv_obj_delete(subsonic_saved_servers_screen); subsonic_saved_servers_screen = NULL; }
    if (subsonic_new_connection_screen) { lv_obj_delete(subsonic_new_connection_screen); subsonic_new_connection_screen = NULL; }
    if (subsonic_menu_screen) { lv_obj_delete(subsonic_menu_screen); subsonic_menu_screen = NULL; }
    if (subsonic_quality_screen) { lv_obj_delete(subsonic_quality_screen); subsonic_quality_screen = NULL; }
    if (subsonic_download_settings_screen) {
        lv_obj_delete(subsonic_download_settings_screen);
        subsonic_download_settings_screen = NULL;
    }
    memset(subsonic_quality_options, 0, sizeof(subsonic_quality_options));
    subsonic_quality_list = NULL;
    subsonic_quality_row = NULL;
    subsonic_download_settings_list = NULL;
    subsonic_download_folder_row = NULL;
    memset(subsonic_download_layout_options, 0, sizeof(subsonic_download_layout_options));
    subsonic_download_settings_row = NULL;
    if (subsonic_artists_screen) { lv_obj_delete(subsonic_artists_screen); subsonic_artists_screen = NULL; }
    if (subsonic_albums_screen) { lv_obj_delete(subsonic_albums_screen); subsonic_albums_screen = NULL; }
    if (subsonic_songs_screen) { lv_obj_delete(subsonic_songs_screen); subsonic_songs_screen = NULL; }
    if (subsonic_playlists_screen) { lv_obj_delete(subsonic_playlists_screen); subsonic_playlists_screen = NULL; }
}

bool gui_subsonic_has_background_work(void) {
    return download_active || subsonic_library_download_active || subsonic_connect_active || subsonic_browse_active;
}

void gui_subsonic_handle_wifi_disabled(void) {
    /* The activity flags remain set until the normal poll functions join
     * their workers and dismiss their UI. Cancellation only interrupts the
     * socket here; it never blocks the GUI thread waiting for DNS/TCP/TLS. */
    if (subsonic_connect_active) http_cancel_token_cancel(&subsonic_connect_cancel);
    if (subsonic_browse_active) http_cancel_token_cancel(&subsonic_browse_cancel);
    if (download_active) http_cancel_token_cancel(&download_cancel);
    if (subsonic_library_download_active) http_cancel_token_cancel(&subsonic_library_download_cancel);
}

void gui_subsonic_cancel_background_work(void) {
    gui_subsonic_handle_wifi_disabled();
    if (subsonic_connect_active) {
        pthread_join(subsonic_connect_thread, NULL);
        subsonic_connect_active = false;
        http_cancel_token_destroy(&subsonic_connect_cancel);
        free(subsonic_connect_result_artists);
        subsonic_connect_result_artists = NULL;
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
    }
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
    if (subsonic_browse_active) {
        pthread_join(subsonic_browse_thread, NULL);
        subsonic_browse_active = false;
        http_cancel_token_destroy(&subsonic_browse_cancel);
        free(subsonic_browse_result_artists);
        subsonic_browse_result_artists = NULL;
        free(subsonic_browse_result_songs);
        subsonic_browse_result_songs = NULL;
        free(subsonic_browse_result_albums);
        subsonic_browse_result_albums = NULL;
        free(subsonic_browse_result_playlists);
        subsonic_browse_result_playlists = NULL;
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
    }
}
