#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "gui_library.h"
#include "gui_folder_actions.h"
#include "../playlists/gui_playlists.h"
#include "../playlists/gui_playlist_target.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/metadata/scan/library_scan.h"
#include "../../settings/power/gui_power_hub.h"
#include "../library_selection_snapshot.h"
#include "../library_az_index.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../library_thumbnails.h"
#include "gui_list_reorder.h"
#include "../../../core/updates/firmware_update.h"
#include "../../network/gui_network.h"
#include "../../shell/gui_shell.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../player/screen/gui_queue.h"
#include "../../assets/assets.h"
#include "../../../core/system/lifecycle/idle_shutdown.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../player/screen/gui_player.h"
#include "../../../network/remote/control/remote_control.h"
#include "../../effects/frosted_glass.h"
#include "../../effects/transition_compositor.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"

extern void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
typedef struct file_browser_index file_browser_index_t;
extern void on_file_browser_index_selected(file_browser_index_t *index, unsigned playable_count,
                                           unsigned selected_playable);
extern void open_queue_screen(void);
extern void nav_remove_stack_slot(int depth);
extern void mount_sd_card_if_needed(void);
extern void library_load_from_cache_only(void);
extern void remote_control_finish_collection(bool success, const char *error);
void refresh_artist_albums_now_playing_indicator(void);
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_notifications.h"
#include "../../settings/gui_settings.h"
#include "../../text/rendering/gui_text_input.h"
#include "../collections/gui_books.h"
#include "../../text/rendering/fallback_font.h"
#include "../../../plugins/manager/plugin_manager.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/metadata/catalog/metadata_refresh_targets.h"
#include "../../../core/storage/sd_fsck_run.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../core/ui/display/ui_wake.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/browser/files/file_ops.h"
#include "../../storage/sd_card_identity.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"
#include "../../../hardware/device/device_config.h"
#include "../../../core/themes/ui_customization.h"
#include "../../../core/persistence/settings.h"
#include "../../../audio/engine/audio.h"
#include "../../../audio/probe/track_probe.h"
#include "../../../core/process/subprocess.h"
#include "../../../core/system/logging/db_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
/* SCHED_BATCH below is glibc's own <bits/sched.h> policy constant, gated
 * behind _GNU_SOURCE there -- this file deliberately doesn't define that
 * (see search_matches()'s own comment on strcasestr() for why). The raw
 * kernel UAPI header defines the same numeric policy constants ungated on
 * both glibc and musl (confirmed in both toolchains' linux/sched.h), so
 * this alone is enough on host and target alike without touching feature-
 * test macros anywhere in this file. */
#include <linux/sched.h>
#include <sys/resource.h>

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define COMPACT_LIST_PAGE_CACHE_SIZE 64
/* STATUS_BAR_CLEARANCE is provided by screen_builders.h. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)
extern bool library_rescan_active;
static bool group_songs_source_is_album = false;
static bool group_songs_loading;
static bool sd_format_active;
static uint64_t album_load_generation;
static void cancel_album_load(void);
typedef enum {
    GROUP_SONGS_REFRESH_NONE,
    GROUP_SONGS_REFRESH_FAVORITES,
    GROUP_SONGS_REFRESH_MOST_PLAYED,
    GROUP_SONGS_REFRESH_M3U,
    GROUP_SONGS_REFRESH_ALBUM,
    GROUP_SONGS_REFRESH_ARTIST,
    GROUP_SONGS_REFRESH_GENRE
} group_songs_refresh_source_t;
static group_songs_refresh_source_t group_songs_refresh_source;
static char group_songs_refresh_album[128];
static char group_songs_refresh_album_artist[128];
static char group_songs_refresh_artist_filter[128];
static char group_songs_refresh_artist_name[128];
static metadata_db_group_kind_t group_songs_refresh_artist_kind;
static char group_songs_refresh_m3u_path[PATH_MAX];
static char group_songs_refresh_genre[METADATA_DB_TEXT_MAX] = {0};
static gui_busy_handle_t library_rescan_token = 0;
static gui_busy_handle_t sd_format_token = 0;


static void refresh_group_song_thumbnails(void);
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static bool resolve_album_display_row(int index, group_row_t * out);
static void populate_artist_albums_view(const char * name, metadata_db_group_kind_t kind);
/* Genre browser (issue #191), defined with the Genres screen. */
enum { GENRE_VIEW_SONGS, GENRE_VIEW_ARTISTS, GENRE_VIEW_ALBUM_ARTISTS, GENRE_VIEW_ALBUMS, GENRE_VIEW_COUNT };
static lv_obj_t * genre_groups_list;
static lv_obj_t * genre_albums_list;
static int genre_browse_view(void);
static void rearm_library_list(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, int total_count);
static pthread_mutex_t artist_albums_state_mutex;
static metadata_db_group_kind_t artist_albums_current_kind;
static metadata_db_album_sort_t artist_albums_current_sort;
static char artist_albums_current_name[128];
static int artist_albums_group_count;

static metadata_db_album_sort_t effective_album_sort(void) {
    metadata_db_album_sort_t sort = (metadata_db_album_sort_t) current_settings.album_sort_mode;
    return metadata_db_album_sort_available(sort) ? sort : METADATA_DB_ALBUM_SORT_NAME;
}

/* Pure scalar validation for refresh requests; unlike effective_album_sort(),
 * this does not ask the database whether an optional sort is available. */
static metadata_db_album_sort_t requested_album_sort(void) {
    metadata_db_album_sort_t sort = (metadata_db_album_sort_t) current_settings.album_sort_mode;
    return sort >= METADATA_DB_ALBUM_SORT_NAME && sort <= METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR
        ? sort : METADATA_DB_ALBUM_SORT_NAME;
}

static int configured_group_album_sort(metadata_db_group_kind_t kind) {
    if (kind == METADATA_DB_GROUP_ARTIST) return current_settings.artist_albums_sort_mode;
    if (kind == METADATA_DB_GROUP_ALBUM_ARTIST) return current_settings.album_artist_albums_sort_mode;
    return current_settings.album_sort_mode;
}

static metadata_db_album_sort_t effective_group_album_sort(metadata_db_group_kind_t kind) {
    int value = configured_group_album_sort(kind);
    metadata_db_album_sort_t sort = (metadata_db_album_sort_t) value;
    metadata_db_album_sort_t max_sort = METADATA_DB_ALBUM_SORT_YEAR;
    if (sort < METADATA_DB_ALBUM_SORT_NAME || sort > max_sort)
        return METADATA_DB_ALBUM_SORT_NAME;
    return metadata_db_album_sort_available(sort) ? sort : METADATA_DB_ALBUM_SORT_NAME;
}

static metadata_db_album_sort_t requested_group_album_sort(metadata_db_group_kind_t kind) {
    int value = configured_group_album_sort(kind);
    return value >= METADATA_DB_ALBUM_SORT_NAME && value <= METADATA_DB_ALBUM_SORT_YEAR
        ? (metadata_db_album_sort_t) value : METADATA_DB_ALBUM_SORT_NAME;
}

/* Playback can change while the library database is busy. Resolve the
 * database-only part on one coalescing worker and leave all list/search/LVGL
 * mapping on the UI thread. The request and result contain copied scalars;
 * the worker never observes UI-owned state. */
typedef struct {
    uint64_t generation;
    uint64_t lifetime;
    char path[sizeof(now_playing_path)];
    metadata_db_album_sort_t album_sort;
    metadata_db_album_sort_t artist_albums_sort;
    int32_t library_revision;
    metadata_db_group_kind_t artist_albums_kind;
    char artist_albums_name[128];
} now_playing_lookup_request_t;

typedef struct {
    now_playing_lookup_request_t request;
    int artist_row;
    int album_row;
    int album_sorted_row;
    int album_artist_row;
    int all_songs_row;
    int recently_added_row;
    int artist_albums_row;
    metadata_db_album_sort_t effective_album_sort;
    int32_t library_revision;
} now_playing_lookup_result_t;

static pthread_mutex_t now_playing_lookup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t now_playing_lookup_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t now_playing_lookup_once = PTHREAD_ONCE_INIT;
static now_playing_lookup_request_t now_playing_lookup_request;
static now_playing_lookup_result_t now_playing_lookup_result;
static now_playing_lookup_result_t now_playing_lookup_applied_result;
static uint64_t now_playing_lookup_generation;
static uint64_t now_playing_lookup_lifetime;
static bool now_playing_lookup_pending;
static bool now_playing_lookup_result_ready;
static bool now_playing_lookup_result_valid;
static bool now_playing_lookup_applied_valid;
static bool now_playing_lookup_worker_started;
static bool library_ui_live;

static void request_now_playing_indicator_lookup(void);
static void apply_now_playing_indicator_result(const now_playing_lookup_result_t * result);

static void * now_playing_lookup_worker(void * unused) {
    (void) unused;
    for (;;) {
        now_playing_lookup_request_t request;
        pthread_mutex_lock(&now_playing_lookup_mutex);
        while (!now_playing_lookup_pending)
            pthread_cond_wait(&now_playing_lookup_cond, &now_playing_lookup_mutex);
        request = now_playing_lookup_request;
        now_playing_lookup_pending = false;
        pthread_mutex_unlock(&now_playing_lookup_mutex);

        now_playing_lookup_result_t result = { .request = request,
            .artist_row = -1, .album_row = -1, .album_sorted_row = -1,
            .album_artist_row = -1, .all_songs_row = -1,
            .recently_added_row = -1, .artist_albums_row = -1,
            .effective_album_sort = METADATA_DB_ALBUM_SORT_NAME };
        int32_t revision_before = metadata_db_get_generation();
        result.request.library_revision = revision_before;
        song_row_t song;
        if (request.path[0] && metadata_db_get_song_by_path(request.path, &song)) {
            int64_t value = 0;
            if (metadata_db_try_artist_row(song.tags.artist, &value) && value >= 0 && value <= INT_MAX)
                result.artist_row = (int) value;
            value = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                                                  song.tags.album, song.tags.album_artist);
            if (value >= 0 && value <= INT_MAX) {
                result.album_row = (int) value;
                result.effective_album_sort = metadata_db_album_sort_available(request.album_sort)
                    ? request.album_sort : METADATA_DB_ALBUM_SORT_NAME;
                result.album_sorted_row = metadata_db_album_canonical_to_sorted(
                    result.effective_album_sort, result.album_row);
            }
            value = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM_ARTIST,
                                                  song.tags.album_artist, NULL);
            if (value >= 0 && value <= INT_MAX) result.album_artist_row = (int) value;
            value = metadata_db_get_song_title_offset(request.path);
            if (value >= 0 && value <= INT_MAX) result.all_songs_row = (int) value;
            value = metadata_db_get_song_recency_offset(request.path);
            if (value >= 0 && value <= INT_MAX) result.recently_added_row = (int) value;
            if (request.artist_albums_name[0]) {
                value = metadata_db_get_album_for_group_sorted_offset(request.artist_albums_kind,
                                                                       request.artist_albums_name,
                                                                       request.artist_albums_sort,
                                                                       song.tags.album,
                                                                       song.tags.album_artist);
                if (value >= 0 && value < INT_MAX) result.artist_albums_row = (int) value + 1;
            }
        }
        result.library_revision = metadata_db_get_generation();
        bool revision_consistent = revision_before == result.library_revision;

        pthread_mutex_lock(&now_playing_lookup_mutex);
        if (request.generation == now_playing_lookup_generation &&
            request.lifetime == now_playing_lookup_lifetime) {
            now_playing_lookup_result = result;
            now_playing_lookup_result_ready = true;
            now_playing_lookup_result_valid = revision_consistent;
        }
        pthread_mutex_unlock(&now_playing_lookup_mutex);
        ui_wake_notify();
    }
    return NULL;
}

static void start_now_playing_lookup_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, now_playing_lookup_worker, NULL) == 0) {
        pthread_detach(thread);
        now_playing_lookup_worker_started = true;
    }
}

static void request_now_playing_indicator_lookup(void) {
    pthread_once(&now_playing_lookup_once, start_now_playing_lookup_worker);
    int32_t revision = -1;
    (void) metadata_db_try_get_generation(&revision);
    pthread_mutex_lock(&now_playing_lookup_mutex);
    now_playing_lookup_generation++;
    now_playing_lookup_request.generation = now_playing_lookup_generation;
    now_playing_lookup_request.lifetime = now_playing_lookup_lifetime;
    snprintf(now_playing_lookup_request.path, sizeof(now_playing_lookup_request.path), "%s", now_playing_path);
    now_playing_lookup_request.album_sort = requested_album_sort();
    now_playing_lookup_request.library_revision = revision;
    pthread_mutex_lock(&artist_albums_state_mutex);
    now_playing_lookup_request.artist_albums_kind = artist_albums_current_kind;
    now_playing_lookup_request.artist_albums_sort = requested_group_album_sort(artist_albums_current_kind);
    snprintf(now_playing_lookup_request.artist_albums_name,
             sizeof(now_playing_lookup_request.artist_albums_name), "%s", artist_albums_current_name);
    pthread_mutex_unlock(&artist_albums_state_mutex);
    now_playing_lookup_pending = now_playing_lookup_worker_started;
    now_playing_lookup_result_ready = false;
    now_playing_lookup_result_valid = false;
    pthread_cond_signal(&now_playing_lookup_cond);
    pthread_mutex_unlock(&now_playing_lookup_mutex);
}
static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int genres_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static void music_files_tile_cb(lv_event_t * e);
static void album_more_click_cb(int index);
static void album_row_long_press_cb(int index);
static void artist_collection_more_click_cb(int index);
static void artist_collection_long_press_cb(int index);
static void album_artist_collection_more_click_cb(int index);
static void album_artist_collection_long_press_cb(int index);
static void artist_album_more_click_cb(int index);
static void build_collection_menus(void);

/* Screen pointers owned by this module */
static lv_obj_t * music_screen = NULL;
static lv_obj_t * files_screen = NULL;
static lv_obj_t * files_search_list = NULL;
static file_browser_t * music_files_browser = NULL;
static lv_obj_t * files_title_label = NULL;
static lv_obj_t * files_options_btn = NULL;

static lv_obj_t * all_songs_screen = NULL;
static lv_obj_t * all_songs_list = NULL;

static lv_obj_t * recently_added_screen = NULL;
static lv_obj_t * recently_added_list = NULL;

static lv_obj_t * artists_screen = NULL;
static lv_obj_t * artists_list = NULL;

static lv_obj_t * albums_screen = NULL;
static lv_obj_t * albums_list = NULL;

static lv_obj_t * album_artist_screen = NULL;
static lv_obj_t * album_artist_list = NULL;

static lv_obj_t * genres_screen = NULL;
static lv_obj_t * genres_list = NULL;

static lv_obj_t * group_songs_screen = NULL;
static lv_obj_t * group_songs_list = NULL;
static lv_obj_t * group_songs_search_list = NULL;
static lv_obj_t * group_songs_title_label = NULL;
static char * group_songs_source_title;
static void update_group_songs_search_button(bool editing);
static void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail);
static bool group_song_has_cover(int index);
static void refresh_group_songs_search_now_playing(void);

static lv_obj_t * artist_albums_screen = NULL;
static lv_obj_t * artist_albums_list = NULL;
static lv_obj_t * artist_albums_title_label = NULL;

static lv_obj_t * cue_tracks_screen = NULL;
static lv_obj_t * cue_tracks_list = NULL;
static lv_obj_t * cue_tracks_title_label = NULL;

static lv_obj_t * collection_menu_popup = NULL;
static lv_obj_t * collection_menu_backdrop = NULL;
static lv_obj_t * album_collection_menu_popup = NULL;
static lv_obj_t * album_collection_menu_backdrop = NULL;
static bool collection_menu_is_album;
static char collection_menu_name[128];
static char collection_menu_album_artist[128];
static metadata_db_group_kind_t collection_menu_artist_kind;
static int collection_menu_song_count;
/* Credited artist the album menu was opened under, empty when it was not
 * opened from an Artists drill-down. */
static char collection_menu_artist[128];

/* Externs to player/queue and global state */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void on_file_selected(char ** new_playlist, int count, int selected_index);
extern const char * playlist_path_at(int index);
extern void enable_gesture_bubble_recursive(lv_obj_t * obj);
extern void register_static_snapshot(int index, lv_obj_t * screen);
extern void unregister_static_snapshot(lv_obj_t * screen);

static void file_browser_long_press(const char * path, bool is_dir) {
    if (!is_dir) {
        if (!path || strlen(path) >= GUI_PLAYLIST_TARGET_SONG_PATH_CAPACITY) {
            show_error_toast(TR("File path is too long"));
            return;
        }
        open_song_context_menu(path);
        return;
    }
    /* Match the old bounded menu path before comparing with the live browser. */
    char menu_path[PATH_MAX];
    snprintf(menu_path, sizeof(menu_path), "%s", path);
    bool show_page_jump = music_files_browser &&
        strcmp(menu_path, file_browser_current_dir(music_files_browser)) == 0 &&
        file_browser_can_jump_pages(music_files_browser);
    gui_folder_actions_open_menu(menu_path, show_page_jump);
}

static void file_folder_show_page_jump(const char * path) {
    if (!music_files_browser ||
        strcmp(path, file_browser_current_dir(music_files_browser)) != 0 ||
        !file_browser_can_jump_pages(music_files_browser)) return;
    file_browser_show_page_jump(music_files_browser);
}

static void files_current_folder_options_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !music_files_browser) return;
    file_browser_long_press(file_browser_current_dir(music_files_browser), true);
}

static void files_screen_delete_cb(lv_event_t * e) {
    if (lv_event_get_target(e) != files_screen) return;
    music_files_browser = NULL;
    files_title_label = NULL;
    files_options_btn = NULL;
}

static void files_header_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (search_close_if_active_for_screen(files_screen)) return;
    if (!file_browser_go_up_for_screen(files_screen)) nav_pop();
}

/* Shared entry point into the Add to Playlist picker -- the player's own
 * "..." menu (more_menu_add_to_playlist_cb() below) and the song long-press
 * context menu (song_context_menu_add_to_playlist_cb(), defined with the
 * rest of that popup further down) both reach this screen for whatever
 * song path they have, not necessarily the currently-playing one. */
void open_add_to_playlist_for(const char * path) {
    gui_playlists_open_for_song(path);
}

void on_cue_file_selected(const char * cue_path);

static lv_obj_t * build_files_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_add_event_cb(scr, files_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(scr, gui_folder_actions_menu_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, NULL);

    files_title_label = build_screen_header(scr, TR("Files"), files_header_back_cb, NULL, NULL);
    files_options_btn = build_top_right_icon_button(scr, asset_path("navigation/more.png"),
                                                    files_current_folder_options_cb);

    file_browser_set_sort_mode((file_browser_sort_mode_t) current_settings.file_sort_mode);
    const file_browser_options_t options = {
        .on_select = on_file_browser_selected,
        .on_cue_select = on_cue_file_selected,
        .on_index_select = on_file_browser_index_selected,
        .on_long_press = file_browser_long_press,
    };
    music_files_browser = file_browser_create(scr, MUSIC_ROOT_DIR, &options);

    finalize_screen_navigation(scr);
    return scr;
}

/* One song within a group_songs_screen listing (Artist/Album Artist's
 * albums, one album, Favorites, Most Played, a user .m3u playlist) -- an
 * owned path and display title resolved via targeted database query. */
/* group_song_entry_t defined in gui_library.h */

void free_group_song_entries(group_song_entry_t * entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].title);
    }
    free(entries);
}

bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count) {
    *out = NULL;
    if (count <= 0) return true;
    group_song_entry_t * copy = calloc((size_t) count, sizeof(*copy));
    if (!copy) return false;
    for (int i = 0; i < count; i++) {
        copy[i].path = strdup(entries[i].path ? entries[i].path : "");
        copy[i].title = strdup(entries[i].title ? entries[i].title : "");
        copy[i].disc_number = entries[i].disc_number;
        copy[i].show_disc_header = entries[i].show_disc_header;
        copy[i].song_id = entries[i].song_id;
        copy[i].artwork_key = entries[i].artwork_key;
        if (!copy[i].path || !copy[i].title) {
            free_group_song_entries(copy, count);
            return false;
        }
    }
    *out = copy;
    return true;
}


/* What a "Play All" row starts: the open group (album, artist, playlist),
 * Recently Added, or All Songs. */
enum { PLAY_ALL_SOURCE_GROUP, PLAY_ALL_SOURCE_RECENTLY_ADDED, PLAY_ALL_SOURCE_ALL_SONGS };

/* Paged All Songs -- see build_all_songs_screen()'s own comment. offset is
 * a position in the DB's own title-sorted order (metadata_db_get_songs_
 * filtered_page() with every filter NULL), the exact same order playlist_
 * lazy_sort_order/on_file_selected_lazy_all_songs() already assume, so a
 * row tapped here and the playback queue it starts always agree on what
 * comes next. Heap-allocated, not a stack array -- see remote_control.c's
 * own build_library_json() comment on why a COMPACT_LIST_PAGE_CACHE_SIZE-
 * sized array of song_row_t (~1.25KB each) has no business on a stack. */
static const char * song_quality_asset_for_path(const char * path) {
    const char * ext = strrchr(path ? path : "", '.');
    if (!ext) return "";
    if (!strcasecmp(ext, ".dsf") || !strcasecmp(ext, ".dff"))
        return "touch_list/quality_hr.png";
    if (!strcasecmp(ext, ".flac") || !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".rf64") ||
        !strcasecmp(ext, ".w64") || !strcasecmp(ext, ".aif") || !strcasecmp(ext, ".aifc") ||
        !strcasecmp(ext, ".aiff") || !strcasecmp(ext, ".ape") || !strcasecmp(ext, ".alac"))
        return "touch_list/quality_high.png";
    return "touch_list/quality_nomal.png";
}

static const char * library_codec_name(audio_codec_t codec) {
    const char * name = audio_codec_name(codec);
    return name[0] ? name : TR("Audio");
}

static void format_music_submenu_identity(const song_row_t * song, char * out, size_t out_size) {
    char title[128];
    metadata_db_song_display_title(song, title, sizeof(title));
    /* Building a list must not open every track's decoder. The displayed
     * page is enriched by gui_library_poll_track_probes() after it opens. */
    const char * ext = strrchr(song->path, '.');
    snprintf(out, out_size, "%s\n%s", title, ext && ext[1] ? ext + 1 : TR("Audio"));
}

static int effective_disc_number(const song_row_t * song) {
    return song && song->tags.disc_number > 0 ? song->tags.disc_number : 1;
}

static bool album_songs_are_multi_disc(const song_row_t * songs, int count) {
    if (!songs || count <= 1) return false;
    int first = effective_disc_number(&songs[0]);
    for (int i = 1; i < count; i++) {
        if (effective_disc_number(&songs[i]) != first) return true;
    }
    return false;
}

static int cmp_song_disc_track(const void * a, const void * b) {
    const song_row_t * sa = (const song_row_t *) a;
    const song_row_t * sb = (const song_row_t *) b;
    int da = effective_disc_number(sa), db = effective_disc_number(sb);
    if (da != db) return da < db ? -1 : 1;
    int ta = sa->tags.track_number, tb = sb->tags.track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(sa->path, sb->path);
}

static void set_album_disc_headers(const song_row_t * songs, int count,
                                   bool multi_disc, group_song_entry_t * entries) {
    int previous_disc = effective_disc_number(&songs[0]);
    for (int i = 0; i < count; i++) {
        entries[i].disc_number = effective_disc_number(&songs[i]);
        entries[i].show_disc_header = multi_disc && (i == 0 || entries[i].disc_number != previous_disc);
        previous_disc = entries[i].disc_number;
    }
}

void gui_library_format_song_identity(const song_row_t * row,
                                      char * title, size_t title_size,
                                      char * subtitle, size_t subtitle_size) {
    char artist[121], album[121];
    metadata_db_song_display_title(row, title, title_size);
    utf8_truncate_safe(artist, row->tags.artist[0] ? row->tags.artist : TR("Unknown artist"), sizeof(artist));
    utf8_truncate_safe(album, row->tags.album[0] ? row->tags.album : TR("Unknown album"), sizeof(album));
    snprintf(subtitle, subtitle_size, "%s · %s", artist, album);
}

static void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row) {
    gui_library_format_song_identity(row, out->label, sizeof(out->label),
                                     out->subtitle, sizeof(out->subtitle));
    out->identity = row->id;
    out->artwork_key = song_album_thumbnail_key(row);
    snprintf(out->trailing_asset, sizeof(out->trailing_asset), "%s", song_quality_asset_for_path(row->path));
}

/* Row 0 is "Play All" (same row as Recently Added), so song i is row i + 1.
 * Search results replace the whole list and have no such row. */
static void fill_play_all_row(compact_list_page_row_t * out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->label, sizeof(out->label), "%s", TR("Play All"));
    out->is_action = true;
    int64_t tracks = metadata_db_get_song_count();
    snprintf(out->subtitle, sizeof(out->subtitle), TR_N("%lld track", "%lld tracks", (long) tracks),
             (long long)tracks);
}

static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    int prefix = 0;
    if (offset == 0 && count > 0) {
        fill_play_all_row(&out_rows[0]);
        prefix = 1;
        count--;
        offset++;
    }
    if (count <= 0) return prefix;
    int32_t generation_before = 0, generation_after = 0;
    bool cacheable = metadata_db_try_get_generation(&generation_before);
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, offset - 1, count, rows) : 0;
    if (cacheable)
        cacheable = metadata_db_try_get_generation(&generation_after) && generation_before == generation_after;
    for (int i = 0; i < n; i++) {
        fill_song_page_visual(&out_rows[prefix + i], &rows[i]);
        if (cacheable)
            library_selection_snapshot_store(&rows[i], SELECTION_SNAPSHOT_ALL_SONGS,
                                     offset - 1 + i, generation_before);
    }
    free(rows);
    return n + prefix;
}

/* A tapped All Songs row as a position in the title order, or -1 for the
 * Play All row. */
static int all_songs_song_index(int display_index) {
    if (library_search_showing_results(SEARCH_BINDING_ALL_SONGS))
        return search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    return display_index - 1;
}

/* Resolves one All-Songs display position to a real path -- for the
 * long-press context menu, which needs a path directly (unlike the click
 * handler, which goes through play_track_at_from() -> playlist_path_at()'s
 * own resolution instead). false if display_index is out of range (e.g.
 * raced a rescan shrinking the library between the tap and this lookup --
 * same stale-reference tolerance as playlist_path_at()'s own comment). */
static bool all_songs_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void playlist_start_options(int source);

static void all_songs_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = all_songs_song_index(display_index);
    if (display_index < 0) {
        gui_player_seed_selection_song(NULL, NULL);
        playlist_start_options(PLAY_ALL_SOURCE_ALL_SONGS);
        return;
    }
    /* on_file_selected_lazy_all_songs() builds the queue lazily, identity-
     * mapped into the DB's own title-sorted order (the same display order
     * this screen's paged provider uses) instead of eagerly strdup'ing
     * every one of the library's paths on every tap -- see its own
     * comment. Tap-to-play from All Songs is DB-driven end to end now. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, display_index, 0, NULL);
    set_player_source_all_songs(display_index);
    on_file_selected_lazy_all_songs(display_index);
}

static void all_songs_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = all_songs_song_index(display_index);
    if (display_index < 0) return;
    char path[600];
    if (all_songs_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

/* Builds the "All Songs" screen with a paged provider
 * (compact_list_set_paged_provider()) so items are loaded incrementally
 * on scroll rather than materializing the full library in memory. */
static lv_obj_t * build_all_songs_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("All Songs"), generic_back_cb, NULL, 0, all_songs_row_click_cb,
                                                all_songs_row_long_press_cb, &all_songs_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(all_songs_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(all_songs_list, all_songs_fetch_page, NULL,
                                     (int) metadata_db_get_song_count() + 1);
    library_thumbnails_bind_list(scr, all_songs_list, LIBRARY_THUMB_LIST_ALL_SONGS);
    finalize_screen_navigation(scr);
    return scr;
}

/* Recently Added -- same paged compact_list architecture as All Songs above
 * (see build_all_songs_screen()'s own comment on why: a bounded page at a
 * time regardless of library size, not the show_group_songs()/group_song_
 * entry_t path Favorites/Most Played use, which loads its whole, small,
 * deliberately-capped result set into RAM up front -- Recently Added has no
 * such cap, it's every song in the library just reordered by first_seen, so
 * it needs the same scale guarantee All Songs has). Only difference from
 * All Songs is the ORDER BY (first_seen DESC, rowid DESC via metadata_db_
 * get_songs_page_by_recency() instead of title via metadata_db_get_songs_
 * filtered_page()) -- no search/A-Z index wired up here, unlike All Songs,
 * since neither makes as much sense against a recency-ordered list. */
static void format_song_identity(const song_row_t * row, char * out, size_t size) {
    char title[128], subtitle[256];
    gui_library_format_song_identity(row, title, sizeof(title), subtitle, sizeof(subtitle));
    snprintf(out, size, "%s\n%s", title, subtitle);
}

static int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    int prefix = 0;
    if (offset == 0 && count > 0) {
        fill_play_all_row(&out_rows[0]);
        prefix = 1; count--; offset++;
    }
    if (count <= 0) return prefix;
    int32_t generation_before = 0, generation_after = 0;
    bool cacheable = metadata_db_try_get_generation(&generation_before);
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_page_by_recency(offset - 1, count, rows) : 0;
    if (cacheable)
        cacheable = metadata_db_try_get_generation(&generation_after) && generation_before == generation_after;
    for (int i = 0; i < n; i++) {
        fill_song_page_visual(&out_rows[prefix + i], &rows[i]);
        if (cacheable)
            library_selection_snapshot_store(&rows[i], SELECTION_SNAPSHOT_RECENTLY_ADDED,
                                     offset - 1 + i, generation_before);
    }
    free(rows);
    return n + prefix;
}

/* Same role as all_songs_resolve_path_at() above, against the recency order. */
static bool recently_added_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_page_by_recency(display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

/* A tapped Recently Added row as a position in the recency order, or -1 for
 * the Play All row. */
static int recently_added_song_index(int display_index) {
    if (library_search_showing_results(SEARCH_BINDING_RECENTLY_ADDED))
        return search_remap_index(SEARCH_BINDING_RECENTLY_ADDED, display_index);
    return display_index - 1;
}

static void recently_added_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = recently_added_song_index(display_index);
    if (display_index < 0) {
        gui_player_seed_selection_song(NULL, NULL);
        playlist_start_options(PLAY_ALL_SOURCE_RECENTLY_ADDED);
        return;
    }
    /* on_file_selected_lazy_recently_added() builds the queue lazily,
     * identity-mapped into the DB's own first_seen-DESC order (the same
     * display order this screen's paged provider uses) -- see its own
     * comment. Tap-to-play here is DB-driven end to end, same as All Songs. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_RECENTLY_ADDED, display_index, 0, NULL);
    set_player_source_recently_added(display_index);
    on_file_selected_lazy_recently_added(display_index);
}

static void recently_added_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = recently_added_song_index(display_index);
    if (display_index < 0) return;
    char path[600];
    if (recently_added_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

static lv_obj_t * build_recently_added_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Recently Added"), generic_back_cb, NULL, 0, recently_added_row_click_cb,
                                                recently_added_row_long_press_cb, &recently_added_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(recently_added_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
                                     (int) metadata_db_get_song_count() + 1);
    library_thumbnails_bind_list(scr, recently_added_list, LIBRARY_THUMB_LIST_RECENTLY_ADDED);
    finalize_screen_navigation(scr);
    return scr;
}

static void all_songs_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(all_songs_screen);
}

/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
static lv_obj_t * group_songs_edit_btn;
static group_song_entry_t * group_songs_entries; /* owned -- see set_group_songs_entries() */
static int group_songs_count;

/* Frees the previous group_songs_entries (if any) and replaces it with an
 * owned copy of entries[0..count) -- every show_group_songs_editable() call
 * and every "Removed from playlist" refresh goes through this single choke
 * point so the ownership rule (group_songs_entries is always either NULL or
 * a malloc'd copy this screen owns outright) never has to be re-derived at
 * each call site. Caller's own entries[] can be freed immediately after. */
static void set_group_songs_entries(const group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = NULL;
    group_songs_count = copy_group_song_entries(&group_songs_entries, entries, count) ? count : 0;
}

/* Sibling to set_group_songs_entries() that takes ownership of an already
 * allocated entries[] array rather than copying it, avoiding duplicate
 * allocations for large song lists. The caller transfers ownership and
 * must not free `entries` or its elements afterward. */
static void set_group_songs_entries_owned(group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = entries;
    group_songs_count = count;
}

/* A pathological album can contain tens of thousands of tracks. Building one
 * LVGL object per track caused a large post-scan RSS spike and could make ADB
 * unresponsive. Keep the backing group intact for playback, but materialize
 * only one bounded page of rows at a time. */
#define GROUP_SONGS_PAGE_SIZE 200
static int group_songs_page_start;
static bool * group_songs_selected;
static int group_songs_selected_count;
static lv_obj_t * group_songs_selection_checks[GROUP_SONGS_PAGE_SIZE];
static lv_obj_t * group_songs_reorder_handles[GROUP_SONGS_PAGE_SIZE];
static lv_obj_t * group_songs_delete_btn;
static lv_obj_t * group_songs_select_all_row;
static lv_obj_t * group_songs_select_all_checkbox;
static lv_obj_t * group_songs_select_all_label;
static lv_obj_t * group_songs_select_all_done_btn;
static int group_songs_populated_page_start = -1;
static bool group_songs_anchor_pending;
static bool group_songs_rebuild_on_load;
static bool group_songs_rows_rebuilding;
static int group_songs_anchor_index;
static int32_t group_songs_anchor_offset;

/* One bounded job at a time. The worker owns path copies and never sees
 * LVGL objects or group_songs_entries. Page generations invalidate late
 * results without waiting for the current decoder to finish. */
static track_probe_job_t * group_probe_job;
static uint64_t group_probe_generation = 1;
static uint64_t group_probe_job_generation;
static uint64_t group_probe_requested_generation;
static int group_probe_page_start;
static bool group_probe_storage_available = true;
static bool group_probe_retry_pending;
static uint32_t group_probe_retry_tick;

static void cancel_group_song_probes(void) {
    ++group_probe_generation;
    group_probe_retry_pending = false;
    if (group_probe_job) track_probe_job_cancel(group_probe_job);
}

/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
static lv_obj_t * group_songs_now_playing_bar;
static lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
static gui_list_reorder_state_t group_songs_reorder_state = { .drag_id = -1 };
static lv_obj_t * group_songs_drag_original_children[GROUP_SONGS_PAGE_SIZE * 2 + 8];
static int group_songs_drag_original_child_count;
static int group_songs_drag_source_id = -1;
static lv_obj_t * group_songs_thumbnail_images[GROUP_SONGS_PAGE_SIZE];
static bool group_songs_music_submenu;

/* Forward-declared here (defined after on_file_selected()) because
 * set_player_source_group_songs() needs group_songs_entries/count/title_label
 * already in scope, which the following definitions did not have. */
/* set_player_source_group_songs_direct defined in gui.c */

static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          group_songs_source_title ? group_songs_source_title : "", pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album && group_songs_source_title ? group_songs_source_title : "");
}

/* Non-NULL only when the currently shown group is a user-created .m3u
 * playlist (set by show_m3u_playlist() via show_group_songs_editable()),
 * enabling the edit/remove-song UI. NULL for Artists/Albums/Favorites/
 * Most Played, which do not back onto a rewritable file. Borrowed pointer
 * from playlists_m3u_paths[]; valid for the lifetime of the screen.
 * group_songs_edit_mode is reset to false on every fresh entry. */
static const char * group_songs_edit_m3u_path = NULL;
static char group_songs_owned_m3u_path[PATH_MAX];
static void reload_edited_playlist(void);
static struct stat group_songs_file_stat;
static bool group_songs_file_stat_valid;
static bool group_playlist_unchanged(void) {
    struct stat st;
    return group_songs_edit_m3u_path && group_songs_file_stat_valid &&
        stat(group_songs_edit_m3u_path, &st) == 0 && st.st_ino == group_songs_file_stat.st_ino &&
        st.st_dev == group_songs_file_stat.st_dev && st.st_size == group_songs_file_stat.st_size &&
        st.st_mtim.tv_sec == group_songs_file_stat.st_mtim.tv_sec &&
        st.st_mtim.tv_nsec == group_songs_file_stat.st_mtim.tv_nsec;
}
static bool group_songs_edit_mode = false;

static int32_t group_songs_reorder_handle_width(void) {
    int32_t width = BOARD_SCALE_PX(44);
    return width < 48 ? 48 : width;
}

static void group_songs_delete_selected_cb(lv_event_t * e);
static void group_song_selection_row_cb(lv_event_t * e);
static void group_songs_select_all_row_cb(lv_event_t * e);
static void group_songs_edit_btn_cb(lv_event_t * e);
static void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count);
static void populate_group_songs_rows(void);
static void group_songs_reorder_handle_cb(lv_event_t * e);
static void group_songs_reorder_prepare(gui_list_reorder_state_t * state, int id);
static void group_songs_reorder_commit(gui_list_reorder_state_t * state);
static void group_songs_reorder_restore(gui_list_reorder_state_t * state);
static bool group_songs_playlist_check(void * context, char * const * paths, int count);

static void group_songs_restore_drag_child_order(void) {
    if (!group_songs_list) return;
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i)
        gui_list_reorder_row_stop_translate(group_songs_visible_rows[i]);
    for (int i = 0; i < group_songs_drag_original_child_count; ++i) {
        lv_obj_t * child = group_songs_drag_original_children[i];
        if (child && lv_obj_get_parent(child) == group_songs_list)
            lv_obj_move_to_index(child, i);
    }
    group_songs_drag_original_child_count = 0;
    group_songs_drag_source_id = -1;
}

static void group_songs_unregister_reorder_handles(void) {
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i) {
        if (group_songs_reorder_handles[i]) {
            unregister_swipe_dead_zone(group_songs_reorder_handles[i]);
            group_songs_reorder_handles[i] = NULL;
        }
    }
}

static void group_songs_reorder_prepare(gui_list_reorder_state_t * state, int id) {
    if (!state) return;
    state->count = 0;
    group_songs_drag_source_id = -1;
    group_songs_drag_original_child_count = 0;
    if (!group_songs_edit_mode || !group_songs_edit_m3u_path ||
        !group_playlist_unchanged() || id < 0 || id >= GROUP_SONGS_PAGE_SIZE) {
        show_info_toast(TR("Playlist changed. Try again."));
        return;
    }
    int visible_count = group_songs_count - group_songs_page_start;
    if (visible_count > GROUP_SONGS_PAGE_SIZE) visible_count = GROUP_SONGS_PAGE_SIZE;
    if (visible_count < 2 || id >= visible_count) return;
    uint32_t child_count = lv_obj_get_child_count(group_songs_list);
    if (child_count > sizeof(group_songs_drag_original_children) /
                      sizeof(group_songs_drag_original_children[0])) {
        show_error_toast(TR("Cannot reorder playlist"));
        return;
    }
    for (uint32_t i = 0; i < child_count; ++i)
        group_songs_drag_original_children[i] = lv_obj_get_child(group_songs_list, i);
    group_songs_drag_original_child_count = (int) child_count;
    group_songs_drag_source_id = id;
    for (int i = 0; i < visible_count; ++i) {
        if (!group_songs_visible_rows[i]) {
            group_songs_drag_original_child_count = 0;
            group_songs_drag_source_id = -1;
            return;
        }
        state->ids[i] = i;
    }
    state->count = visible_count;
}

static void group_songs_reorder_restore(gui_list_reorder_state_t * state) {
    (void) state;
    group_songs_restore_drag_child_order();
    group_songs_drag_source_id = -1;
}

static void group_songs_reorder_commit(gui_list_reorder_state_t * state) {
    int source_id = group_songs_drag_source_id;
    int destination_id = -1;
    if (state && source_id >= 0) {
        for (int i = 0; i < state->drag_count; ++i) {
            if (state->ids[i] == source_id) { destination_id = i; break; }
        }
    }
    if (source_id < 0 || destination_id < 0 || source_id == destination_id) {
        group_songs_restore_drag_child_order();
        group_songs_drag_source_id = -1;
        return;
    }
    int from = group_songs_page_start + source_id;
    int to = group_songs_page_start + destination_id;
    group_songs_restore_drag_child_order();
    playlist_edit_status_t status = playlist_files_edit_entry_checked(
        group_songs_edit_m3u_path, from, to, group_songs_playlist_check, NULL);
    group_songs_drag_source_id = -1;
    if (status == PLAYLIST_EDIT_OK) {
        reload_edited_playlist();
    } else if (status == PLAYLIST_EDIT_REFUSED) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Try again."));
    } else {
        populate_group_songs_rows();
        show_error_toast(TR("Cannot reorder playlist"));
    }
}

static void group_songs_reorder_handle_cb(lv_event_t * e) {
    int id = (int) (intptr_t) lv_event_get_user_data(e);
    gui_list_reorder_event(&group_songs_reorder_state, id, e);
}

static void group_songs_clear_selection(void) {
    free(group_songs_selected);
    group_songs_selected = NULL;
    group_songs_selected_count = 0;
}

static bool group_songs_reset_selection(void) {
    group_songs_clear_selection();
    if (group_songs_edit_mode && group_songs_count > 0) {
        group_songs_selected = calloc((size_t) group_songs_count, sizeof(*group_songs_selected));
        return group_songs_selected != NULL;
    }
    return true;
}

static void group_songs_update_edit_header(void) {
    if (!group_songs_edit_btn) return;
    lv_label_set_text(group_songs_edit_btn, TR("Edit"));
    if (group_songs_edit_mode) lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    if (group_songs_delete_btn) {
        if (group_songs_edit_mode) lv_obj_clear_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(group_songs_delete_btn,
            group_songs_selected_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_image_opa(group_songs_delete_btn,
            group_songs_selected_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    if (group_songs_select_all_checkbox) {
        if (group_songs_count > 0 && group_songs_selected_count == group_songs_count)
            lv_obj_add_state(group_songs_select_all_checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(group_songs_select_all_checkbox, LV_STATE_CHECKED);
        if (group_songs_count == 0) lv_obj_add_state(group_songs_select_all_checkbox, LV_STATE_DISABLED);
        else lv_obj_remove_state(group_songs_select_all_checkbox, LV_STATE_DISABLED);
    }
    if (group_songs_select_all_label) {
        char text[64];
        if (group_songs_selected_count > 0)
            snprintf(text, sizeof(text), TR("Select all (%d)"), group_songs_selected_count);
        else snprintf(text, sizeof(text), "%s", TR("Select all"));
        lv_label_set_text(group_songs_select_all_label, text);
    }
    if (group_songs_select_all_row) {
        if (group_songs_count == 0) lv_obj_clear_flag(group_songs_select_all_row, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_add_flag(group_songs_select_all_row, LV_OBJ_FLAG_CLICKABLE);
    }
    if (group_songs_edit_m3u_path && group_songs_title_label) {
        lv_obj_t * action = group_songs_edit_mode ? group_songs_delete_btn : group_songs_edit_btn;
        if (action) reserve_title_width_before(group_songs_title_label, action);
    }
}

static void group_songs_prev_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_page_start <= 0) return;
    group_songs_page_start -= GROUP_SONGS_PAGE_SIZE;
    if (group_songs_page_start < 0) group_songs_page_start = 0;
    populate_group_songs_rows();
}

static void group_songs_next_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int next = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (next >= group_songs_count) return;
    group_songs_page_start = next;
    populate_group_songs_rows();
}

static lv_obj_t * add_group_songs_page_row(const char * text, lv_event_cb_t cb) {
    lv_obj_t * row = lv_label_create(group_songs_list);
    lv_obj_add_style(row, &list_row_style, 0);
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    row_label_enable_marquee(row);
    int32_t row_height = ui_list_row_height();
    lv_obj_set_style_height(row, row_height, LV_PART_MAIN);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_set_style_pad_top(row,
        (row_height - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(row, text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

static lv_obj_t * add_group_songs_select_all_row(void) {
    lv_obj_t * row = add_pill_row_base(group_songs_list, TR("Select all"));
    group_songs_select_all_row = row;
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, group_songs_select_all_row_cb, LV_EVENT_CLICKED, NULL);

    group_songs_select_all_checkbox = lv_checkbox_create(row);
    lv_checkbox_set_text(group_songs_select_all_checkbox, "");
    lv_obj_set_size(group_songs_select_all_checkbox, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
    lv_obj_align(group_songs_select_all_checkbox, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18), 0);
    lv_obj_set_style_border_color(group_songs_select_all_checkbox, accent_lv_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(group_songs_select_all_checkbox, accent_lv_color(),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_image_src(group_songs_select_all_checkbox, LV_SYMBOL_OK,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(group_songs_select_all_checkbox,
        lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(),
        LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_flag(group_songs_select_all_checkbox,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE);

    group_songs_select_all_label = lv_obj_get_child(row, 0);
    if (group_songs_select_all_label) {
        lv_obj_update_layout(row);
        int32_t text_width = lv_obj_get_width(row) - BOARD_SCALE_PX(100 + 18 + 28 + 8);
        if (text_width < BOARD_SCALE_PX(40)) text_width = BOARD_SCALE_PX(40);
        lv_obj_set_width(group_songs_select_all_label, text_width);
        lv_obj_align(group_songs_select_all_label, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18 + 28 + 8), 0);
        lv_obj_set_style_text_align(group_songs_select_all_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    group_songs_select_all_done_btn = lv_button_create(row);
    lv_obj_remove_style_all(group_songs_select_all_done_btn);
    lv_obj_set_size(group_songs_select_all_done_btn, BOARD_SCALE_PX(72), BOARD_SCALE_PX(48));
    lv_obj_align(group_songs_select_all_done_btn, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(12), 0);
    lv_obj_add_style(group_songs_select_all_done_btn, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(group_songs_select_all_done_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group_songs_select_all_done_btn, 0, 0);
    lv_obj_remove_flag(group_songs_select_all_done_btn,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(group_songs_select_all_done_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * done_label = lv_label_create(group_songs_select_all_done_btn);
    lv_label_set_text(done_label, TR("Done"));
    lv_obj_add_style(done_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(done_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(done_label);
    lv_obj_remove_flag(done_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already triggered the context menu on a group song row. */
static bool group_song_row_long_press_fired = false;

static void group_play_at(int pos) {
    if (library_rescan_active) return;
    if (pos < 0 || pos >= group_songs_count) return;
    gui_player_seed_selection_song(NULL, NULL);
    if (group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        reload_edited_playlist(); show_info_toast(TR("Playlist changed. Select a song again.")); return;
    }
    /* Allocate and copy playlist paths using calloc to handle potential
     * allocation failures cleanly. */
    char ** playlist_copy = calloc((size_t) group_songs_count, sizeof(char *));
    if (!playlist_copy) return;
    bool ok = true;
    for (int i = 0; i < group_songs_count; i++) {
        playlist_copy[i] = strdup(group_songs_entries[i].path);
        if (!playlist_copy[i]) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (int i = 0; i < group_songs_count; i++) free(playlist_copy[i]);
        free(playlist_copy);
        return;
    }
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1,
                           group_songs_entries[pos].song_id,
                           group_songs_entries[pos].path);
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) { group_song_row_long_press_fired = false; return; }
    group_play_at((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * playlist_start_popup, * playlist_start_backdrop;
static int playlist_start_source;

/* Starting a collection sequentially should keep an active repeat mode. */
static play_mode_t collection_play_mode(play_mode_t requested_mode) {
    if (requested_mode == PLAY_MODE_SHUFFLE || requested_mode == PLAY_MODE_SHUFFLE_ONCE)
        return requested_mode;
    if (requested_mode == PLAY_MODE_SEQUENTIAL &&
        (current_settings.play_mode == PLAY_MODE_REPEAT_ALL ||
         current_settings.play_mode == PLAY_MODE_REPEAT_ONE)) {
        return (play_mode_t) current_settings.play_mode;
    }
    return requested_mode;
}

static void playlist_start_cancel(lv_event_t * e) {
    (void) e;
    lv_obj_add_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
}
static void playlist_start_selected(lv_event_t * e, bool shuffle) {
    if (library_rescan_active) return;
    playlist_start_cancel(e);
    int count = playlist_start_source == PLAY_ALL_SOURCE_GROUP ? group_songs_count
                                                               : (int) metadata_db_get_song_count();
    if (count <= 0) { show_info_toast(TR("Playlist is empty")); return; }
    int index = 0;
    if (shuffle) {
        unsigned int value = (unsigned int) time(NULL) ^ lv_tick_get();
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, &value, sizeof(value)); (void) n; close(fd); }
        index = (int) (value % (unsigned int) count);
    }
    gui_player_set_play_mode(collection_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL));
    if (playlist_start_source == PLAY_ALL_SOURCE_RECENTLY_ADDED) {
        set_player_source_recently_added(index);
        on_file_selected_lazy_recently_added(index);
    } else if (playlist_start_source == PLAY_ALL_SOURCE_ALL_SONGS) {
        set_player_source_all_songs(index);
        on_file_selected_lazy_all_songs(index);
    } else group_play_at(index);
}
static void playlist_start_sequential(lv_event_t * e) { playlist_start_selected(e, false); }
static void playlist_start_shuffle(lv_event_t * e) { playlist_start_selected(e, true); }
static void playlist_start_options(int source) {
    playlist_start_source = source;
    if (!playlist_start_popup) {
        const menu_popup_row_t rows[] = {
            { TR("Start sequentially"), playlist_start_sequential, false },
            { TR("Shuffle from a random song"), playlist_start_shuffle, false },
            { TR("Cancel"), playlist_start_cancel, false, true },
        };
        playlist_start_popup = build_menu_popup(rows, 3, playlist_start_cancel, &playlist_start_backdrop);
    }
    lv_obj_remove_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_start_backdrop);
    lv_obj_move_foreground(playlist_start_popup);
}
static void group_start_options_cb(lv_event_t * e) {
    (void) e; playlist_start_options(PLAY_ALL_SOURCE_GROUP);
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (library_rescan_active) return;
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
}

/* Music's Artist/Album drill-down uses a deliberate two-line identity.
 * Playlist and Queue rows intentionally retain their established shared
 * builder geometry. */
static void layout_music_submenu_row_text(lv_obj_t * row) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * primary = lv_obj_get_child(row, 0);
    lv_obj_t * secondary = lv_obj_get_child(row, 1);
    int32_t height = lv_obj_get_style_height(row, LV_PART_MAIN);
    int32_t title_height = row_label_bounded_height(lv_obj_get_style_text_font(primary, 0));
    bool has_detail = !lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    int32_t detail_height = has_detail
        ? row_label_bounded_height(lv_obj_get_style_text_font(secondary, 0)) : 0;
    int32_t gap = has_detail ? BOARD_SCALE_PX(4) : 0;
    int32_t top = (height - title_height - gap - detail_height) / 2;
    if (top < 0) top = 0;
    lv_obj_set_y(primary, top);
    if (has_detail) lv_obj_set_y(secondary, top + title_height + gap);
}

void gui_library_poll_track_probes(void) {
    bool visible = group_songs_screen && lv_screen_active() == group_songs_screen &&
                   group_songs_music_submenu && group_probe_storage_available && backlight_screen_is_on();
    if (!visible && group_probe_job && group_probe_job_generation == group_probe_generation)
        cancel_group_song_probes();

    if (group_probe_job) {
        bool current = visible && group_probe_job_generation == group_probe_generation;
        /* Take the completion snapshot before draining results, so a final
         * result published just after next() returns false is not lost. */
        bool done = track_probe_job_done(group_probe_job);
        bool drained = false;
        if (current) {
            /* Limit label work per tick; never rebuild the list or reset
             * its scroll position when a probe completes. */
            for (int budget = 0; budget < 4; ++budget) {
                size_t index;
                audio_current_format_info_t info;
                if (!track_probe_job_next(group_probe_job, &index, &info)) {
                    drained = true;
                    break;
                }
                int pos = group_probe_page_start + (int)index;
                if (!info.valid || !(info.duration_seconds > 0.0 &&
                    info.duration_seconds <= UINT_MAX - 1.0) ||
                    index >= GROUP_SONGS_PAGE_SIZE || pos >= group_songs_count) continue;
                char detail[64];
                unsigned int seconds = (unsigned int)(info.duration_seconds + 0.5);
                snprintf(detail, sizeof(detail), "%u:%02u · %s", seconds / 60, seconds % 60,
                         library_codec_name(info.codec));
                const char * old = group_songs_entries[pos].title;
                const char * newline = strchr(old, '\n');
                size_t title_len = newline ? (size_t)(newline - old) : strlen(old);
                char * title = malloc(title_len + strlen(detail) + 2);
                if (!title) continue;
                memcpy(title, old, title_len);
                title[title_len] = '\n';
                strcpy(title + title_len + 1, detail);
                free(group_songs_entries[pos].title);
                group_songs_entries[pos].title = title;
                lv_obj_t * row = group_songs_visible_rows[index];
                if (row) {
                    lv_obj_t * secondary = lv_obj_get_child(row, 1);
                    lv_label_set_text(secondary, detail);
                    lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
                    layout_music_submenu_row_text(row);
                }
            }
        }
        if (done && (!current || drained)) {
            track_probe_job_destroy(group_probe_job);
            group_probe_job = NULL;
        }
    }

    if (!visible || group_probe_job || group_probe_requested_generation == group_probe_generation)
        return;
    if (group_probe_retry_pending && lv_tick_elaps(group_probe_retry_tick) < 1000) return;
    int count = group_songs_count - group_songs_page_start;
    if (count > GROUP_SONGS_PAGE_SIZE) count = GROUP_SONGS_PAGE_SIZE;
    if (count <= 0) return;
    const char * paths[GROUP_SONGS_PAGE_SIZE];
    for (int i = 0; i < count; ++i) paths[i] = group_songs_entries[group_songs_page_start + i].path;
    group_probe_job = track_probe_job_start(paths, (size_t)count);
    if (!group_probe_job) {
        group_probe_retry_pending = true;
        group_probe_retry_tick = lv_tick_get();
        return;
    }
    group_probe_retry_pending = false;
    group_probe_requested_generation = group_probe_generation;
    group_probe_job_generation = group_probe_generation;
    group_probe_page_start = group_songs_page_start;
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. The visible-row
 * table lets the marker follow LVGL's final flex layout, including runtime
 * font/touch sizing and any action or paging rows before the song. */
static void refresh_group_songs_now_playing_indicator(void) {
    /* Search results can include songs outside the underlying list's page. */
    refresh_group_songs_search_now_playing();
    if (!group_songs_now_playing_bar) return;

    int match = -1;
    if (now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) { match = i; break; }
        }
    }

    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    if (match < group_songs_page_start || match >= page_end) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t * row = group_songs_visible_rows[match - group_songs_page_start];
    if (!row) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Runtime font/touch sizing can make the rendered row taller than its
     * compile-time minimum. Follow the real laid-out object rather than
     * reconstructing its bounds from fixed height and gap constants. */
    lv_obj_update_layout(group_songs_list);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, lv_obj_get_y(row));
    lv_obj_set_height(group_songs_now_playing_bar, lv_obj_get_height(row));
}

/* Rebuilds group_songs_list's rows from whatever group_songs_entries/count
 * currently hold, in either of two shapes: a plain tappable-to-play label
 * (list_row_style, same as every other group -- Artists/Albums/Favorites/
 * Most Played always use this one) when not actively editing a playlist, or
 * rows with selection and reorder controls (no play-on-tap) when
 * group_songs_edit_mode is on for an
 * editable .m3u playlist. Split out from show_group_songs_editable() so the
 * selection, reorder, and Edit/Done actions can redraw in place without
 * re-deriving the group or nav_push()ing a second copy of this screen. */
static void populate_group_songs_rows(void) {
    group_songs_rows_rebuilding = true;
    int32_t restore_scroll_y = 0;
    bool restore_scroll = group_songs_list && group_songs_populated_page_start == group_songs_page_start;
    if (restore_scroll) restore_scroll_y = lv_obj_get_scroll_y(group_songs_list);
    if (group_songs_reorder_state.active)
        gui_list_reorder_finish(&group_songs_reorder_state, false);
    group_songs_unregister_reorder_handles();
    group_songs_reorder_state.list = group_songs_list;
    group_songs_reorder_state.rows = group_songs_visible_rows;
    group_songs_reorder_state.row_capacity = GROUP_SONGS_PAGE_SIZE;
    group_songs_reorder_state.prepare = group_songs_reorder_prepare;
    group_songs_reorder_state.commit = group_songs_reorder_commit;
    group_songs_reorder_state.restore = group_songs_reorder_restore;
    library_thumbnails_group_rows_rebuilt(group_songs_list);
    cancel_group_song_probes();
    memset(group_songs_reorder_handles, 0, sizeof(group_songs_reorder_handles));
    memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
    group_songs_select_all_row = NULL;
    group_songs_select_all_checkbox = NULL;
    group_songs_select_all_label = NULL;
    group_songs_select_all_done_btn = NULL;
    /* Cleaning a scrolled list can synchronously emit SCROLL_END. Detach
     * borrowed row pointers before deletion, and suppress decoration until
     * the replacement rows are complete. */
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
    lv_obj_clean(group_songs_list);

    bool editable = group_songs_edit_m3u_path != NULL;
    bool editing = editable && group_songs_edit_mode;
    if (editable) {
        if (editing) lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        group_songs_update_edit_header();
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        if (group_songs_delete_btn) lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }

    update_group_songs_search_button(editing || group_songs_loading);
    if (!editing && !group_songs_loading) {
        lv_obj_t * start = add_group_songs_page_row(TR("Play All"), group_start_options_cb);
        lv_obj_add_style(start, &style_theme_card_bg, 0);
        lv_obj_set_style_text_align(start, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_left(start, BOARD_SCALE_PX(100), 0);
        char count_text[48];
        snprintf(count_text, sizeof(count_text), TR_N("%d track", "%d tracks", group_songs_count),
                 group_songs_count);
        lv_obj_t * subtitle = row_label_create_subtitle(start);
        row_label_set_identity(start, subtitle, TR("Play All"), count_text, ui_music_row_height());
        decorate_play_all_row(start);
    }
    if (editing) {
        add_group_songs_select_all_row();
        group_songs_update_edit_header();
    }

    if (group_songs_count == 0) {
        if (group_songs_loading)
            build_list_message(group_songs_list, TR("Loading tracks…"), NULL);
        else if (group_songs_edit_m3u_path)
            build_list_message(group_songs_list, TR("Playlist is empty"), TR("Add songs from a song menu."));
        else
            build_list_message(group_songs_list, TR("No tracks found"), TR("Refresh the music database to update this list."));
    }

    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    if (group_songs_page_start != group_songs_populated_page_start) restore_scroll = false;
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;

    if (group_songs_page_start > 0) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), TR("Previous  •  %d–%d of %d"),
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_prev_page_cb);
    }

    for (int i = group_songs_page_start; i < page_end; i++) {
        if (group_songs_entries[i].show_disc_header) {
            char header[32];
            snprintf(header, sizeof(header), TR("Disc %d"), group_songs_entries[i].disc_number);
            add_section_header(group_songs_list, header);
        }
        if (editing) {
            /* Keep dragging on the left and selection on the right. The
             * cover and text share a left inset beyond the handle. */
            int32_t handle_width = group_songs_reorder_handle_width();
            int32_t text_reserve = BOARD_SCALE_PX(18 + 28 + 8);
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL,
                                                  text_reserve);
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);
            group_song_row_text_offset(row, group_song_has_cover(i)); /* see below */
            int slot = i - group_songs_page_start;
            lv_obj_t * handle = lv_button_create(row);
            group_songs_reorder_handles[slot] = handle;
            lv_obj_set_size(handle, handle_width, ui_music_row_height());
            lv_obj_align(handle, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_add_flag(handle, LV_OBJ_FLAG_PRESS_LOCK);
            lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_PRESSED);
            lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_USER_1);
            lv_obj_set_style_radius(handle, BOARD_SCALE_PX(8), 0);
            lv_obj_set_style_bg_opa(handle, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(handle, 0, 0);
            lv_obj_set_style_shadow_width(handle, 0, 0);
            lv_obj_set_style_pad_all(handle, 0, 0);
            lv_obj_t * grip = lv_image_create(handle);
            lv_obj_set_size(grip, BOARD_SCALE_PX(32), BOARD_SCALE_PX(32));
            set_reorder_handle_icon(grip);
            lv_image_set_inner_align(grip, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_center(grip);
            lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(handle, group_songs_reorder_handle_cb, LV_EVENT_ALL,
                                (void *) (intptr_t) slot);
            register_swipe_dead_zone(handle);
            lv_obj_t * checkbox = lv_checkbox_create(row);
            lv_checkbox_set_text(checkbox, "");
            if (group_songs_selected && group_songs_selected[i]) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
            group_songs_selection_checks[i - group_songs_page_start] = checkbox;
            lv_obj_set_style_border_color(checkbox, accent_lv_color(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(checkbox, accent_lv_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_image_src(checkbox, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_text_color(checkbox,
                lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(),
                LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_size(checkbox, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
            lv_obj_align(checkbox, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-18), 0);
            lv_obj_remove_flag(checkbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                        LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_selection_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            /* One lv_label via the shared list_row_style, not a container +
             * child label each with their own local style properties -- see
             * list_row_style's own doc comment (screen_builders.h). */
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL, BOARD_SCALE_PX(70));
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);
            /* Room for the cover from the first frame: the cover (or its
             * placeholder) is added later, and moving the text then made
             * every row jump sideways. */
            if (group_song_has_cover(i)) group_song_row_text_offset(row, true);

            lv_obj_t * quality = lv_image_create(row);
            lv_image_set_src(quality, asset_path(song_quality_asset_for_path(group_songs_entries[i].path)));
            /* Child alignment is relative to the label's padded content
             * box. Cancel the 70px text reserve so the badge is physically
             * 14px from the card edge (same rule as compact-list rows). */
            lv_obj_align(quality, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-14), 0);
            lv_obj_remove_flag(quality, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
        }
    }

    if (page_end < group_songs_count) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), TR("Next  •  %d–%d of %d"),
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_next_page_cb);
    }

    /* Recreated fresh here (lv_obj_clean() above just destroyed whatever
     * was here before) rather than kept as a truly persistent object --
     * every row in this list gets rebuilt on every populate call already,
     * so this just follows the same pattern. LV_OBJ_FLAG_IGNORE_LAYOUT
     * keeps this list's own flex column layout from trying to stack it in
     * as another row, same trick build_icon_grid_screen() uses for its
     * divider lines. Positioned/shown by refresh_group_songs_now_playing_
     * indicator() below, not here -- that also runs standalone (no rebuild)
     * whenever now_playing_path changes while this screen stays
     * open, e.g. a gapless auto-advance to the next track in the group. */
    group_songs_now_playing_bar = lv_obj_create(group_songs_list);
    lv_obj_remove_style_all(group_songs_now_playing_bar);
    lv_obj_set_size(group_songs_now_playing_bar, BOARD_SCALE_PX(5), ui_list_row_height());
    lv_obj_add_style(group_songs_now_playing_bar, &native_row_min_style, 0);
    lv_obj_add_style(group_songs_now_playing_bar, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(group_songs_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group_songs_now_playing_bar, 2, 0);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_group_songs_now_playing_indicator();
    group_songs_rows_rebuilding = false;
    refresh_group_song_thumbnails();
    lv_obj_update_layout(group_songs_list);
    if (restore_scroll) lv_obj_scroll_to_y(group_songs_list, restore_scroll_y, LV_ANIM_OFF);
    if (group_songs_anchor_pending) {
        lv_area_t list_area;
        lv_obj_get_coords(group_songs_list, &list_area);
        int anchor_index = group_songs_anchor_index;
        if (anchor_index >= group_songs_count) anchor_index = group_songs_count - 1;
        if (anchor_index >= group_songs_page_start && anchor_index < page_end) {
            lv_obj_t * row = group_songs_visible_rows[anchor_index - group_songs_page_start];
            if (row) {
                lv_area_t row_area;
                lv_obj_get_coords(row, &row_area);
                int32_t scroll_y = lv_obj_get_scroll_y(group_songs_list) +
                    row_area.y1 - list_area.y1 - group_songs_anchor_offset;
                lv_obj_scroll_to_y(group_songs_list, scroll_y, LV_ANIM_OFF);
            }
        }
        group_songs_anchor_pending = false;
    }
    group_songs_populated_page_start = group_songs_page_start;
}

/* Every screen's back button is a fixed 64x64 at the screen's own left
 * edge (build_group_songs_screen() below, build_subsonic_list_screen()) --
 * this is where a title label should start, not centered over top of it. */
#define TITLE_LABEL_LEFT_INSET 76 /* 64px back button + 12px breathing room */
/* Default right margin for a title with no right-side icon of its own on
 * that particular screen -- most build_subsonic_list_screen() callers
 * (Playlists, Saved Servers, New Connection, Queue, DLNA, Remote Control,
 * the plugin list pool, ...) never get anywhere near this wide anyway
 * (always a short fixed string), so this is mostly headroom for the ones
 * that do have a long dynamic title but no button beside it (the local
 * library's Artist -> Albums drill-down, artist_albums_screen). */
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

static void group_songs_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    bool entering_edit_at_top = !group_songs_edit_mode && group_songs_list &&
        lv_obj_get_scroll_y(group_songs_list) == 0;
    if (entering_edit_at_top) group_songs_anchor_pending = false;
    else group_songs_capture_scroll_anchor(NULL, 0);
    if (group_songs_edit_mode) {
        group_songs_edit_mode = false;
        group_songs_clear_selection();
    } else {
        group_songs_edit_mode = true;
        if (!group_songs_reset_selection()) {
            group_songs_edit_mode = false;
            group_songs_anchor_pending = false;
            group_songs_update_edit_header();
            show_error_toast(TR("Cannot select playlist entries"));
            return;
        }
    }
    populate_group_songs_rows();
}

static void group_songs_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        library_thumbnails_forget_list(group_songs_list);
        gui_list_reorder_finish(&group_songs_reorder_state, false);
        group_songs_unregister_reorder_handles();
        group_songs_reorder_state.rows = NULL;
        group_songs_reorder_state.list = NULL;
        group_songs_clear_selection();
        group_songs_edit_mode = false;
        group_songs_edit_btn = NULL;
        group_songs_delete_btn = NULL;
        group_songs_select_all_row = NULL;
        group_songs_select_all_checkbox = NULL;
        group_songs_select_all_label = NULL;
        group_songs_select_all_done_btn = NULL;
        group_songs_title_label = NULL;
        free(group_songs_source_title);
        group_songs_source_title = NULL;
        group_songs_list = NULL;
        group_songs_populated_page_start = -1;
        group_songs_anchor_pending = false;
        group_songs_rebuild_on_load = false;
        memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
        memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
        memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        gui_list_reorder_finish(&group_songs_reorder_state, false);
        group_songs_unregister_reorder_handles();
        group_songs_reorder_state.rows = NULL;
        group_songs_reorder_state.list = NULL;
        if (group_songs_edit_mode) {
            group_songs_capture_scroll_anchor(NULL, 0);
            group_songs_clear_selection();
            memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
            group_songs_edit_mode = false;
            group_songs_rebuild_on_load = true;
        }
    } else if (code == LV_EVENT_SCREEN_LOADED && group_songs_rebuild_on_load) {
        group_songs_rebuild_on_load = false;
        populate_group_songs_rows();
    }
}

static void group_song_selection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !group_songs_edit_mode ||
        group_songs_reorder_state.active) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count || !group_songs_selected) return;
    if (!group_playlist_unchanged()) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
    group_songs_selected[pos] = !group_songs_selected[pos];
    group_songs_selected_count += group_songs_selected[pos] ? 1 : -1;
    int slot = pos - group_songs_page_start;
    lv_obj_t * checkbox = slot >= 0 && slot < GROUP_SONGS_PAGE_SIZE
        ? group_songs_selection_checks[slot] : NULL;
    if (checkbox) {
        if (group_songs_selected[pos]) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
    }
    group_songs_update_edit_header();
}

static void group_songs_select_all_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !group_songs_edit_mode ||
        group_songs_reorder_state.active || group_songs_count <= 0 || !group_songs_selected) return;
    if (!group_playlist_unchanged()) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
    bool select_all = group_songs_selected_count != group_songs_count;
    for (int i = 0; i < group_songs_count; ++i) group_songs_selected[i] = select_all;
    group_songs_selected_count = select_all ? group_songs_count : 0;
    int visible_count = group_songs_count - group_songs_page_start;
    if (visible_count > GROUP_SONGS_PAGE_SIZE) visible_count = GROUP_SONGS_PAGE_SIZE;
    for (int slot = 0; slot < visible_count; ++slot) {
        lv_obj_t * checkbox = group_songs_selection_checks[slot];
        if (!checkbox) continue;
        if (select_all) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
    }
    group_songs_update_edit_header();
}

static bool group_songs_playlist_check(void * context, char * const * paths, int count) {
    (void) context;
    if (!group_playlist_unchanged() || count != group_songs_count) return false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(paths[i], group_songs_entries[i].path) != 0) return false;
    }
    return true;
}

/* Map a visible old playlist position to its new position after deleting a
 * set of ordered entry offsets. When the old top row is deleted, retain the
 * next surviving row, or the previous one when the deletion reaches EOF. */
static int group_songs_anchor_index_after_delete(int top_index, int song_count,
                                                 const int * deleted_indexes, int deleted_count) {
    if (top_index < 0 || top_index >= song_count) return -1;
    /* The delete UI builds unique ascending offsets, matching the backend's
     * ordering contract. One forward pass handles a playlist-wide selection
     * without quadratic scans. */
    int deleted_before_top = 0;
    while (deleted_before_top < deleted_count && deleted_indexes[deleted_before_top] < top_index)
        ++deleted_before_top;
    if (deleted_before_top == deleted_count || deleted_indexes[deleted_before_top] != top_index)
        return top_index - deleted_before_top;

    int next = top_index + 1;
    int deleted_cursor = deleted_before_top + 1;
    while (next < song_count && deleted_cursor < deleted_count && deleted_indexes[deleted_cursor] == next) {
        ++next;
        ++deleted_cursor;
    }
    if (next < song_count) return top_index - deleted_before_top;

    int previous = top_index - 1;
    int deleted_before_previous = deleted_before_top;
    while (previous >= 0 && deleted_before_previous > 0 &&
           deleted_indexes[deleted_before_previous - 1] == previous) {
        --previous;
        --deleted_before_previous;
    }
    return previous < 0 ? -1 : previous - deleted_before_previous;
}

static void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count) {
    group_songs_anchor_pending = false;
    if (!group_songs_list) return;
    lv_obj_update_layout(group_songs_list);
    lv_area_t list_area;
    lv_obj_get_coords(group_songs_list, &list_area);
    int best_index = -1;
    int32_t best_y = INT32_MAX;
    for (int slot = 0; slot < GROUP_SONGS_PAGE_SIZE; ++slot) {
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        lv_area_t row_area;
        lv_obj_get_coords(row, &row_area);
        if (row_area.y2 < list_area.y1 || row_area.y1 > list_area.y2 || row_area.y1 >= best_y) continue;
        best_index = group_songs_page_start + slot;
        best_y = row_area.y1;
    }
    if (best_index < 0 || best_index >= group_songs_count) return;
    int anchor_index = group_songs_anchor_index_after_delete(
        best_index, group_songs_count, deleted_indexes, deleted_count);
    if (anchor_index < 0) return;
    group_songs_anchor_index = anchor_index;
    group_songs_anchor_offset = best_y - list_area.y1;
    group_songs_anchor_pending = true;
}

static void group_songs_delete_selected_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_selected_count <= 0 ||
        !group_songs_selected || !group_songs_edit_m3u_path) return;
    int * indexes = malloc((size_t) group_songs_selected_count * sizeof(*indexes));
    if (!indexes) { show_error_toast(TR("Cannot remove entries")); return; }
    int count = 0;
    for (int i = 0; i < group_songs_count; ++i)
        if (group_songs_selected[i]) indexes[count++] = i;
    group_songs_capture_scroll_anchor(indexes, count);
    playlist_edit_status_t status = playlist_files_delete_entries_checked(
        group_songs_edit_m3u_path, indexes, count, group_songs_playlist_check, NULL);
    free(indexes);
    if (status == PLAYLIST_EDIT_REFUSED) {
        group_songs_anchor_pending = false;
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
    if (status != PLAYLIST_EDIT_OK) {
        group_songs_anchor_pending = false;
        show_error_toast(TR("Cannot remove entries"));
        return;
    }
    reload_edited_playlist();
    show_info_toast(TR("Removed from playlist"));
}

/* entries[0..count) is copied into group_songs_entries (see set_group_songs_
 * entries()) -- the caller's own array/buffer can be freed immediately after
 * this call returns, unlike the old group_t-based API where the group_t's
 * .indices had to stay valid for as long as this screen kept showing it. */
static void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path, bool music_submenu) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) { show_error_toast(TR("Out of memory")); return; }
    ++album_load_generation;
    group_songs_source_is_album = group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM;
    group_songs_music_submenu = music_submenu;
    if (editable_m3u_path) snprintf(group_songs_owned_m3u_path, sizeof(group_songs_owned_m3u_path), "%s", editable_m3u_path);
    group_songs_edit_m3u_path = editable_m3u_path ? group_songs_owned_m3u_path : NULL;
    group_songs_file_stat_valid = editable_m3u_path && stat(editable_m3u_path, &group_songs_file_stat) == 0;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    set_group_songs_entries(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    /* The title arrives already translated (show_favorites()/show_most_played()). */
    group_songs_refresh_source = name && strcmp(name, TR("Favorites")) == 0 ? GROUP_SONGS_REFRESH_FAVORITES
                                : name && strcmp(name, TR("Most Played")) == 0 ? GROUP_SONGS_REFRESH_MOST_PLAYED
                                : GROUP_SONGS_REFRESH_NONE;
    show_group_songs_editable(name, entries, count, NULL, false);
}

static void show_music_group_songs(const char * name, const char * album_artist, const char * artist_filter,
                                   const group_song_entry_t * entries, int count) {
    group_songs_refresh_source = GROUP_SONGS_REFRESH_ALBUM;
    snprintf(group_songs_refresh_album, sizeof(group_songs_refresh_album), "%s", name ? name : "");
    snprintf(group_songs_refresh_album_artist, sizeof(group_songs_refresh_album_artist), "%s",
             album_artist ? album_artist : "");
    snprintf(group_songs_refresh_artist_filter, sizeof(group_songs_refresh_artist_filter), "%s",
             artist_filter ? artist_filter : "");
    show_group_songs_editable(name, entries, count, NULL, true);
}

/* Ownership-transferring sibling of show_group_songs() -- see set_group_
 * songs_entries_owned()'s own comment for why this exists. Never editable
 * (no m3u path makes sense for a flattened, non-playlist view), same as
 * plain show_group_songs()'s own default. Caller must not free `entries`
 * after this call -- ownership has moved to group_songs_entries. */
static void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count,
                                            bool push_screen) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) {
        free_group_song_entries(entries, count);
        show_error_toast(TR("Out of memory"));
        return;
    }
    ++album_load_generation;
    group_songs_source_is_album = false;
    group_songs_music_submenu = true;
    group_songs_edit_m3u_path = NULL;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    set_group_songs_entries_owned(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    if (push_screen) nav_push(group_songs_screen);
}

static lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    group_songs_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    /* Keep dynamic album/group names on one line. The title remains bounded
     * by the existing Edit/search action reservations below. */
    row_label_enable_marquee(group_songs_title_label);
    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, TR("Edit"));
    lv_obj_add_style(group_songs_edit_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(group_songs_edit_btn, BOARD_SCALE_PX(100));
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_DELETE, NULL);

    group_songs_delete_btn = build_top_right_icon_button(scr, asset_path("playlist_actions/trash.png"),
                                                          group_songs_delete_selected_cb);
    align_screen_header_action(group_songs_delete_btn, BOARD_SCALE_PX(20));
    lv_obj_t * delete_image = lv_obj_get_child(group_songs_delete_btn, 0);
    if (delete_image) {
        lv_obj_set_size(delete_image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(delete_image, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_style(delete_image, gui_theme_accent_style(), 0);
    }
    lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);

    group_songs_list = lv_obj_create(scr);
    lv_obj_set_size(group_songs_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(group_songs_list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(group_songs_list, 0, 0);
    lv_obj_set_style_border_width(group_songs_list, 0, 0);
    /* Clear padding so rows align flush with container edges. */
    lv_obj_set_style_pad_all(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_scrollbar_mode(group_songs_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(group_songs_list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(group_songs_list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(group_songs_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_style(group_songs_list, gui_theme_accent_style(), LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(group_songs_list, BOARD_SCALE_PX(8), 0);

    library_thumbnails_bind_list(scr, group_songs_list, LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM);

    finalize_screen_navigation(scr);
    return scr;
}

/* Reusable on-screen keyboard text entry -- the app had none of this
 * before network streaming needed a way to type a server URL/username/
 * password. One persistent screen (title + textarea + T9 keypad),
 * repurposed for whatever's being entered via show_text_entry() rather
 * than a screen per use, same "one object, rebuilt/retargeted on demand"
 * approach as group_songs_screen above. Deliberately skips
 * finalize_screen_navigation() (no swipe-to-go-back) -- a swipe gesture
 * while typing on the keypad would be an easy accidental way to lose
 * whatever was being entered. (text_entry_done_cb_t itself is forward-
 * declared earlier, alongside show_text_entry(), for the PEQ screen's
 * tap-to-edit handlers.)
 *
 * T9 multi-tap keypad using themed keyboard assets. 12-key layout (1-9, Mode,
 * Shift, plus Del/Left/Right/Enter/Space) in a 4-column x 5-row grid; keys 2-9
 * cycle through their letter group (e.g. key 2 -> a -> b -> c) on repeated taps within
 * TEXT_ENTRY_MULTITAP_MS, matching classic phone-keypad multi-tap text
 * entry. Three keypad modes (ABC/NUM/SYM), cycled via the Mode key: ABC
 * cycles letters per key (0 and 1 have no letter group on this asset set,
 * so they always insert their literal digit); NUM makes every digit key
 * insert its literal digit directly, no cycling; SYM cycles each key's own
 * punctuation set (symbol0.png..symbol9.png). PEQ's numeric-only fields
 * (show_text_entry()'s `numeric` flag) skip all three and lock permanently
 * to plain-digit entry, hiding Mode/Shift (neither means anything for a
 * frequency/gain/Q value) and exposing a dedicated "./-" key instead (gain
 * needs negative numbers; nothing else here provides them once Mode is
 * hidden).
 *
 * Scope cuts, deliberate for this first pass: num_more.png/symbol_more.png
 * (a second punctuation page) aren't wired up -- symbol0-9 alone covers
 * ordinary punctuation (. , ! ? etc.), and every text_entry field here is a
 * short single-line value (URLs, names, credentials), not prose. The
 * select/ subfolder's pressed-state art isn't used either (no PRESSED/
 * RELEASED image swap, unlike screen_builders.c's icon-grid tiles) --
 * LV_EVENT_CLICKED alone is enough feedback for a keypad tap. bg.png/
 * text_bg.png/cursor.png (decorative chrome) and ok.png/ok_s.png (redundant
 * with enter.png's own baked-in "Enter" text) are unused. char_l.png/
 * char_u.png are never referenced -- confirmed byte-identical to each other
 * and to no clear distinct purpose from char.png (the Mode key's own ABC
 * glyph) by direct pixel inspection. */

/* Whether a group row gets a cover (or its placeholder) at all. */
static bool group_song_has_cover(int index) {
    return index >= 0 && index < group_songs_count && group_songs_entries[index].song_id > 0 &&
           group_songs_entries[index].artwork_key != 0;
}

static void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    int32_t target_x = (with_thumbnail ? BOARD_SCALE_PX(100) : GUI_TEXT_INSET) +
        (group_songs_edit_mode ? group_songs_reorder_handle_width() : 0);
    /* New rows have explicit label geometry but no computed coordinates
     * until LVGL lays them out. Reading those coordinates here can turn a
     * full-width label into the 40px minimum permanently. Use the configured
     * pixel values, also keeping repeated thumbnail refreshes idempotent. */
    int32_t delta = target_x - lv_obj_get_style_x(lv_obj_get_child(row, 0), 0);
    if (delta == 0) return;
    for (int child_index = 0; child_index < 2; child_index++) {
        lv_obj_t * label = lv_obj_get_child(row, child_index);
        int32_t width = lv_obj_get_style_width(label, 0) - delta;
        if (width < 40) width = 40;
        lv_obj_set_x(label, target_x);
        lv_obj_set_width(label, width);
    }
}

static void refresh_group_song_thumbnails(void) {
    if (!group_songs_list || group_songs_rows_rebuilding) return;
    lv_obj_update_layout(group_songs_list);
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    int32_t scroll_y = lv_obj_get_scroll_y(group_songs_list);
    int32_t view_bottom = scroll_y + lv_obj_get_height(group_songs_list);
    for (int index = group_songs_page_start; index < page_end; index++) {
        int slot = index - group_songs_page_start;
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        int32_t row_y = lv_obj_get_y(row);
        bool in_viewport = row_y + lv_obj_get_height(row) >= scroll_y && row_y <= view_bottom;
        bool has_cover = group_song_has_cover(index);
        if (!in_viewport || !has_cover) {
            /* Off screen the image goes but the text keeps its place, so
             * scrolling back does not shift it. */
            if (group_songs_thumbnail_images[slot]) {
                lv_obj_delete(group_songs_thumbnail_images[slot]);
                group_songs_thumbnail_images[slot] = NULL;
            }
            group_song_row_text_offset(row, has_cover);
            continue;
        }
        if (!group_songs_thumbnail_images[slot]) {
            group_songs_thumbnail_images[slot] = lv_image_create(row);
            lv_obj_remove_flag(group_songs_thumbnail_images[slot], LV_OBJ_FLAG_CLICKABLE);
            group_song_row_text_offset(row, true);
        }
        library_thumbnails_decorate_group_row(group_songs_list, row, group_songs_thumbnail_images[slot],
                                              index, slot, group_songs_entries[index].song_id,
                                              group_songs_entries[index].artwork_key);
        /* The shared decorator pads compact-list rows by 100px for the cover;
         * these rows position their labels absolutely instead (see
         * group_song_row_text_offset()), so padding would shift the text twice. */
        lv_obj_set_style_pad_left(row, 0, 0);
        int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        int32_t pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        int32_t pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
        lv_obj_set_size(group_songs_thumbnail_images[slot],
                        BOARD_SCALE_PX(72), BOARD_SCALE_PX(72));
        lv_image_set_inner_align(group_songs_thumbnail_images[slot], LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_align(group_songs_thumbnail_images[slot], LV_ALIGN_LEFT_MID,
                     BOARD_SCALE_PX(14) +
                         (group_songs_edit_mode ? group_songs_reorder_handle_width() : 0) - pad_left,
                     (pad_top - pad_bottom) / -2);
    }
}

static library_thumbnail_context_t library_thumbnail_genre_context(void) {
    int view = genre_browse_view();
    if (view == GENRE_VIEW_ALBUMS)
        return (library_thumbnail_context_t) {
            .key_kind = THUMBNAIL_KEY_ALBUM,
            .artist_group_kind = METADATA_DB_GROUP_ARTIST
        };
    return (library_thumbnail_context_t) {
        .key_kind = THUMBNAIL_KEY_ARTIST,
        .artist_group_kind = view == GENRE_VIEW_ALBUM_ARTISTS
            ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST
    };
}

static bool library_thumbnail_policy(library_thumbnail_policy_t policy) {
    if (policy == LIBRARY_THUMB_POLICY_LAZY) return library_rescan_active;
    return library_rescan_active || sd_format_active;
}

static bool detach_group_song_thumbnail(const lv_image_dsc_t *dsc) {
    bool detached = false;
    for (int slot = 0; slot < GROUP_SONGS_PAGE_SIZE; ++slot) {
        lv_obj_t *image = group_songs_thumbnail_images[slot];
        if (!image || lv_image_get_src(image) != dsc) continue;
        lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        detached = true;
    }
    return detached;
}

static void refresh_group_thumbnail_binding(void) {
    refresh_group_song_thumbnails();
}

static void library_thumbnail_show_busy(library_thumbnail_cover_scope_t scope) {
    const char *title = scope == LIBRARY_THUMB_COVER_ALL
        ? TR("Refreshing\nall covers...") : TR("Reloading\ncover...");
    library_rescan_token = gui_busy_show(title, TR("Preparing cover refresh..."));
    gui_busy_set_progress(library_rescan_token, 0);
}

/* Called after list deletion: unregistering is pointer identity only, so it never
 * touches a freed LVGL object. */
static void forget_deleted_library_lists(void) {
    lv_obj_t *dead[] = { all_songs_list, artists_list, albums_list, album_artist_list,
                         recently_added_list, genres_list };
    for (size_t i = 0; i < sizeof(dead) / sizeof(dead[0]); ++i)
        if (dead[i]) library_thumbnails_forget_list(dead[i]);
    all_songs_list = artists_list = albums_list = album_artist_list = recently_added_list = NULL;
}

/* Unfiltered (NULL filter, "every album in the library") -- uses metadata_
 * db_get_albums_page_filtered() rather than metadata_db_get_groups_page(
 * METADATA_DB_GROUP_ALBUM, ...), since the latter groups by album name
 * alone (the exact bug this session fixed) and this screen must show the
 * corrected (album, album_artist) grouping. */
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_albums_page_sorted(NULL, effective_album_sort(), offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        snprintf(out_rows[i].subtitle, sizeof(out_rows[i].subtitle), "%s", rows[i].album_artist);
        out_rows[i].identity = rows[i].first_song_id;
        if (rows[i].album_artist[0]) {
            albumart_info_t info = {0};
            snprintf(info.albumartist, sizeof(info.albumartist), "%s", rows[i].album_artist);
            snprintf(info.album, sizeof(info.album), "%s", rows[i].name);
            out_rows[i].artwork_key = albumart_thumbnail_key(&info);
        } else {
            song_row_t representative;
            if (metadata_db_get_song_by_id(rows[i].first_song_id, &representative))
                out_rows[i].artwork_key = song_album_thumbnail_key(&representative);
        }
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n;
}

static void artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    index = search_remap_index(SEARCH_BINDING_ARTISTS, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ARTIST);
}

/* artist_filter scopes an album to one credited artist, for the Artists
 * drill-down where a track tagged "A;B" puts the album under both. The album
 * group's own song_count covers the whole album, so a filtered listing has to
 * count its own rows or it would promise tracks it will not show. Album
 * Artists and the plain Albums list pass NULL and keep the whole album. */
static group_song_entry_t * load_album_entries_filtered(const char * name, const char * album_artist,
                                                         int song_count, const char * artist_filter,
                                                         int * out_count, const atomic_bool * cancel) {
    *out_count = 0;
    int32_t generation_before = 0;
    bool snapshot_generation_valid = metadata_db_try_get_generation(&generation_before);
    if (artist_filter && artist_filter[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist, name, NULL);
        song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    }
    if (song_count <= 0) return NULL;
    song_row_t * songs = calloc((size_t) song_count, sizeof(*songs));
    group_song_entry_t * entries = calloc((size_t) song_count, sizeof(*entries));
    if (!songs || !entries) { free(songs); free(entries); return NULL; }
    int n = 0;
    while (n < song_count) {
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) break;
        int want = song_count - n;
        if (want > 64) want = 64;
        int got = (artist_filter && artist_filter[0])
                      ? metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist, name, NULL,
                                                            n, want, songs + n)
                      : metadata_db_get_album_songs(name, album_artist, n, songs + n, want);
        if (got <= 0) break;
        n += got;
        if (got < want) break;
    }
    if (n <= 0 || (cancel && atomic_load_explicit(cancel, memory_order_relaxed))) {
        free(songs);
        free_group_song_entries(entries, song_count);
        return NULL;
    }
    int32_t generation_after = 0;
    if (!snapshot_generation_valid || !metadata_db_try_get_generation(&generation_after) ||
        generation_before != generation_after)
        snapshot_generation_valid = false;
    qsort(songs, (size_t)n, sizeof(*songs), cmp_song_disc_track);
    if (snapshot_generation_valid) {
        int snapshot_count = n < GROUP_SONGS_PAGE_SIZE ? n : GROUP_SONGS_PAGE_SIZE;
        for (int i = 0; i < snapshot_count; i++)
            library_selection_snapshot_store(&songs[i], SELECTION_SNAPSHOT_GROUP, -1, generation_before);
    }
    bool multi_disc = album_songs_are_multi_disc(songs, n);
    for (int i = 0; i < n; i++) {
        char title[384];
        format_music_submenu_identity(&songs[i], title, sizeof(title));
        entries[i].path = strdup(songs[i].path);
        entries[i].title = strdup(title);
        entries[i].song_id = songs[i].id;
        entries[i].artwork_key = song_album_thumbnail_key(&songs[i]);
        if (!entries[i].path || !entries[i].title) {
            free(songs);
            free_group_song_entries(entries, song_count);
            return NULL;
        }
    }
    set_album_disc_headers(songs, n, multi_disc, entries);
    free(songs);
    *out_count = n;
    return entries;
}

/* The artist whose album list is on screen, when that list is a plain artist
 * rather than an album artist. NULL elsewhere, so only the Artists path
 * scopes an album to one contributor. */
static const char * artist_albums_song_filter(void);

/* One album read at a time, plus one latest pending request. Requests own
 * strings and results; only the completion timer touches the LVGL tree. */
typedef struct {
    group_row_t group;
    char artist_filter[128];
    uint64_t generation;
    int32_t catalog_generation;
    int autoplay_mode; /* -1 for browsing; collection actions run after the read. */
    atomic_bool cancel, done;
    group_song_entry_t * entries;
    int count;
} album_load_request_t;
static album_load_request_t * album_load_job, * album_load_pending;
static bool library_screen_in_navigation(lv_obj_t * screen);
static bool play_current_group(play_mode_t mode);
static pthread_t album_load_thread;
static lv_timer_t * album_load_timer;

static void free_album_load_request(album_load_request_t * request) {
    if (!request) return;
    free_group_song_entries(request->entries, request->count);
    free(request);
}

static void * album_load_worker(void * arg) {
    album_load_request_t * request = arg;
    request->entries = load_album_entries_filtered(request->group.name, request->group.album_artist,
        request->group.song_count, request->artist_filter, &request->count, &request->cancel);
    atomic_store_explicit(&request->done, true, memory_order_release);
    return NULL;
}

static bool start_album_load(album_load_request_t * request) {
    if (pthread_create(&album_load_thread, NULL, album_load_worker, request) != 0) return false;
    album_load_job = request;
    return true;
}

static void album_load_poll(lv_timer_t * timer) {
    if (album_load_job && atomic_load_explicit(&album_load_job->done, memory_order_acquire)) {
        /* Do not modify the destination while a transition shows its frozen
         * loading snapshot. Back navigation is allowed throughout the read. */
        if (gui_navigation_transition_in_progress()) return;
        pthread_join(album_load_thread, NULL);
        album_load_request_t * request = album_load_job;
        album_load_job = NULL;
        bool current = request->generation == album_load_generation &&
            library_screen_in_navigation(group_songs_screen) &&
            group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM &&
            !atomic_load_explicit(&request->cancel, memory_order_relaxed);
        if (current) {
            bool valid = request->catalog_generation == tagcache_generation() && !library_rescan_active;
            if (valid && request->entries) {
                set_group_songs_entries_owned(request->entries, request->count);
                request->entries = NULL;
                request->count = 0;
            }
            populate_group_songs_rows();
            gui_navigation_invalidate_back_snapshot(group_songs_screen);
            if (lv_screen_active() == group_songs_screen) {
                if (!valid) show_info_toast(TR("Library changed. Open the album again."));
                else if (!group_songs_count) show_error_toast(TR("Cannot load album tracks"));
                else if (request->autoplay_mode >= 0)
                    (void) play_current_group((play_mode_t) request->autoplay_mode);
            }
        }
        free_album_load_request(request);
    }
    if (!album_load_job && album_load_pending) {
        album_load_request_t * request = album_load_pending;
        album_load_pending = NULL;
        if (request->generation != album_load_generation ||
            !library_screen_in_navigation(group_songs_screen)) {
            free_album_load_request(request);
        } else if (!start_album_load(request)) {
            free_album_load_request(request);
            show_error_toast(TR("Cannot load album tracks"));
            if (lv_screen_active() == group_songs_screen) populate_group_songs_rows();
        }
    }
    if (!album_load_job && !album_load_pending) lv_timer_pause(timer);
}

static void cancel_album_load(void) {
    ++album_load_generation;
    if (album_load_job) {
        atomic_store_explicit(&album_load_job->cancel, true, memory_order_relaxed);
        pthread_join(album_load_thread, NULL);
        free_album_load_request(album_load_job);
        album_load_job = NULL;
    }
    free_album_load_request(album_load_pending);
    album_load_pending = NULL;
    if (album_load_timer) lv_timer_delete(album_load_timer);
    album_load_timer = NULL;
}

static bool show_album_group_filtered(const group_row_t * group, const char * artist_filter) {
    album_load_request_t * request = calloc(1, sizeof(*request));
    if (!request) return false;
    request->group = *group;
    request->autoplay_mode = -1;
    snprintf(request->artist_filter, sizeof(request->artist_filter), "%s", artist_filter ? artist_filter : "");
    request->catalog_generation = tagcache_generation();
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!album_load_timer) album_load_timer = lv_timer_create(album_load_poll, 25, NULL);
    if (!album_load_timer) { free(request); return false; }
    if (!album_load_job && !start_album_load(request)) { free(request); return false; }
    if (album_load_job != request) {
        atomic_store_explicit(&album_load_job->cancel, true, memory_order_relaxed);
        free_album_load_request(album_load_pending);
        album_load_pending = request;
    }
    group_songs_loading = true;
    show_music_group_songs(group->name, group->album_artist, artist_filter, NULL, 0);
    group_songs_loading = false;
    request->generation = album_load_generation;
    lv_timer_resume(album_load_timer);
    return true;
}

static bool show_album_group(const group_row_t * group) {
    return show_album_group_filtered(group, NULL);
}

/* Shared library screens may already be below Now Playing. Once the target
 * has loaded successfully, return to its original slot instead of keeping
 * a second copy and the intervening player entries in the Back history. */
static void collapse_now_playing_library_return(lv_obj_t * target) {
    if (!target || lv_screen_active() != target) return;
    int depth = gui_navigation_get_depth();
    for (int i = 0; i < depth - 1; i++) {
        if (gui_navigation_get_screen_at(i) != target) continue;
        while (gui_navigation_get_depth() > i + 1)
            nav_remove_stack_slot(gui_navigation_get_depth() - 1);
        return;
    }
}

bool gui_library_open_now_playing_group(const char * path, bool album) {
    if (!path || !path[0] || library_rescan_active || gui_library_navigation_blocked()) return false;

    /* A local tagcache path is the membership guard. This keeps provider,
     * radio, and Subsonic metadata from accidentally opening a local group
     * that happens to share the same display name. The indexed path lookup
     * and following group lookup stay bounded; no library-wide song list is
     * materialized on the UI thread. */
    song_row_t song;
    if (!metadata_db_get_song_by_path(path, &song)) return false;

    if (!album) {
        if (!song.tags.artist[0]) return false;
        char artist_name[TAGCACHE_TAG_MAX];
        tagcache_artist_primary(song.tags.artist, artist_name, sizeof(artist_name));
        if (!artist_name[0]) return false;
        int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ARTIST,
                                                       artist_name, NULL);
        group_row_t artist;
        if (offset < 0 || offset > INT_MAX ||
            metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, (int) offset, 1, &artist) != 1)
            return false;
        if (tagcache_cmp_ascii(artist.name, artist_name) != 0) return false;
        show_artist_albums(artist.name, METADATA_DB_GROUP_ARTIST);
        collapse_now_playing_library_return(artist_albums_screen);
        return true;
    }

    if (!song.tags.album[0]) return false;
    /* The scanner normalizes missing ALBUMARTIST to ARTIST; keep that same
     * effective identity for older cache rows as well. This remains distinct
     * from song.tags.artist when an album artist is explicitly present. */
    const char * album_artist = song.tags.album_artist[0]
        ? song.tags.album_artist : song.tags.artist;
    int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                                                   song.tags.album, album_artist);
    /* Rows written by the current scanner use ARTIST when ALBUMARTIST is
     * absent. Older tagcache generations can still have an empty album-artist
     * key, so allow that exact legacy pair if the normalized identity misses. */
    const char * group_album_artist = album_artist;
    if (offset < 0 && !song.tags.album_artist[0]) {
        offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                                              song.tags.album, "");
        group_album_artist = "";
    }
    group_row_t group;
    if (offset < 0 || offset > INT_MAX ||
        metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int) offset, 1, &group) != 1)
        return false;
    /* Guard against an offset resolving to a different same-titled album
     * if the database changed between the path and group lookups. */
    if (tagcache_cmp_ascii(group.name, song.tags.album) != 0 ||
        tagcache_cmp_ascii(group.album_artist, group_album_artist) != 0) return false;
    if (!show_album_group(&group)) return false;
    collapse_now_playing_library_return(group_songs_screen);
    return true;
}

static void album_row_click_cb(int index) {
    if (library_rescan_active) return;
    /* Resolve this specific (album, album_artist) pair at its current
     * display position via a single-row offset lookup (same pattern as All
     * Songs' own row-click resolution) -- disambiguated by album_artist too,
     * so two different artists sharing an album title never collide. */
    group_row_t group;
    if (!resolve_album_display_row(index, &group)) return;
    (void) show_album_group(&group);
}

/* Builds the Artists screen using a paged provider
 * (compact_list_set_paged_provider()) to load artist groups incrementally. */
static lv_obj_t * build_artists_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Artists"), generic_back_cb, NULL, 0, artist_row_click_cb,
                                                artist_collection_long_press_cb, &artists_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(artists_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    compact_list_set_paged_provider(artists_list, artists_fetch_page, NULL, artist_count);
    library_thumbnails_bind_list(scr, artists_list, LIBRARY_THUMB_LIST_ARTISTS);
    compact_list_set_trailing_click(artists_list, artist_collection_more_click_cb);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_albums_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Albums"), generic_back_cb, NULL, 0, album_row_click_cb,
                                                album_row_long_press_cb, &albums_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(albums_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    /* Cache invalidation belongs to the operation that replaces the library
     * (refresh_library_screens_after_reload(), SD reinsertion, UI reload),
     * done once there. Clearing here repeated it and, mid-rebuild, redrew
     * lists that were not rebuilt yet. */
    /* Decorator before the paged provider so the first (now synchronous)
     * window fill already has pad_left=100 and the 72px cover slot; names
     * then lay out to the right of the art instead of under it. */
    library_thumbnails_bind_list(scr, albums_list, LIBRARY_THUMB_LIST_ALBUMS);
    compact_list_set_trailing_click(albums_list, album_more_click_cb);
    compact_list_set_paged_provider(albums_list, albums_fetch_page, NULL, album_count);
    finalize_screen_navigation(scr);
    return scr;
}

static void album_artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    index = search_remap_index(SEARCH_BINDING_ALBUM_ARTIST, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ALBUM_ARTIST);
}

static lv_obj_t * build_album_artist_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Album Artist"), generic_back_cb, NULL, 0, album_artist_row_click_cb,
                                                album_artist_collection_long_press_cb, &album_artist_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(album_artist_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    compact_list_set_paged_provider(album_artist_list, album_artists_fetch_page, NULL, album_artist_count);
    library_thumbnails_bind_list(scr, album_artist_list, LIBRARY_THUMB_LIST_ALBUM_ARTISTS);
    compact_list_set_trailing_click(album_artist_list, album_artist_collection_more_click_cb);
    finalize_screen_navigation(scr);
    return scr;
}

static int count_genres_from_database(void) {
    int total = 0;
    const int batch = 256;
    group_row_t * buf = malloc(sizeof(group_row_t) * (size_t) batch);
    if (!buf) return 0;
    while (true) {
        int n = metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, total, batch, buf);
        if (n <= 0) break;
        total += n;
        if (n < batch) break;
    }
    free(buf);
    return total;
}

static int genres_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    if (!rows) return 0;
    int n = metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, offset, count, rows);
    for (int i = 0; i < n; i++) {
        const char * name = rows[i].genre_name[0] ? rows[i].genre_name : rows[i].name;
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", name);
        snprintf(out_rows[i].subtitle, sizeof(out_rows[i].subtitle), TR_N("%d song", "%d songs", rows[i].song_count),
                 rows[i].song_count);
        out_rows[i].identity = rows[i].first_song_id;
    }
    free(rows);
    return n;
}

#define GENRE_SONGS_CAP 4000

static bool show_genre_songs_internal(bool push_screen, const char * genre_name) {
    if (!genre_name || !genre_name[0]) return false;
    /* Database refresh passes the saved genre buffer back into this function. */
    char genre[sizeof(group_songs_refresh_genre)];
    snprintf(genre, sizeof(genre), "%s", genre_name);
    genre_name = genre;
    int64_t total64 = metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, genre_name);
    int real_total = (total64 > 0 && total64 <= INT_MAX) ? (int) total64 : 0;
    if (real_total <= 0) return false;

    int total = real_total > GENRE_SONGS_CAP ? GENRE_SONGS_CAP : real_total;
    group_song_entry_t * entries = calloc((size_t) total, sizeof(*entries));
    if (!entries) return false;

    const int page_size = 64;
    song_row_t * page = malloc(sizeof(song_row_t) * (size_t) page_size);
    if (!page) {
        free(entries);
        return false;
    }

    int n = 0;
    bool failed = false;
    while (n < total) {
        int want = total - n;
        if (want > page_size) want = page_size;
        int got = metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, genre_name, n, want, page);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[384];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            entries[n + i].path = strdup(page[i].path);
            entries[n + i].title = strdup(title);
            entries[n + i].song_id = page[i].id;
            entries[n + i].artwork_key = song_album_thumbnail_key(&page[i]);
            entries[n + i].disc_number = page[i].tags.disc_number > 0 ? page[i].tags.disc_number : 1;
            entries[n + i].show_disc_header = false;
            if (!entries[n + i].path || !entries[n + i].title) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        n += got;
        if (got < want) break;
    }
    free(page);

    if (failed || n <= 0) {
        free_group_song_entries(entries, total);
        return false;
    }

    if (n < total) {
        group_song_entry_t * shrunk = realloc(entries, sizeof(*entries) * (size_t) n);
        if (shrunk) entries = shrunk;
    }

    group_songs_refresh_source = GROUP_SONGS_REFRESH_GENRE;
    snprintf(group_songs_refresh_genre, sizeof(group_songs_refresh_genre), "%s", genre_name);
    show_group_songs_take_ownership(genre_name, entries, n, push_screen);
    return true;
}

/* ---- Genre browsing by artist or album (issue #191) ----
 *
 * The Genres title-bar settings icon picks how a genre opens: All Songs keeps
 * the flat song list, Artists / Album Artist list the genre's artists and
 * then each artist's albums in that genre, Albums lists the genre's albums.
 * An album opens the normal album screen with all of its tracks. The page
 * providers can run on the compact-list fetch worker, so the current query
 * is read under genre_browse_mutex. */
static lv_obj_t * genre_groups_screen = NULL;
static lv_obj_t * genre_groups_list = NULL;
static lv_obj_t * genre_groups_title_label = NULL;
static lv_obj_t * genre_albums_screen = NULL;
static lv_obj_t * genre_albums_list = NULL;
static lv_obj_t * genre_albums_title_label = NULL;
static lv_obj_t * genre_view_screen = NULL;
static lv_obj_t * genre_view_options[GENRE_VIEW_COUNT];
static pthread_mutex_t genre_browse_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    char genre[METADATA_DB_TEXT_MAX];
    int view;
    char artist[128];
} genre_browse_query_t;
static genre_browse_query_t genre_browse_current;

static genre_browse_query_t genre_browse_snapshot(void) {
    pthread_mutex_lock(&genre_browse_mutex);
    genre_browse_query_t query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    return query;
}

static int genre_browse_view(void) {
    return genre_browse_snapshot().view;
}

static metadata_db_group_kind_t genre_view_artist_kind(int view) {
    return view == GENRE_VIEW_ALBUM_ARTISTS ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST;
}

static metadata_db_group_kind_t genre_view_group_kind(int view) {
    return view == GENRE_VIEW_ALBUMS ? METADATA_DB_GROUP_ALBUM : genre_view_artist_kind(view);
}

/* Same row content as Music -> Albums and Music -> Artists. */
static void fill_genre_album_row(compact_list_page_row_t * out, const group_row_t * row) {
    snprintf(out->label, sizeof(out->label), "%s", row->name);
    snprintf(out->subtitle, sizeof(out->subtitle), "%s", row->album_artist);
    out->identity = row->first_song_id;
    if (row->album_artist[0]) {
        albumart_info_t info = {0};
        snprintf(info.albumartist, sizeof(info.albumartist), "%s", row->album_artist);
        snprintf(info.album, sizeof(info.album), "%s", row->name);
        out->artwork_key = albumart_thumbnail_key(&info);
    } else {
        song_row_t representative;
        if (metadata_db_get_song_by_id(row->first_song_id, &representative))
            out->artwork_key = song_album_thumbnail_key(&representative);
    }
}

static void fill_genre_artist_row(compact_list_page_row_t * out, const group_row_t * row) {
    snprintf(out->label, sizeof(out->label), "%s", row->name);
    snprintf(out->subtitle, sizeof(out->subtitle), TR_N("%d song", "%d songs", row->song_count), row->song_count);
    out->identity = row->first_song_id;
    out->artwork_key = albumart_artist_thumbnail_key(row->name);
    snprintf(out->artwork_name, sizeof(out->artwork_name), "%s", row->name);
}

static int genre_groups_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    metadata_db_group_kind_t kind = genre_view_group_kind(query.view);
    int n = rows ? metadata_db_get_genre_groups_page(kind, query.genre, METADATA_DB_GROUP_ARTIST, NULL,
                                                     offset, count, rows, NULL) : 0;
    for (int i = 0; i < n; i++) {
        if (kind == METADATA_DB_GROUP_ALBUM) fill_genre_album_row(&out_rows[i], &rows[i]);
        else fill_genre_artist_row(&out_rows[i], &rows[i]);
    }
    free(rows);
    return n;
}

static int genre_albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query.genre,
                                                     genre_view_artist_kind(query.view), query.artist,
                                                     offset, count, rows, NULL) : 0;
    for (int i = 0; i < n; i++) fill_genre_album_row(&out_rows[i], &rows[i]);
    free(rows);
    return n;
}

static int genre_groups_count(const genre_browse_query_t * query, bool albums_of_artist) {
    int total = 0;
    if (albums_of_artist)
        (void) metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query->genre,
                                                 genre_view_artist_kind(query->view), query->artist,
                                                 0, 0, NULL, &total);
    else
        (void) metadata_db_get_genre_groups_page(genre_view_group_kind(query->view), query->genre,
                                                 METADATA_DB_GROUP_ARTIST, NULL, 0, 0, NULL, &total);
    return total;
}

/* Genre rows carry the scanner's album identity; resolve the real album
 * group (with the legacy empty album-artist key as fallback) so the album
 * screen gets its full track count. */
static void open_genre_album(const group_row_t * row) {
    const char * album_artist = row->album_artist;
    int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row->name, album_artist);
    if (offset < 0) {
        album_artist = "";
        offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row->name, album_artist);
    }
    group_row_t group;
    if (offset < 0 || offset > INT_MAX ||
        metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int) offset, 1, &group) != 1) return;
    if (tagcache_cmp_ascii(group.name, row->name) != 0 ||
        tagcache_cmp_ascii(group.album_artist, album_artist) != 0) return;
    (void) show_album_group(&group);
}

static void show_genre_list(lv_obj_t * screen, lv_obj_t * list, lv_obj_t * title_label, const char * title,
                            compact_list_fetch_page_cb_t fetch_page, int total) {
    lv_label_set_text(title_label, title);
    screen_title_fit(title_label);
    compact_list_set_paged_provider(list, fetch_page, NULL, total);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    nav_push(screen);
}

static void genre_albums_row_click_cb(int index) {
    if (library_rescan_active) return;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t row;
    if (metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query.genre, genre_view_artist_kind(query.view),
                                          query.artist, index, 1, &row, NULL) != 1) return;
    open_genre_album(&row);
}

static void genre_groups_row_click_cb(int index) {
    if (library_rescan_active) return;
    genre_browse_query_t query = genre_browse_snapshot();
    metadata_db_group_kind_t kind = genre_view_group_kind(query.view);
    group_row_t row;
    if (metadata_db_get_genre_groups_page(kind, query.genre, METADATA_DB_GROUP_ARTIST, NULL,
                                          index, 1, &row, NULL) != 1) return;
    if (kind == METADATA_DB_GROUP_ALBUM) {
        open_genre_album(&row);
        return;
    }
    pthread_mutex_lock(&genre_browse_mutex);
    snprintf(genre_browse_current.artist, sizeof(genre_browse_current.artist), "%s", row.name);
    query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    show_genre_list(genre_albums_screen, genre_albums_list, genre_albums_title_label, row.name,
                    genre_albums_fetch_page, genre_groups_count(&query, true));
}

static void show_genre_groups(const char * genre_name, int view) {
    pthread_mutex_lock(&genre_browse_mutex);
    snprintf(genre_browse_current.genre, sizeof(genre_browse_current.genre), "%s", genre_name);
    genre_browse_current.view = view;
    genre_browse_current.artist[0] = '\0';
    genre_browse_query_t query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    show_genre_list(genre_groups_screen, genre_groups_list, genre_groups_title_label, genre_name,
                    genre_groups_fetch_page, genre_groups_count(&query, false));
}

/* Library reloads keep these screens; re-read whatever they show. */
static void rearm_genre_browse_lists(void) {
    genre_browse_query_t query = genre_browse_snapshot();
    if (!query.genre[0]) return;
    rearm_library_list(genre_groups_list, genre_groups_fetch_page, genre_groups_count(&query, false));
    if (query.artist[0])
        rearm_library_list(genre_albums_list, genre_albums_fetch_page, genre_groups_count(&query, true));
}

static lv_obj_t * build_genre_browse_screen(lv_obj_t ** out_list, lv_obj_t ** out_title,
                                            compact_list_click_cb_t on_click) {
    lv_obj_t * scr = build_compact_list_screen(TR("Genres"), generic_back_cb, NULL, 0, on_click, NULL,
                                                out_list, out_title, LIST_ROW_WIDTH_WIDE, true,
                                                gui_theme_accent_style());
    compact_list_set_row_height(*out_list, MUSIC_LIST_ROW_HEIGHT);
    library_thumbnails_bind_list(scr, *out_list,
        on_click == genre_groups_row_click_cb ? LIBRARY_THUMB_LIST_GENRE_GROUPS : LIBRARY_THUMB_LIST_GENRE_ALBUMS);
    finalize_screen_navigation(scr);
    return scr;
}

static void refresh_genre_view_options(void) {
    for (int i = 0; i < GENRE_VIEW_COUNT; i++)
        if (genre_view_options[i])
            lv_obj_set_style_border_width(genre_view_options[i],
                                          current_settings.genre_view_mode == i ? BOARD_SCALE_PX(3) : 0, 0);
}

static void genre_view_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int view = (int) (intptr_t) lv_event_get_user_data(e);
    if (view < 0 || view >= GENRE_VIEW_COUNT || current_settings.genre_view_mode == view) return;
    current_settings.genre_view_mode = view;
    settings_save_async(&current_settings);
    refresh_genre_view_options();
}

static lv_obj_t * build_genre_view_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Display"), &title, &list);
    (void) title;
    static const char * labels[GENRE_VIEW_COUNT] = { N_("All Songs"), N_("Artists"), N_("Album Artist"),
                                                     N_("Albums") };
    for (int i = 0; i < GENRE_VIEW_COUNT; i++) {
        genre_view_options[i] = add_pill_option_row(list, TR(labels[i]), false, genre_view_option_cb,
                                                    (void *) (intptr_t) i);
        lv_obj_add_style(genre_view_options[i], gui_theme_accent_outline_style(), 0);
    }
    refresh_genre_view_options();
    return screen;
}

static void genre_view_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!genre_view_screen) genre_view_screen = build_genre_view_screen();
    if (!genre_view_screen) return;
    refresh_genre_view_options();
    nav_push(genre_view_screen);
}

static void genre_row_click_cb(int index) {
    if (library_rescan_active) return;
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, index, 1, &group) != 1) return;
    const char * genre_name = group.genre_name[0] ? group.genre_name : group.name;
    if (!genre_name[0]) return;
    int view = current_settings.genre_view_mode;
    if (view > GENRE_VIEW_SONGS && view < GENRE_VIEW_COUNT && genre_groups_screen) {
        show_genre_groups(genre_name, view);
        return;
    }
    (void) show_genre_songs_internal(true, genre_name);
}

static lv_obj_t * build_genres_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Genres"), generic_back_cb, NULL, 0, genre_row_click_cb,
                                                NULL, &genres_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, false, NULL);
    compact_list_set_row_height(genres_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(genres_list, genres_fetch_page, NULL, count_genres_from_database());
    build_top_right_icon_button(scr, asset_path("settings/playback.png"), genre_view_open_cb);
    finalize_screen_navigation(scr);
    return scr;
}

typedef const char * (*az_index_name_of_t)(int display_index);

/* The A-Z widgets, visibility timer and raw pointer drag timer are owned by
 * library_az_index. This GUI supplies active-list policy and the shared raw
 * pointer-indev lookup. Keep this wrapper for the existing public callback. */
static bool az_index_strip_allowed(lv_obj_t * list) {
    return list != albums_list || effective_album_sort() == METADATA_DB_ALBUM_SORT_NAME;
}

void poll_az_index_drag(lv_timer_t * timer) {
    library_az_index_poll(timer);
}

static bool resolve_album_display_row(int index, group_row_t * out) {
    if (index < 0 || !out) return false;
    if (library_search_showing_results(SEARCH_BINDING_ALBUMS)) {
        int canonical;
        if (!library_search_try_map_filtered_row(SEARCH_BINDING_ALBUMS, index, &canonical)) return false;
        return metadata_db_get_albums_page_filtered(NULL, canonical, 1, out) == 1;
    }
    return metadata_db_get_albums_page_sorted(NULL, effective_album_sort(), index, 1, out) == 1;
}

static lv_obj_t * search_az_strip_for_screen(lv_obj_t *screen) {
    return library_az_index_strip_for_screen(screen);
}

static void search_refresh_now_playing(void) {
    refresh_now_playing_indicators();
}

static library_search_group_item_t group_songs_search_item(int index) {
    if (index < 0 || index >= group_songs_count) return (library_search_group_item_t){0};
    return (library_search_group_item_t){
        .path = group_songs_entries[index].path,
        .song_id = group_songs_entries[index].song_id,
        .artwork_key = group_songs_entries[index].artwork_key,
    };
}

/* Kept as the legacy ABI: Subsonic registers before gui_library_init(), so
 * the wrapper installs the complete, program-lifetime callback set there. */
void register_search(search_binding_id_t id, lv_obj_t *screen, lv_obj_t *list,
                     az_index_name_of_t name_of, const int *count_ptr,
                     bool is_overlay_list, bool db_backed,
                     metadata_db_az_kind_t db_kind,
                     compact_list_fetch_page_cb_t restore_fetch_page) {
    library_search_config_t config = {0};
    config.screen = screen;
    config.list = list;
    config.name_of = name_of;
    config.count_ptr = count_ptr;
    config.is_overlay_list = is_overlay_list;
    config.db_backed = db_backed;
    config.db_kind = db_kind;
    config.restore_fetch_page = restore_fetch_page;
    if (id == SEARCH_BINDING_FILES) {
        config.files_title = files_title_label;
        config.files_options = files_options_btn;
        config.inset_search_beside_options = true;
    } else if (id == SEARCH_BINDING_GROUP_SONGS) {
        config.restore_thumbnail_list = group_songs_list;
        config.group_item = group_songs_search_item;
    }
    config.az_strip_for_screen = search_az_strip_for_screen;
    config.refresh_now_playing = search_refresh_now_playing;
    library_search_register(id, &config);
}

void gui_library_apply_sorting(gui_library_sort_scope_t scope) {
    if (scope == GUI_LIBRARY_SORT_FILES) {
        if (library_search_is_active(SEARCH_BINDING_FILES)) search_close_if_active_for_screen(files_screen);
        file_browser_set_sort_mode((file_browser_sort_mode_t) current_settings.file_sort_mode);
    } else if (scope == GUI_LIBRARY_SORT_ALBUMS && albums_list) {
        if (library_search_is_active(SEARCH_BINDING_ALBUMS)) search_close_if_active_for_screen(albums_screen);
        int artists = 0, album_artists = 0, albums = 0;
        metadata_db_get_group_counts(&artists, &album_artists, &albums);
        compact_list_set_paged_provider(albums_list, albums_fetch_page, NULL, albums);
        compact_list_scroll_to_index(albums_list, 0);
        lv_obj_t * az_strip = library_az_index_strip_for_screen(albums_screen);
        if (az_strip) lv_obj_add_flag(az_strip, LV_OBJ_FLAG_HIDDEN);
    } else if ((scope == GUI_LIBRARY_SORT_ARTIST_ALBUMS ||
                scope == GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS) && artist_albums_list) {
        char name[sizeof(artist_albums_current_name)];
        metadata_db_group_kind_t kind;
        pthread_mutex_lock(&artist_albums_state_mutex);
        snprintf(name, sizeof(name), "%s", artist_albums_current_name);
        kind = artist_albums_current_kind;
        pthread_mutex_unlock(&artist_albums_state_mutex);
        metadata_db_group_kind_t affected_kind = scope == GUI_LIBRARY_SORT_ARTIST_ALBUMS
            ? METADATA_DB_GROUP_ARTIST : METADATA_DB_GROUP_ALBUM_ARTIST;
        if (name[0] && kind == affected_kind) populate_artist_albums_view(name, kind);
    }
    if (scope != GUI_LIBRARY_SORT_FILES) refresh_now_playing_indicators();
    gui_navigation_invalidate_theme_snapshots();
}



/* Files' search (SEARCH_BINDING_FILES) is the one binding with no per-
 * screen row-click callback of its own to add a search_remap_index() line
 * to (see the other four's own callbacks) -- it plays the tapped result
 * directly, building its own playlist fresh in exactly the order currently
 * displayed (the filtered subset if one's active, matching
 * all_songs_row_click_cb's own "build in display order, pass display_index
 * straight through" shape), so display_index never needs remapping here. */
static void files_search_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    gui_player_seed_selection_song(NULL, NULL);
    int all_songs_display_index = library_search_showing_results(SEARCH_BINDING_FILES)
        ? search_remap_index(SEARCH_BINDING_FILES, display_index) : display_index;
    set_player_source_all_songs(all_songs_display_index);
    on_file_selected_lazy_all_songs(all_songs_display_index);
}

static const char * group_songs_search_name_of(int index) {
    if (index >= 0 && index < group_songs_count && group_songs_entries[index].title)
        return group_songs_entries[index].title;
    return "";
}

static void group_songs_search_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    group_play_at(real_index);
}

static void group_songs_search_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    if (real_index >= 0 && real_index < group_songs_count && group_songs_entries[real_index].path)
        open_song_context_menu(group_songs_entries[real_index].path);
}

static void update_group_songs_search_button(bool editing) {
    /* Album views have no Edit action; give their title the space up to
     * Search. Editable playlists reserve whichever action remains visible:
     * Edit in normal mode, trash while editing. */
    lv_obj_t * title_action = group_songs_edit_m3u_path
        ? (group_songs_edit_mode ? group_songs_delete_btn : group_songs_edit_btn)
        : NULL;
    library_search_update_header(SEARCH_BINDING_GROUP_SONGS, group_songs_title_label,
                                 title_action, editing);
}

static void refresh_group_songs_search_now_playing(void) {
    if (!group_songs_search_list) return;
    int target = -1;
    if (library_search_is_active(SEARCH_BINDING_GROUP_SONGS) && now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) {
                target = library_search_find_filtered_row(SEARCH_BINDING_GROUP_SONGS, i);
                break;
            }
        }
    }
    compact_list_set_now_playing(group_songs_search_list, target);
}

/* ---- Playlists (Music submenu) --
 *
 * Favorites and Most Played (top 20 by play count, see metadata_db's
 * song_play_count table) are resolved fresh from the DB on every tap and
 * shown through show_group_songs() -- the exact same drill-down screen
 * Artists/Albums/Album Artist already use -- rather than a screen of their
 * own. Membership can change between visits (unlike Artists/Albums/Album
 * Artist, fixed at scan time), so these two are recomputed fresh every
 * time their row is tapped rather than cached. show_group_songs() takes an
 * owned copy of the small result set, so the DB-returned path array can be
 * released immediately.
 *
 * User-created .m3u playlists (Player screen's "Add to Playlist", or
 * dropped into MUSIC_ROOT_DIR/Playlists by hand) are listed below those
 * from that folder only. ---- */

#define MOST_PLAYED_LIMIT 20
#define RECENTLY_ADDED_LIMIT 50

/* Resolves each of paths[0..count) to a display title via a single-row DB
 * lookup (metadata_db_get_song_by_path()) -- falls back to the raw
 * basename if a path somehow isn't in the library anymore (a stale
 * favorite/playlist entry that outlived a rescan). Caller owns the
 * returned array (free() it); paths[] itself is untouched. Shared by
 * show_favorites()/show_most_played()/show_m3u_playlist() below -- these
 * lists are small (a hand-curated favorites list, MOST_PLAYED_LIMIT, one
 * playlist), so a DB lookup per song costs nothing next to loading the
 * whole library just to resolve a title. */
static group_song_entry_t * build_group_song_entries_from_paths(char ** paths, int count) {
    group_song_entry_t * entries = calloc((size_t) (count > 0 ? count : 1), sizeof(*entries));
    if (!entries) return NULL;
    for (int i = 0; i < count; i++) {
        entries[i].path = strdup(paths[i]);
        song_row_t row = {0};
        snprintf(row.path, sizeof(row.path), "%s", paths[i]);
        char title[384];
        if (metadata_db_get_song_by_path(paths[i], &row)) {
            format_song_identity(&row, title, sizeof(title));
        } else {
            snprintf(title, sizeof(title), "%s", basename_of(paths[i]));
        }
        entries[i].title = strdup(title);
        entries[i].song_id = row.id;
        entries[i].artwork_key = song_album_thumbnail_key(&row);
        if (!entries[i].path || !entries[i].title) {
            free_group_song_entries(entries, count);
            return NULL;
        }
    }
    return entries;
}

typedef struct {
    remote_collection_play_request_t request;
    pthread_t thread;
    atomic_bool cancel;
    atomic_bool done;
    bool ok;
    bool lazy;
    char error[96];
    metadata_db_snapshot_t *snapshot;
    int *title_ranks;
    char **paths;
    int count;
    uint64_t playback_epoch;
    int initial_play_mode;
    lv_timer_t *timer;
} remote_collection_job_t;

static remote_collection_job_t *remote_collection_job;

static bool remote_collection_cancelled(void *userdata) {
    remote_collection_job_t *job = userdata;
    return !job || atomic_load_explicit(&job->cancel, memory_order_acquire);
}

static void remote_collection_free_paths(char **paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static bool remote_collection_load_paths(remote_collection_job_t *job) {
    const remote_collection_play_request_t *request = &job->request;
    switch (request->scope) {
        case REMOTE_COLLECTION_FAVORITES:
            metadata_db_load_favorite_songs(&job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_RECENTLY_ADDED:
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_RECENTLY_PLAYED:
            metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX,
                                                   &job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_PLAYLIST: {
            if (!request->name[0] || strchr(request->name, '/') || strstr(request->name, "..")) {
                snprintf(job->error, sizeof(job->error), "%s", "Playlist could not be loaded");
                return false;
            }
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s.m3u", PLAYLISTS_DIR, request->name);
            if (n < 0 || (size_t)n >= sizeof(path) ||
                !file_browser_build_playlist_from_m3u(path, &job->paths, &job->count))
                return false;
            break;
        }
        default:
            return false;
    }
    if (remote_collection_cancelled(job)) return false;
    if (job->count <= 0 || !job->paths) {
        snprintf(job->error, sizeof(job->error), "%s", "Collection is empty or unavailable");
        return false;
    }
    return true;
}

static void *remote_collection_worker(void *userdata) {
    remote_collection_job_t *job = userdata;
    const remote_collection_play_request_t *request = &job->request;
    const char *artist = NULL, *album_artist = NULL, *album = NULL, *genre = NULL;
    switch (request->scope) {
        case REMOTE_COLLECTION_ALL:
            break;
        case REMOTE_COLLECTION_ALBUM:
            album = request->album[0] ? request->album : request->name;
            if (request->artist[0]) artist = request->artist;
            else album_artist = request->album_artist;
            if (!album[0]) {
                snprintf(job->error, sizeof(job->error), "%s", "Album identity is incomplete");
                goto done;
            }
            break;
        case REMOTE_COLLECTION_ARTIST:
            artist = request->artist[0] ? request->artist : request->name;
            break;
        case REMOTE_COLLECTION_ALBUM_ARTIST:
            album_artist = request->album_artist[0] ? request->album_artist : request->name;
            break;
        case REMOTE_COLLECTION_GENRE:
            genre = request->genre[0] ? request->genre : request->name;
            break;
        case REMOTE_COLLECTION_PLAYLIST:
        case REMOTE_COLLECTION_FAVORITES:
        case REMOTE_COLLECTION_RECENTLY_ADDED:
        case REMOTE_COLLECTION_RECENTLY_PLAYED:
            job->ok = remote_collection_load_paths(job);
            goto done;
        default:
            snprintf(job->error, sizeof(job->error), "%s", "Unsupported collection scope");
            goto done;
    }

    metadata_db_catalog_result_t result = metadata_db_filtered_snapshot_open(
        request->expected_revision, artist, album_artist,
        request->scope == REMOTE_COLLECTION_ALBUM && !artist, album, genre,
        &job->snapshot, &job->title_ranks, &job->count,
        remote_collection_cancelled, job);
    if (result == METADATA_DB_CATALOG_STALE)
        snprintf(job->error, sizeof(job->error), "%s", "Catalog changed before playback");
    else if (remote_collection_cancelled(job))
        snprintf(job->error, sizeof(job->error), "%s", "Playback request was cancelled");
    else if (result != METADATA_DB_CATALOG_OK)
        snprintf(job->error, sizeof(job->error), "%s", "Unable to prepare collection playback");
    else if (job->count <= 0)
        snprintf(job->error, sizeof(job->error), "%s", "Collection is empty or unavailable");
    else
        job->ok = true;
done:
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static void remote_collection_job_destroy(remote_collection_job_t *job) {
    if (!job) return;
    if (job->snapshot) metadata_db_snapshot_close(job->snapshot);
    free(job->title_ranks);
    remote_collection_free_paths(job->paths, job->count);
    free(job);
}

static void remote_collection_cancel_and_finish(void) {
    remote_collection_job_t *job = remote_collection_job;
    if (!job) return;
    atomic_store_explicit(&job->cancel, true, memory_order_release);
}

static void remote_collection_poll(lv_timer_t *timer) {
    remote_collection_job_t *job = remote_collection_job;
    if (!job || job->timer != timer) return;
    if (gui_player_playback_intent_epoch() != job->playback_epoch ||
        current_settings.play_mode != job->initial_play_mode)
        atomic_store_explicit(&job->cancel, true, memory_order_release);
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;

    bool context_current = !atomic_load_explicit(&job->cancel, memory_order_acquire) &&
        gui_player_playback_intent_epoch() == job->playback_epoch &&
        current_settings.play_mode == job->initial_play_mode;
    if (context_current && job->ok) {
        metadata_db_catalog_result_t check = metadata_db_try_catalog_revision_check(
            job->request.expected_revision);
        if (check == METADATA_DB_CATALOG_BUSY) return;
        context_current = check == METADATA_DB_CATALOG_OK;
        if (!context_current && check == METADATA_DB_CATALOG_STALE)
            snprintf(job->error, sizeof(job->error), "%s", "Catalog changed before playback");
        else if (!context_current)
            snprintf(job->error, sizeof(job->error), "%s", "Catalog is unavailable");
    }
    lv_timer_delete(timer);
    job->timer = NULL;
    pthread_join(job->thread, NULL);
    bool ok = job->ok && context_current;
    if (ok) {
        if (job->lazy) {
            ok = gui_player_play_lazy_collection(job->snapshot, job->title_ranks,
                                                  job->count, job->request.play_mode);
            if (ok) { job->snapshot = NULL; job->title_ranks = NULL; }
        } else {
            ok = gui_player_play_collection_paths(job->paths, job->count,
                                                  job->request.play_mode);
            if (ok) job->paths = NULL;
        }
    }
    if (!ok && !job->error[0]) {
        snprintf(job->error, sizeof(job->error), "%s",
                 context_current ? "Unable to start collection playback"
                                 : "Playback request was cancelled");
    }
    remote_collection_job = NULL;
    remote_control_finish_collection(ok, ok ? "" : job->error);
    remote_collection_job_destroy(job);
}

bool gui_library_play_remote_collection(const remote_collection_play_request_t *request) {
    bool catalog_scope = request && request->scope <= REMOTE_COLLECTION_GENRE;
    if (!request || remote_collection_job || request->play_mode < PLAY_MODE_SEQUENTIAL ||
        request->play_mode > PLAY_MODE_SHUFFLE_ONCE ||
        (catalog_scope && !request->expected_revision[0]) ||
        !memchr(request->expected_revision, '\0', sizeof(request->expected_revision)) ||
        !memchr(request->name, '\0', sizeof(request->name)) ||
        !memchr(request->artist, '\0', sizeof(request->artist)) ||
        !memchr(request->album_artist, '\0', sizeof(request->album_artist)) ||
        !memchr(request->album, '\0', sizeof(request->album)) ||
        !memchr(request->genre, '\0', sizeof(request->genre))) return false;
    remote_collection_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->request = *request;
    job->lazy = request->scope <= REMOTE_COLLECTION_GENRE;
    job->playback_epoch = gui_player_playback_intent_epoch();
    job->initial_play_mode = current_settings.play_mode;
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);
    remote_collection_job = job;
    job->timer = lv_timer_create(remote_collection_poll, 50, NULL);
    if (!job->timer) {
        remote_collection_job = NULL;
        remote_collection_job_destroy(job);
        return false;
    }
    if (pthread_create(&job->thread, NULL, remote_collection_worker, job) != 0) {
        lv_timer_delete(job->timer);
        job->timer = NULL;
        remote_collection_job = NULL;
        remote_collection_job_destroy(job);
        return false;
    }
    return true;
}

void gui_library_cancel_remote_collection(void) {
    if (remote_collection_job)
        atomic_store_explicit(&remote_collection_job->cancel, true, memory_order_release);
}

/* Plays a song selected via the remote-control web UI within its intended
 * scope (album, playlist, or artist). Falls back to full-library queue if
 * no scope is provided or resolved. */
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter) {
    if (!song_path || !song_path[0]) return;
    group_song_entry_t * scoped_entries = NULL;
    int scoped_count = 0;
    char scoped_title[128] = "";

    if (playlist_name[0] != '\0') {
        char ** paths = NULL;
        int count = 0;
        bool loaded = false;
        if (strcmp(playlist_name, "@favorites") == 0 || strcmp(playlist_name, "Favorites") == 0) {
            metadata_db_load_favorite_songs(&paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Favorites"));
        } else if (strcmp(playlist_name, "@most_played") == 0 || strcmp(playlist_name, "Most Played") == 0) {
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Most Played"));
        } else if (strcmp(playlist_name, "@recently_added") == 0 || strcmp(playlist_name, "Recently Added") == 0) {
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Recently Added"));
        } else if (strcmp(playlist_name, "@recently_played") == 0) {
            metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Recently Played"));
        } else {
            char m3u_path[512];
            snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, playlist_name);
            loaded = file_browser_build_playlist_from_m3u(m3u_path, &paths, &count);
        }
        if (loaded && count > 0) {
            scoped_entries = build_group_song_entries_from_paths(paths, count);
            scoped_count = scoped_entries ? count : 0;
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            if (scoped_title[0] == '\0') snprintf(scoped_title, sizeof(scoped_title), "%s", playlist_name);
        }
    } else if (album_filter[0] != '\0' && (artist_filter[0] != '\0' || album_artist_filter[0] != '\0')) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist_filter, album_filter, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            scoped_entries = calloc((size_t) count64, sizeof(*scoped_entries));
            song_row_t rows[64];
            while (scoped_entries && scoped_count < count64) {
                int want = (int) (count64 - scoped_count);
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist_filter, album_filter, NULL,
                                                               scoped_count, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    scoped_entries[scoped_count + i].path = strdup(rows[i].path);
                    scoped_entries[scoped_count + i].title = strdup(title);
                    scoped_entries[scoped_count + i].song_id = rows[i].id;
                    scoped_entries[scoped_count + i].artwork_key = song_album_thumbnail_key(&rows[i]);
                    if (!scoped_entries[scoped_count + i].path || !scoped_entries[scoped_count + i].title) {
                        free_group_song_entries(scoped_entries, (int) count64);
                        scoped_entries = NULL;
                        scoped_count = 0;
                        break;
                    }
                }
                if (!scoped_entries) break;
                scoped_count += got;
                if (got < want) break;
            }
            snprintf(scoped_title, sizeof(scoped_title), "%s", album_filter);
        }
    }

    if (scoped_entries && scoped_count > 0) {
        int pos = -1;
        for (int i = 0; i < scoped_count; i++) {
            if (strcmp(scoped_entries[i].path, song_path) == 0) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            char ** playlist_copy = malloc(sizeof(char *) * (size_t) scoped_count);
            for (int i = 0; i < scoped_count; i++) playlist_copy[i] = strdup(scoped_entries[i].path);
            set_player_source_group_songs_direct(scoped_entries, scoped_count, scoped_title, pos);
            on_file_selected(playlist_copy, scoped_count, pos);
            free_group_song_entries(scoped_entries, scoped_count);
            return;
        }
    }
    free_group_song_entries(scoped_entries, scoped_count);

    /* A caller that supplied a collection context asked for that exact
     * source. If it no longer resolves (for example after a rescan), do not
     * surprise them by replacing it with the entire library. */
    if ((playlist_name && playlist_name[0]) ||
        (artist_filter && artist_filter[0]) ||
        (album_artist_filter && album_artist_filter[0]) ||
        (album_filter && album_filter[0])) return;

    int64_t offset = metadata_db_get_song_title_offset(song_path);
    if (offset >= 0 && offset <= INT_MAX) {
        set_player_source_all_songs((int) offset);
        on_file_selected_lazy_all_songs((int) offset);
    }
}

/* Called from apply_track_metadata_to_ui() right after now_playing_path is
 * updated -- the single dispatch point that pushes it out to every now-
 * playing-aware list. Resolves the playing path's own tags via a single DB
 * lookup (metadata_db_get_song_by_path()), then each of Artists/Albums/
 * Album Artist/All Songs gets its own display offset via metadata_db_get_
 * group_offset()/metadata_db_get_song_title_offset() -- Artists/Albums/
 * Album Artist match by name (a whole group's songs share one row, so the
 * indicator lights up whichever artist/album/album-artist the CURRENT
 * TRACK belongs to, not just an exact-song match); All Songs matches by
 * exact song identity. Skips (rather than asserts on) any list that's
 * NULL -- Artists/Albums/Album Artist/All Songs are all built once at
 * startup so in practice this only ever matters before gui_init() finishes
 * building them, but there's no reason to depend on call-order here when a
 * simple guard covers it. */
/* The list row that shows the playing item: its row among search results
 * while searching (-1 if not among them), otherwise row plus the rows
 * above the items (a Play All row). */
static int now_playing_list_row(search_binding_id_t binding_id, int row, int prefix) {
    if (library_search_is_active(binding_id)) return library_search_find_filtered_row(binding_id, row);
    return row >= 0 ? row + prefix : -1;
}

static void apply_now_playing_indicator_result(const now_playing_lookup_result_t * result) {
    if (!result || strcmp(result->request.path, now_playing_path) != 0) return;
    int artist_row = result->artist_row;
    int album_row = result->album_row;
    int album_sorted_row = result->album_sorted_row;
    int album_artist_row = result->album_artist_row;
    int all_songs_row = result->all_songs_row;
    int recently_added_row = result->recently_added_row;
    if (artists_list) compact_list_set_now_playing(artists_list, now_playing_list_row(SEARCH_BINDING_ARTISTS, artist_row, 0));
    if (albums_list) {
        int display_row = library_search_showing_results(SEARCH_BINDING_ALBUMS)
            ? library_search_find_filtered_row(SEARCH_BINDING_ALBUMS, album_row)
            : album_sorted_row;
        compact_list_set_now_playing(albums_list, display_row);
    }
    if (album_artist_list)
        compact_list_set_now_playing(album_artist_list,
                                     now_playing_list_row(SEARCH_BINDING_ALBUM_ARTIST, album_artist_row, 0));
    /* All Songs and Recently Added start with a Play All row. */
    if (all_songs_list)
        compact_list_set_now_playing(all_songs_list, now_playing_list_row(SEARCH_BINDING_ALL_SONGS, all_songs_row, 1));
    if (recently_added_list)
        compact_list_set_now_playing(recently_added_list,
                                     now_playing_list_row(SEARCH_BINDING_RECENTLY_ADDED, recently_added_row, 1));

    refresh_group_songs_now_playing_indicator(); /* group_songs isn't compact_list-based -- see its own comment */
    pthread_mutex_lock(&artist_albums_state_mutex);
    bool same_artist_album_view = result->request.artist_albums_kind == artist_albums_current_kind &&
        strcmp(result->request.artist_albums_name, artist_albums_current_name) == 0;
    pthread_mutex_unlock(&artist_albums_state_mutex);
    if (artist_albums_list)
        compact_list_set_now_playing(artist_albums_list,
            same_artist_album_view && result->artist_albums_row > 0 &&
            result->artist_albums_row <= artist_albums_group_count
                ? result->artist_albums_row : -1);
}

void refresh_now_playing_indicators(void) {
    request_now_playing_indicator_lookup();
    /* These rows are already in memory, so keep their marker responsive while
     * the database-only lookup runs. */
    refresh_group_songs_now_playing_indicator();
    refresh_artist_albums_now_playing_indicator();
}

void gui_library_poll_now_playing_indicators(void) {
    now_playing_lookup_result_t result;
    bool result_valid = false;
    pthread_mutex_lock(&now_playing_lookup_mutex);
    bool candidate = library_ui_live && now_playing_lookup_result_ready &&
        now_playing_lookup_result.request.generation == now_playing_lookup_generation &&
        now_playing_lookup_result.request.lifetime == now_playing_lookup_lifetime;
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    if (!candidate) return;

    /* Keep the completed result queued while a rebuild owns the DB lock. The
     * try getter is bounded; the next UI poll can consume it after publication. */
    int32_t current_revision;
    if (!metadata_db_try_get_generation(&current_revision)) return;

    pthread_mutex_lock(&now_playing_lookup_mutex);
    bool ready = library_ui_live && now_playing_lookup_result_ready &&
        now_playing_lookup_result.request.generation == now_playing_lookup_generation &&
        now_playing_lookup_result.request.lifetime == now_playing_lookup_lifetime;
    if (ready) {
        result = now_playing_lookup_result;
        result_valid = now_playing_lookup_result_valid;
        now_playing_lookup_result_ready = false;
    }
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    if (ready && strcmp(result.request.path, now_playing_path) != 0) {
        request_now_playing_indicator_lookup();
        return;
    }
    if (ready && result.request.album_sort != requested_album_sort()) {
        request_now_playing_indicator_lookup();
        return;
    }
    if (ready && result.request.artist_albums_sort !=
                 requested_group_album_sort(result.request.artist_albums_kind)) {
        request_now_playing_indicator_lookup();
        return;
    }
    if (ready && (!result_valid || result.library_revision != current_revision ||
                  result.request.library_revision != result.library_revision)) {
        request_now_playing_indicator_lookup();
        return;
    }
    if (ready) {
        apply_now_playing_indicator_result(&result);
        pthread_mutex_lock(&now_playing_lookup_mutex);
        now_playing_lookup_applied_result = result;
        now_playing_lookup_applied_valid = true;
        pthread_mutex_unlock(&now_playing_lookup_mutex);
    }
}

static void show_favorites(void) {
    char ** paths;
    int count;
    metadata_db_load_favorite_songs(&paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs(TR("Favorites"), entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void show_most_played(void) {
    char ** paths;
    int count;
    metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs(TR("Most Played"), entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Unlike Favorites/Most Played above, Recently Added is not built through
 * show_group_songs() -- it's the whole library just reordered, potentially
 * far larger than either of those two's own small deliberately-capped
 * result sets, so it needs recently_added_screen's own paged compact_list
 * (build_recently_added_screen()) instead. Same "prebuilt screen, just
 * nav_push it" shape as open_queue_screen(). */
static void open_recently_added_screen(void) {
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
        (int) metadata_db_get_song_count() + 1);
    nav_push(recently_added_screen);
}

static char playlist_m3u_name[128];

/* The currently open M3U's title survives an in-place row removal. Song
 * paths and titles themselves live in group_songs_entries, independent of
 * the full-library arrays. */
/* Shows an .m3u's contents the same way Favorites/Most Played do -- a
 * tappable song list (show_group_songs_editable()), not straight into
 * playback -- rather than the old behavior of jumping directly to track 0
 * the moment the playlist row itself was tapped, matching every other entry
 * point into a group of songs elsewhere in this screen (Artists/Albums/
 * Album Artist all list first too). m3u_path is passed through as the
 * editable-playlist marker so the Edit UI (group_songs_edit_m3u_path,
 * see its own comment) only ever shows up here, not for Favorites/Most
 * Played/Artists/Albums. */
static void show_m3u_playlist(const char * name, const char * m3u_path, char ** paths, int count) {
    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (count && !entries) return;
    snprintf(playlist_m3u_name, sizeof(playlist_m3u_name), "%s", name);
    group_songs_refresh_source = GROUP_SONGS_REFRESH_M3U;
    snprintf(group_songs_refresh_m3u_path, sizeof(group_songs_refresh_m3u_path), "%s", m3u_path ? m3u_path : "");
    show_group_songs_editable(playlist_m3u_name, entries, count, m3u_path, false);
    free_group_song_entries(entries, count);
}

/* Reload an editable playlist after a checked edit. Re-reads the underlying
 * .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void reload_edited_playlist(void) {
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(group_songs_edit_m3u_path, &songs, &count)) {
        group_songs_anchor_pending = false;
        show_error_toast(TR("Cannot read playlist")); return;
    }
    group_song_entry_t * entries = count ? build_group_song_entries_from_paths(songs, count) : NULL;
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
    if (count && !entries) { group_songs_anchor_pending = false; return; }
    group_songs_file_stat_valid = stat(group_songs_edit_m3u_path, &group_songs_file_stat) == 0;
    set_group_songs_entries(entries, count);
    group_songs_drag_original_child_count = 0;
    group_songs_drag_source_id = -1;
    if (!group_songs_reset_selection()) {
        group_songs_edit_mode = false;
        group_songs_clear_selection();
        show_error_toast(TR("Cannot select playlist entries"));
    }
    if (group_songs_anchor_pending) {
        if (count > 0 && group_songs_anchor_index >= count) group_songs_anchor_index = count - 1;
        if (count > 0 && group_songs_anchor_index >= 0)
            group_songs_page_start = (group_songs_anchor_index / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    free_group_song_entries(entries, count);
    populate_group_songs_rows();
}

/* A missing Playlists folder is recreated so the user always has one to use;
 * a read-only card simply fails the mkdir. Callers check the card is mounted
 * so an empty mount point never gets one. */
static void ensure_playlists_dir(void) {
    if (mkdir(PLAYLISTS_DIR, 0755) != 0 && errno != EEXIST)
        DB_LOG("DB", "playlists_mkdir_failed errno=%d", errno);
}

static void library_playlist_open_row(gui_playlist_row_kind_t kind, const char * name,
                                      const char * path, char ** songs, int count) {
    switch (kind) {
        case GUI_PLAYLIST_ROW_FAVORITES: show_favorites(); break;
        case GUI_PLAYLIST_ROW_MOST_PLAYED: show_most_played(); break;
        case GUI_PLAYLIST_ROW_QUEUE: open_queue_screen(); break;
        case GUI_PLAYLIST_ROW_RECENTLY_ADDED: open_recently_added_screen(); break;
        case GUI_PLAYLIST_ROW_SAVED_M3U: show_m3u_playlist(name, path, songs, count); break;
    }
}

static void library_reload_edited_playlist_if_changed(lv_obj_t * screen) {
    if (screen == group_songs_screen && group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        struct stat st;
        if (stat(group_songs_edit_m3u_path, &st) == 0) {
            int32_t scroll = lv_obj_get_scroll_y(group_songs_list);
            reload_edited_playlist();
            lv_obj_scroll_to_y(group_songs_list, scroll, LV_ANIM_OFF);
        }
    }
}

/* ---- CUE sheet track list (File Browser -> tap a .cue) ----
 * External and embedded sheets produce stable virtual queue entries.
 * Each entry keeps its own metadata and bounded playback timeline. */
static cue_sheet_t current_cue_sheet;
static bool current_cue_sheet_valid = false;
static char current_cue_source_path[PATH_MAX];
static char current_cue_source_dir[PATH_MAX];
static int current_cue_source_row = -1;

static void cue_track_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (!current_cue_sheet_valid || index < 0 || index >= current_cue_sheet.track_count) return;

    int count = current_cue_sheet.track_count;
    char ** paths = malloc(sizeof(char *) * (size_t) count);
    if (!paths) return;
    int copied = 0;
    for (; copied < count; copied++) {
        char reference[PATH_MAX];
        if (!cue_reference_format(current_cue_source_path, (unsigned) copied + 1,
                                  reference, sizeof(reference))) break;
        paths[copied] = strdup(reference);
        if (!paths[copied]) break;
    }
    if (copied != count) {
        for (int i = 0; i < copied; i++) free(paths[i]);
        free(paths);
        show_error_toast(TR("Not enough memory to load CUE tracks"));
        return;
    }
    set_player_source_file_browser(current_cue_source_dir, current_cue_source_row);
    on_file_selected(paths, count, index);
}

static void populate_cue_tracks_screen(void) {
    lv_obj_clean(cue_tracks_list);
    for (int i = 0; i < current_cue_sheet.track_count; i++) {
        cue_track_t * t = &current_cue_sheet.tracks[i];
        char label[160];
        /* Falls back to the plain track number when a sheet doesn't set
         * TITLE for a track (rare but real -- some auto-generated sheets
         * only carry INDEX times) rather than showing an empty row. */
        if (t->title[0]) {
            snprintf(label, sizeof(label), "%d. %s", t->number, t->title);
        } else {
            snprintf(label, sizeof(label), TR("Track %d"), t->number);
        }
        lv_obj_t * row = build_wide_list_row(cue_tracks_list, label, false);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cue_track_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_cue_tracks_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen(TR("Tracks"), &cue_tracks_title_label, &cue_tracks_list);
    lv_obj_set_flex_align(cue_tracks_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return scr;
}

/* file_browser.h's on_cue_select callback -- see file_browser_create()'s own
 * comment. Parses fresh on every tap (a .cue sheet is tiny, no reason to
 * cache) and replaces whatever sheet this screen was last showing. */
void on_cue_file_selected(const char * cue_path) {
    cue_sheet_free(&current_cue_sheet);
    const char * ext = strrchr(cue_path, '.');
    current_cue_sheet_valid = ext && strcasecmp(ext, ".cue") == 0
        ? cue_parse_file(cue_path, &current_cue_sheet)
        : cue_parse_embedded(cue_path, &current_cue_sheet);
    if (!current_cue_sheet_valid) {
        show_error_toast(TR("Couldn't read this .cue file"));
        return;
    }
    snprintf(current_cue_source_path, sizeof(current_cue_source_path), "%s", cue_path);
    snprintf(current_cue_source_dir, sizeof(current_cue_source_dir), "%s", file_browser_get_last_selected_dir());
    current_cue_source_row = file_browser_get_last_selected_row();
    lv_label_set_text(cue_tracks_title_label, current_cue_sheet.title[0]
                      ? current_cue_sheet.title : basename_of(current_cue_sheet.audio_path));
    populate_cue_tracks_screen();
    nav_push(cue_tracks_screen);
}

/* Settings > Update Music Database: reruns library_scan_once() (a full
 * rescan + tag re-read of MUSIC_ROOT_DIR, same as gui_init's startup call)
 * on a background thread -- a real library's tag reads are too slow to do
 * on the UI thread, same reasoning as every other pthread_create() in this
 * file -- then rebuilds the four prebuilt library screens (All Songs,
 * Artists, Albums, Album Artist), since unlike group_songs_screen/
 * artist_albums_screen (which rebuild their rows on every visit) these are
 * only ever built once at startup: each one's compact_list_set_paged_
 * provider() call captured a total_count at that build time, which a
 * rescan can make stale (more/fewer songs, artists, albums), so the whole
 * screen is rebuilt to pick up a fresh count rather than trying to patch
 * one in place. Playlists isn't one of these four -- see its own build_
 * playlists_screen()/populate_playlists_screen() comment for why it
 * doesn't need rebuilding here. */
typedef enum {
    LIBRARY_RESCAN_NORMAL,
    LIBRARY_RESCAN_FORCE_METADATA,
    LIBRARY_RESCAN_TARGETED,
    LIBRARY_RESCAN_REFRESH_COVERS_ALL,
    LIBRARY_RESCAN_REFRESH_COVERS_ALBUM
} library_rescan_mode_t;

static pthread_t library_rescan_thread;
bool library_rescan_active = false;
static void (*library_setup_scan_complete)(bool success);
bool library_rescan_success_pending = false;
static atomic_bool library_rescan_done_flag = false;
static library_rescan_mode_t library_rescan_mode;
static gui_popup_t refresh_all_metadata_popup;
static gui_popup_t refresh_all_covers_popup;
/* Published to the UI by library_rescan_done_flag's release/acquire pair. */
/* Set when the walk could not read part of the tree, so the toast does not
 * claim a full update. Rows whose file is missing are still removed. */
static const char library_recovered_message[] = N_("Library recovered. Use Settings > Update Music Database to save");
static void hide_collection_menu(void);
static void refresh_targeted_nested_views(void);
static void rescan_playlists(void);

static void library_scan_copy_result(library_scan_result_t *out) {
    if (!out) return;
    library_scan_get_result(out);
}

/* Cache reports are owned by the UI thread. */
static metadata_db_load_outcome_t library_cache_load_outcome;
static bool library_cache_load_pending;
/* "Library loaded" is shown only for an SD hot swap, not at boot. */
static bool library_cache_load_announce_loaded;
static uint32_t library_cache_load_card_generation;
static uint32_t library_cache_load_generation;
static uint32_t library_prompt_card_generation;
static uint32_t library_boot_ready_tick;
static bool library_boot_ready_tick_set;
static bool boot_warmup_requested;
static bool boot_warmup_pending;

typedef enum {
    LIBRARY_PROMPT_NONE,
    LIBRARY_PROMPT_NO_DATABASE,
    LIBRARY_PROMPT_LOAD_FAILED,
    LIBRARY_PROMPT_FILES_CHANGED,
} library_prompt_reason_t;

typedef enum {
    LIBRARY_PROMPT_REQUEST_NONE,
    LIBRARY_PROMPT_REQUEST_PENDING,
    LIBRARY_PROMPT_REQUEST_VISIBLE,
} library_prompt_request_state_t;

typedef struct {
    library_prompt_reason_t reason;
    library_prompt_request_state_t state;
    uint32_t card_generation;
    uint32_t load_generation;
} library_prompt_request_t;

typedef enum {
    LIBRARY_PROMPT_CARD_CURRENT,
    LIBRARY_PROMPT_CARD_CHANGED,
    LIBRARY_PROMPT_CARD_UNKNOWN,
} library_prompt_card_status_t;

static library_prompt_request_t library_prompt_request;
static library_prompt_request_t library_prompt_queued_request;
static gui_popup_t library_update_prompt_popup;
static lv_obj_t * library_update_prompt_title;
static lv_obj_t * library_update_prompt_body;
static lv_obj_t * library_update_prompt_confirm_label;

static library_prompt_card_status_t library_prompt_card_status(void);
static void library_prompt_prepare_cache_load_card(void);
static void library_update_prompt_confirm_cb(lv_event_t * e);
static void library_update_prompt_later_cb(lv_event_t * e);
static void library_update_prompt_backdrop_cb(lv_event_t * e);
static void library_prompt_requeue_visible(void);

static bool library_prompt_request_is_current(const library_prompt_request_t * request) {
    if (!request || request->state == LIBRARY_PROMPT_REQUEST_NONE ||
        request->card_generation != library_prompt_card_generation) return false;
    if (request->reason == LIBRARY_PROMPT_FILES_CHANGED) return true;
    if (request->load_generation != library_cache_load_generation) return false;
    if (request->reason == LIBRARY_PROMPT_NO_DATABASE)
        return library_cache_load_outcome == METADATA_DB_LOAD_SUCCESS_FRESH;
    if (request->reason == LIBRARY_PROMPT_LOAD_FAILED)
        return library_cache_load_outcome == METADATA_DB_LOAD_FAILED;
    return false;
}

static void library_prompt_clear_request(library_prompt_request_t * request) {
    memset(request, 0, sizeof(*request));
}

static void library_prompt_activate_queued_request(void) {
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
        library_prompt_queued_request.state == LIBRARY_PROMPT_REQUEST_NONE) return;
    library_prompt_request = library_prompt_queued_request;
    library_prompt_clear_request(&library_prompt_queued_request);
}

static void library_prompt_queue_request(const library_prompt_request_t * request) {
    if (library_prompt_queued_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
        request->reason != LIBRARY_PROMPT_FILES_CHANGED) return;
    library_prompt_queued_request = *request;
}

static void library_prompt_enqueue(library_prompt_reason_t reason, uint32_t card_generation) {
    library_prompt_request_t request = {
        .reason = reason,
        .state = LIBRARY_PROMPT_REQUEST_PENDING,
        .card_generation = card_generation,
        .load_generation = library_cache_load_generation,
    };
    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
            reason == LIBRARY_PROMPT_FILES_CHANGED) return;
        library_prompt_queue_request(&request);
    } else if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_PENDING &&
               library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
               reason != LIBRARY_PROMPT_FILES_CHANGED) {
        library_prompt_queue_request(&request);
    } else {
        library_prompt_request = request;
        library_prompt_clear_request(&library_prompt_queued_request);
    }
}

static void library_prompt_drop_all(void) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_clear_request(&library_prompt_queued_request);
}

void gui_library_invalidate_boot_prompt(void) {
    library_prompt_drop_all();
}

void gui_library_suspend_boot_prompt(void) {
    library_prompt_requeue_visible();
}

static void library_prompt_card_changed(void) {
    library_prompt_card_generation++;
    boot_warmup_pending = false;
    library_prompt_drop_all();
}

void gui_library_request_files_changed_prompt(void) {
    library_prompt_card_status_t card_status = library_prompt_card_status();
    if (card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    library_prompt_enqueue(LIBRARY_PROMPT_FILES_CHANGED, library_prompt_card_generation);
}

static bool library_prompt_has_manual_overlay(void) {
    return library_az_index_overlay_visible() ||
           (playlist_start_popup && !lv_obj_has_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_start_backdrop && !lv_obj_has_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           gui_playlists_menu_visible() ||
           (collection_menu_popup && !lv_obj_has_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (collection_menu_backdrop && !lv_obj_has_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_popup && !lv_obj_has_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_backdrop &&
            !lv_obj_has_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           gui_folder_actions_menu_visible();
}

#define BOOT_SETTLE_MS 8000
static bool library_prompt_environment_idle(void) {
    if (!current_settings.setup_complete || !library_boot_ready_tick_set || lv_tick_elaps(library_boot_ready_tick) < BOOT_SETTLE_MS ||
        !backlight_screen_is_on() || gui_navigation_transition_in_progress() ||
        lv_screen_active() == gui_busy_get_screen() || gui_library_navigation_blocked() ||
        gui_network_boot_prompt_blocked() || gui_shell_boot_prompt_blocked() ||
        gui_lock_screen_is_showing() || gui_player_boot_prompt_blocked() ||
        gui_queue_boot_prompt_blocked() || library_prompt_has_manual_overlay() ||
        gui_notifications_toast_visible()) return false;

    lv_obj_t * active = lv_screen_active();
    if (t9_keypad_is_inline_active() ||
        (gui_text_input_get_screen() && active == gui_text_input_get_screen())) return false;

    return gui_popup_visible_count() == 0;
}

/* A visible prompt yields only to something that takes over the screen.
 * Touches on the prompt itself latch the shell's gesture candidates, and a
 * toast can appear under it, so the settle checks above do not apply. */
static bool library_prompt_taken_over(void) {
    return !backlight_screen_is_on() || lv_screen_active() == gui_busy_get_screen() ||
           gui_lock_screen_is_showing() || gui_network_boot_prompt_blocked() ||
           gui_popup_visible_count() != 1;
}

static void library_prompt_update_labels(library_prompt_reason_t reason) {
    switch (reason) {
        case LIBRARY_PROMPT_NO_DATABASE:
            lv_label_set_text(library_update_prompt_title, TR("No music database"));
            lv_label_set_text(library_update_prompt_body,
                              TR("Build it now? Large libraries can take several minutes."));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Build"));
            break;
        case LIBRARY_PROMPT_LOAD_FAILED:
            lv_label_set_text(library_update_prompt_title, TR("Music database unavailable"));
            lv_label_set_text(library_update_prompt_body, TR("It could not be loaded. Rebuild it now?"));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Rebuild"));
            break;
        case LIBRARY_PROMPT_FILES_CHANGED:
            lv_label_set_text(library_update_prompt_title, TR("Update music database?"));
            lv_label_set_text(library_update_prompt_body, TR("Files on the card may have changed."));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Update"));
            break;
        case LIBRARY_PROMPT_NONE:
            break;
    }
}

static void library_update_prompt_close(bool show_settings_toast) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_activate_queued_request();

    if (!show_settings_toast) return;
    show_info_toast(TR("Use Settings > Update Music Database"));
}

/* The settle checks only decide when to show the prompt. Once the user taps
 * the action, revalidate what makes the scan unsafe or stale; the tap's own
 * press still has the shell's gesture candidates latched at this point. */
static bool library_prompt_can_run_action(const library_prompt_request_t * request) {
    return library_prompt_request_is_current(request) &&
           library_prompt_card_status() == LIBRARY_PROMPT_CARD_CURRENT &&
           !gui_library_navigation_blocked() && !gui_network_usb_prompt_invalidated();
}

static void library_update_prompt_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        !gui_popup_is_visible(&library_update_prompt_popup)) return;
    library_prompt_request_t handled = library_prompt_request;
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    if (!library_prompt_can_run_action(&handled)) {
        library_prompt_card_status_t card_status = library_prompt_card_status();
        if (card_status == LIBRARY_PROMPT_CARD_CHANGED) library_prompt_card_changed();
        else if (gui_library_navigation_blocked())
            library_prompt_drop_all();
        else if (library_prompt_request_is_current(&handled)) {
            handled.state = LIBRARY_PROMPT_REQUEST_PENDING;
            library_prompt_request = handled;
        }
        return;
    }
    if (handled.reason == LIBRARY_PROMPT_LOAD_FAILED) start_library_rescan();
    else start_library_auto_rescan();
}

static void library_update_prompt_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

static void library_update_prompt_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

static void build_library_update_prompt_popup(void) {
    library_update_prompt_popup.popup = build_confirm_popup_with_labels(
        "", LV_LABEL_LONG_WRAP, &library_update_prompt_title, "", &library_update_prompt_body,
        "", &library_update_prompt_confirm_label, accent_lv_color(), library_update_prompt_confirm_cb, NULL,
        TR("Later"), lv_color_white(), library_update_prompt_later_cb, NULL,
        library_update_prompt_backdrop_cb, &library_update_prompt_popup.backdrop);
}

static void library_prompt_requeue_visible(void) {
    if (!gui_popup_is_visible(&library_update_prompt_popup)) return;
    gui_popup_hide(&library_update_prompt_popup);
    if (library_prompt_request_is_current(&library_prompt_request))
        library_prompt_request.state = LIBRARY_PROMPT_REQUEST_PENDING;
    else
        library_prompt_clear_request(&library_prompt_request);
}

void gui_library_boot_ready(uint32_t tick) {
    if (library_boot_ready_tick_set) return;
    library_boot_ready_tick = tick;
    library_boot_ready_tick_set = true;
}

void gui_library_poll_boot_prompt(void) {
    bool has_request = library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
                       library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE;
    library_prompt_card_status_t card_status = LIBRARY_PROMPT_CARD_CURRENT;
    if (has_request || boot_warmup_pending) card_status = library_prompt_card_status();
    if ((has_request || boot_warmup_pending) && card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    if (card_status == LIBRARY_PROMPT_CARD_UNKNOWN) {
        library_prompt_requeue_visible();
        return;
    }
    if (has_request && gui_network_usb_prompt_invalidated()) {
        gui_library_suspend_boot_prompt();
        return;
    }
    if (has_request && gui_library_navigation_blocked()) {
        library_prompt_drop_all();
        return;
    }

    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_request)) {
        gui_popup_hide(&library_update_prompt_popup);
        library_prompt_clear_request(&library_prompt_request);
    }
    if (library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_queued_request))
        library_prompt_clear_request(&library_prompt_queued_request);
    library_prompt_activate_queued_request();

    if (boot_warmup_pending && library_prompt_environment_idle()) {
        int64_t song_count = metadata_db_get_song_count();
        library_thumbnail_status_t thumb_status;
        library_thumbnails_get_status(&thumb_status);
        if (song_count <= 0 || thumb_status.warmup_active) {
            boot_warmup_pending = false;
        } else if (!thumb_status.lazy_active) {
            boot_warmup_pending = false;
            library_thumbnails_start_warmup();
        }
    }

    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (!gui_popup_is_visible(&library_update_prompt_popup) ||
            library_prompt_taken_over()) library_prompt_requeue_visible();
        return;
    }
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_PENDING ||
        !library_update_prompt_popup.popup || !library_prompt_environment_idle()) return;

    library_prompt_update_labels(library_prompt_request.reason);
    library_prompt_request.state = LIBRARY_PROMPT_REQUEST_VISIBLE;
    gui_popup_show(&library_update_prompt_popup);
}

/* Initialized before pthread_create and published by library_rescan_done_flag. */
static metadata_db_load_outcome_t reload_library_cache(void);

static void report_library_cache_load(void) {
    if (!library_cache_load_pending) return;
    library_cache_load_pending = false;
    switch (library_cache_load_outcome) {
        case METADATA_DB_LOAD_FAILED:
            library_prompt_enqueue(LIBRARY_PROMPT_LOAD_FAILED, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_UNMOUNTED: show_error_toast(TR("No SD card")); break;
        case METADATA_DB_LOAD_SUCCESS_RECOVERED: show_info_toast(TR(library_recovered_message)); break;
        case METADATA_DB_LOAD_SUCCESS_FRESH:
            library_prompt_enqueue(LIBRARY_PROMPT_NO_DATABASE, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_SUCCESS_NORMAL:
            if (library_cache_load_announce_loaded) show_info_toast(TR("Library loaded"));
            break;
    }
}

/* Keep the metadata scan worker at its existing background priority. The
 * thumbnail component has its own private copy for its three independent
 * worker lifetimes. */
static void library_worker_lower_priority(int nice_value) {
#ifdef __linux__
    errno = 0;
    int inherited = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && inherited < nice_value)
        (void)setpriority(PRIO_PROCESS, 0, nice_value);
#else
    (void)nice_value;
#endif
}

static void * library_rescan_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef __linux__
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_BATCH, &sp);
    library_worker_lower_priority(5);
#endif
    (void)arg;
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    DB_LOG("DB", "rescan_thread_begin rss_kb=%ld", db_log_rss_kb());
    library_scan_run();
    DB_LOG("DB", "rescan_thread_end songs=%lld elapsed_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - started_ms),
           db_log_rss_kb());
    atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

/* Controls automatic library rescanning on boot / SD mount. */
#define GUI_LIBRARY_AUTO_RESCAN_ENABLED true
bool gui_library_auto_rescan_enabled(void) {
    return current_settings.setup_complete && GUI_LIBRARY_AUTO_RESCAN_ENABLED;
}

/* 4MB stack size configured for library rescan thread to accommodate deep
 * call chains through metadata parsers and tagcache indexing. */
#define LIBRARY_RESCAN_THREAD_STACK_SIZE (4 * 1024 * 1024)

static void library_rescan_free_target_paths(void) {
    library_scan_release();
}

static bool start_library_rescan_job(bool allow_rebuild, library_rescan_mode_t mode,
                                     char ** target_paths, int target_count,
                                     library_scan_scope_t scope, const char * scope_name,
                                     const char * scope_album_artist) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    /* Ignore request if a rescan is already running. */
    if (library_rescan_active) {
        for (int i = 0; i < target_count; i++) free(target_paths[i]);
        free(target_paths);
        return false;
    }
    DB_LOG("DB", "rescan_requested existing_songs=%lld rss_kb=%ld",
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
    /* A metadata parser child is already a meaningful peak on this 56 MiB
     * target. Do not overlap it with a previous warmer or lazy cover decode;
     * cancellation is cooperative and bounded by the artwork timeout. */
    library_thumbnails_quiesce();
    library_rescan_mode = mode;
    library_scan_request_t scan_request = {
        .mode = mode == LIBRARY_RESCAN_FORCE_METADATA ? LIBRARY_SCAN_MODE_FORCE_METADATA
             : mode == LIBRARY_RESCAN_TARGETED ? LIBRARY_SCAN_MODE_TARGETED : LIBRARY_SCAN_MODE_NORMAL,
        .allow_rebuild = allow_rebuild,
        .target_paths = target_paths,
        .target_count = target_count,
        .scope = scope,
        .scope_name = scope_name,
        .scope_album_artist = scope_album_artist,
        .outcome_seed = !allow_rebuild && library_cache_load_pending ? library_cache_load_outcome
                                                                    : METADATA_DB_LOAD_SUCCESS_NORMAL,
        .books_rescan = gui_books_rescan,
        .playlists_rescan = rescan_playlists,
    };
    library_scan_reset_cancel();
    library_scan_prepare(&scan_request);
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;
    const char * busy_title = mode == LIBRARY_RESCAN_FORCE_METADATA ? TR("Refreshing\nall metadata...")
        : mode == LIBRARY_RESCAN_TARGETED ? TR("Refreshing\nmetadata...")
        : TR("Updating\nmusic database...");
    const char * busy_detail = mode == LIBRARY_RESCAN_TARGETED || mode == LIBRARY_RESCAN_FORCE_METADATA
            ? TR("Preparing metadata refresh...") : TR("Preparing music library...");
    library_rescan_token = gui_busy_show(busy_title, busy_detail);
    gui_busy_set_progress(library_rescan_token, 0);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, LIBRARY_RESCAN_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&library_rescan_thread, attr_ptr, library_rescan_thread_func, NULL) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (created) library_cache_load_pending = false;
    if (!created) {
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        library_scan_record_start_failure();
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast(TR("Thread launch failed"));
    }
    return created;
}

static void start_library_rescan_with_repair(bool allow_rebuild) {
    (void) start_library_rescan_job(allow_rebuild, LIBRARY_RESCAN_NORMAL, NULL, 0,
                                   LIBRARY_SCAN_SCOPE_NONE, NULL, NULL);
}

void start_library_rescan(void) {
    start_library_rescan_with_repair(true);
}

void start_library_auto_rescan(void) {
    start_library_rescan_with_repair(false);
}

void library_scan_once(void) {
    library_scan_run_full_current(gui_books_rescan, rescan_playlists);
}

bool gui_library_start_setup_scan(void (*on_complete)(bool success)) {
    if (!on_complete || library_setup_scan_complete || library_rescan_active ||
        library_rescan_success_pending || sd_format_active) return false;
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_NORMAL, NULL, 0,
                                 LIBRARY_SCAN_SCOPE_NONE, NULL, NULL)) return false;
    library_setup_scan_complete = on_complete;
    return true;
}

static void finish_setup_scan(bool success) {
    void (*done)(bool) = library_setup_scan_complete;
    library_setup_scan_complete = NULL;
    if (done) done(success);
}

static int copy_scan_target_paths(const char * const * paths, int count, char *** out_paths) {
    return metadata_refresh_copy_unique_paths(paths, count, out_paths);
}

void start_library_metadata_refresh_paths(const char * const * paths, int count) {
    char ** copied = NULL;
    int unique_count = copy_scan_target_paths(paths, count, &copied);
    if (unique_count <= 0) {
        show_error_toast(TR("No songs to refresh"));
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, copied, unique_count,
                                  LIBRARY_SCAN_SCOPE_PATHS, NULL, NULL))
        show_info_toast(TR("Library is busy"));
}

void start_library_metadata_refresh_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_info_toast(TR("No songs to refresh"));
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0,
                                  LIBRARY_SCAN_SCOPE_ALBUM, album, album_artist))
        show_info_toast(TR("Library is busy"));
}

void start_library_metadata_refresh_artist(metadata_db_group_kind_t kind, const char * name) {
    if (!name || !name[0] || (kind != METADATA_DB_GROUP_ARTIST &&
                              kind != METADATA_DB_GROUP_ALBUM_ARTIST)) {
        show_info_toast(TR("No songs to refresh"));
        return;
    }
    library_scan_scope_t scope = kind == METADATA_DB_GROUP_ALBUM_ARTIST
        ? LIBRARY_SCAN_SCOPE_ALBUM_ARTIST : LIBRARY_SCAN_SCOPE_ARTIST;
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0, scope, name, NULL))
        show_info_toast(TR("Library is busy"));
}

static void refresh_all_metadata_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_metadata_popup);
    (void) start_library_rescan_job(true, LIBRARY_RESCAN_FORCE_METADATA, NULL, 0,
                                   LIBRARY_SCAN_SCOPE_NONE, NULL, NULL);
}

static void refresh_all_metadata_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_metadata_popup);
}

static void refresh_all_metadata_backdrop_cb(lv_event_t * e) {
    refresh_all_metadata_cancel_cb(e);
}

static void artwork_failure_cache_clear_album(const char * album, const char * album_artist) {
    song_row_t songs[64];
    int offset = 0;
    for (;;) {
        int count = metadata_db_get_album_songs(album, album_artist, offset, songs,
                                                 (int) (sizeof(songs) / sizeof(songs[0])));
        if (count <= 0) break;
        for (int i = 0; i < count; i++) artwork_failure_cache_note_success(songs[i].id);
        offset += count;
        if (count < (int) (sizeof(songs) / sizeof(songs[0]))) break;
    }
}

static bool library_cover_reload_storage_available(void) {
    if (!metadata_db_storage_current()) return false;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return false;
#endif
    return true;
}

static bool start_library_cover_reload_job(bool all_albums, const song_row_t * target_song,
                                           const char * album, const char * album_artist_key) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    if (library_rescan_active) {
        show_info_toast(TR("Library is busy"));
        return false;
    }
    if (!all_albums && (!target_song || !album || !album_artist_key)) {
        show_error_toast(TR("Album unavailable"));
        return false;
    }
    if (!library_cover_reload_storage_available()) {
        show_error_toast(TR("No SD card"));
        return false;
    }

    uint64_t target_key = 0;
    int64_t target_song_id = 0;
    if (!all_albums) {
        target_key = song_album_thumbnail_key(target_song);
        target_song_id = target_song->id;
    }

    library_thumbnails_quiesce();
    gui_player_begin_cover_reload();

    library_rescan_mode = all_albums ? LIBRARY_RESCAN_REFRESH_COVERS_ALL
                                     : LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
    library_scan_reset_cancel();
    library_rescan_active = true;

    if (all_albums) {
        library_thumbnails_clear_cache();
    } else {
        /* Any artist may alias this album or hold a "no art" entry that it
         * could now resolve; those records are cheap to re-derive. */
        library_thumbnails_invalidate_album(target_key);
        artwork_failure_cache_clear_album(album, album_artist_key);
    }
    gui_player_invalidate_cover_negatives();

    library_thumbnail_cover_request_t request = {
        .scope = all_albums ? LIBRARY_THUMB_COVER_ALL : LIBRARY_THUMB_COVER_ALBUM,
        .target_song_id = target_song_id
    };
    bool created = library_thumbnails_start_cover_reload(&request, library_thumbnail_show_busy);
    if (!created) {
        library_rescan_active = false;
        gui_player_finish_cover_reload(); /* releases any held player request */
        gui_busy_hide(library_rescan_token);
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast(TR("Thread launch failed"));
    }
    return created;
}

static void start_library_cover_reload_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_error_toast(TR("Album unavailable"));
        return;
    }
    song_row_t target_song;
    if (metadata_db_get_album_songs(album, album_artist, 0, &target_song, 1) != 1) {
        show_error_toast(TR("Album unavailable"));
        return;
    }
    (void) start_library_cover_reload_job(false, &target_song, album, album_artist);
}

static void start_library_cover_reload_all(void) {
    (void) start_library_cover_reload_job(true, NULL, NULL, NULL);
}

static void refresh_all_covers_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_covers_popup);
    start_library_cover_reload_all();
}

static void refresh_all_covers_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_covers_popup);
}

static void refresh_all_covers_backdrop_cb(lv_event_t * e) {
    refresh_all_covers_cancel_cb(e);
}

void show_library_refresh_all_covers_prompt(void) {
    if (!refresh_all_covers_popup.popup) {
        refresh_all_covers_popup.popup = build_confirm_popup(
            TR("Refresh all covers?"), LV_LABEL_LONG_WRAP, NULL,
            TR("Removes saved covers and extracts them again. This may take a while."),
            TR("Refresh"), accent_lv_color(), refresh_all_covers_confirm_cb, NULL,
            TR("Cancel"), accent_lv_color(), refresh_all_covers_cancel_cb, NULL,
            refresh_all_covers_backdrop_cb, &refresh_all_covers_popup.backdrop);
    }
    gui_popup_show(&refresh_all_covers_popup);
}

void show_library_refresh_all_metadata_prompt(void) {
    if (!refresh_all_metadata_popup.popup) {
        refresh_all_metadata_popup.popup = build_confirm_popup(
            TR("Refresh all metadata?"), LV_LABEL_LONG_WRAP, NULL,
            TR("Reads the tags of every song again. This may take a while."),
            TR("Refresh"), accent_lv_color(), refresh_all_metadata_confirm_cb, NULL,
            TR("Cancel"), accent_lv_color(), refresh_all_metadata_cancel_cb, NULL,
            refresh_all_metadata_backdrop_cb, &refresh_all_metadata_popup.backdrop);
    }
    gui_popup_show(&refresh_all_metadata_popup);
}

/* Rebuilds the five prebuilt library screens (All Songs, Artists, Albums,
 * Album Artist, Recently Added) so each one's compact_list_set_paged_
 * provider() call recaptures a fresh total_count against the just-reloaded
 * library --
 * unlike group_songs_screen/artist_albums_screen (which rebuild their rows
 * on every visit) these are only ever built once at startup, so a stale
 * cached count would otherwise persist after ANY reload of the underlying
 * data, not just a full rescan. Shared by poll_library_rescan()'s
 * background-scan-completion path below and reload_library_on_sd_reinsert()'s
 * fast-cache-load path further down, both of which replace that data via a
 * different route (library_scan_once() vs library_load_from_cache_only())
 * but need the exact same screen-side cleanup afterward. playlists_screen
 * deliberately NOT rebuilt here -- see poll_library_rescan()'s own former
 * comment on why (still applies verbatim): its content is recomputed fresh
 * on every visit already, never stale to begin with. */
/* Targeted metadata refresh: the list screens stay alive and only re-read
 * their DB-backed pages. Deleting and rebuilding them here (the full-scan
 * path) is unsafe for a refresh started from inside the library: the busy
 * screen pops back into a library screen, and a back swipe during the save
 * can leave one of the replaced screens active, so a later redraw touched a
 * freed list. Scroll positions are kept; search state is closed before its
 * list is rebound to the current catalog. */
static void rearm_library_list(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, int total_count) {
    if (!list || library_search_is_active_on_list(list)) return;
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    compact_list_set_paged_provider(list, fetch_page, NULL, total_count);
    if (scroll_y > 0) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
        compact_list_refresh_visible(list);
    }
}

static void refresh_library_lists_in_place(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    int song_count = (int) metadata_db_get_song_count();
    rearm_library_list(all_songs_list, all_songs_fetch_page, song_count + 1);
    rearm_library_list(artists_list, artists_fetch_page, artist_count);
    rearm_library_list(albums_list, albums_fetch_page, album_count);
    rearm_library_list(album_artist_list, album_artists_fetch_page, album_artist_count);
    rearm_library_list(recently_added_list, recently_added_fetch_page, song_count + 1);
    rearm_library_list(genres_list, genres_fetch_page, count_genres_from_database());
    rearm_genre_browse_lists();
    refresh_targeted_nested_views();
}

static void invalidate_library_views_after_publication(library_rescan_mode_t completed_mode,
                                                       bool identity_changed) {
    library_search_invalidate_catalog();
    library_search_invalidate_bindings();
    hide_collection_menu();
    if (playlist_start_popup) lv_obj_add_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_start_backdrop) lv_obj_add_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
    library_thumbnails_invalidate_publication_view();
    /* A song that joined or left an album can stale its cached cover, so an
     * identity change drops the whole cache. Full publications can compact
     * song IDs and change artist representatives, so also refresh artist
     * covers, aliases, and failures after those commits. Targeted refreshes
     * keep song IDs stable and retain those records. */
    gui_player_invalidate_cover_negatives();
    library_thumbnails_invalidate_publication(identity_changed,
        completed_mode != LIBRARY_RESCAN_TARGETED);
    if (!identity_changed && completed_mode != LIBRARY_RESCAN_TARGETED) {
        artwork_failure_cache_clear();
    }
}

static void refresh_library_screens_after_reload(bool clear_thumbnail_cache) {
    /* A hot-insert cache reload can arrive while one of these screens is
     * the active LVGL screen. Deleting an active screen leaves the display's
     * act_scr pointer dangling; the next refresh then crashes in
     * lv_obj_update_layout(). A user-triggered scan has its non-library
     * progress screen active, so the old screens can be replaced safely. */
    /* Every deletion path (scan publication, SD removal, repair, reinsert)
     * comes through here. An active search owns the shared keypad and
     * textarea on the screen about to be deleted, so close all searches and
     * reject pending results first. Both helpers are idempotent. */
    library_search_invalidate_catalog();
    library_search_invalidate_bindings();
    if (clear_thumbnail_cache) library_thumbnails_clear_cache();
    if (lv_screen_active() != gui_busy_get_screen()) {
        nav_reset_to_home();
    }
    /* Remove replaced screens from navigation and the back-target cache. */
    lv_obj_t * being_replaced[] = { all_songs_screen, artists_screen, albums_screen, album_artist_screen,
                                     recently_added_screen, genres_screen };
    gui_navigation_remove_screen_instances(being_replaced, (int)(sizeof(being_replaced) / sizeof(being_replaced[0])));

    lv_obj_delete(all_songs_screen);
    lv_obj_delete(artists_screen);
    lv_obj_delete(albums_screen);
    lv_obj_delete(album_artist_screen);
    lv_obj_delete(recently_added_screen);
    lv_obj_delete(genres_screen);
    forget_deleted_library_lists();
    /* Each build_*_screen() below activates its own paged provider against
     * the current (fresh, post-reload) library internally -- no separate
     * populate step needed here, and no whole-library load either: drill-
     * down, search, and the A-Z index are all DB-backed end to end now, so
     * a rescan/SD-reinsert reload costs exactly what a fresh boot does,
     * nothing more. */
    all_songs_screen = build_all_songs_screen();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();
    recently_added_screen = build_recently_added_screen();
    genres_screen = build_genres_screen();

    /* The four old screens' A-Z index bindings (strip/popup/list pointers)
     * just went dangling along with the lv_obj_delete()s above -- re-register
     * against the freshly rebuilt screens/lists before anything can poll them. */
    library_az_index_reset_bindings();
    library_az_index_register(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    library_az_index_register(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    library_az_index_register(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    library_az_index_register(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);
    register_search(SEARCH_BINDING_RECENTLY_ADDED, recently_added_screen, recently_added_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, recently_added_fetch_page);
}

/* SD reinsertion loads an existing saved database and queue without walking
 * the whole tree again. A card with no database still needs its first scan;
 * users can explicitly update a saved database after changing files. */
static void reload_library_on_sd_reinsert(bool announce_loaded) {
    library_thumbnails_quiesce();
    library_thumbnails_clear_cache();
    playlist_files_refresh_async(PLAYLISTS_DIR);
    library_search_invalidate_catalog();
    library_search_invalidate_bindings();
    library_load_from_cache_only();
    library_cache_load_announce_loaded = announce_loaded;
    refresh_library_screens_after_reload(false); /* cache already cleared above, before the reload */
    report_library_cache_load();
    if (library_cache_load_outcome != METADATA_DB_LOAD_SUCCESS_FRESH &&
        metadata_db_get_song_count() > 0) {
        boot_warmup_pending = false;
        library_thumbnails_start_warmup();
    }
}

/* How long the "Library updated" success message stays up once a rescan
 * finishes, before falling back to Home -- purely so the user gets a
 * moment to actually read it, not a wait for anything real. */
#define LIBRARY_RESCAN_SUCCESS_MS 1500

static uint32_t library_rescan_success_since_tick = 0;

static void finish_library_cover_reload(bool cancelled) {
    library_rescan_mode_t completed_mode = library_rescan_mode;
    bool all_albums = completed_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL;
    library_thumbnail_status_t status;
    library_thumbnails_get_status(&status);
    int error_count = status.cover_error_count;
    library_rescan_active = false;
    library_thumbnails_finish_cover_reload();
    gui_player_finish_cover_reload();
    /* Pops back to the screen the reload was started from. */
    gui_busy_hide(library_rescan_token);
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    if (cancelled) return;
    if (error_count > 0) {
        if (all_albums) show_error_toast(TR("Some covers could not be refreshed"));
        else show_error_toast(TR("Could not reload cover"));
    } else if (all_albums) {
        show_info_toast(TR("Covers refreshed"));
    } else {
        show_info_toast(TR("Cover reloaded"));
    }
}

void poll_library_rescan(void) {
    library_thumbnails_retry_warmup();

    /* Keep the screen awake for the rescan progress display so the auto
     * screen-timeout does not trigger mid-rescan. */
    if (library_rescan_active || library_rescan_success_pending)
        lv_display_trigger_activity(NULL);

    if (library_rescan_success_pending) {
        if (lv_tick_elaps(library_rescan_success_since_tick) >= LIBRARY_RESCAN_SUCCESS_MS) {
            library_rescan_success_pending = false;
            if (!library_setup_scan_complete) nav_reset_to_home(); /* leaves the busy screen and discards any stale deeper screen */
        }
        return;
    }

    bool cover_reload = library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
                        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
    if (cover_reload) {
        library_thumbnail_status_t status;
        library_thumbnails_get_status(&status);
        if (!status.cover_done) {
            int total = status.cover_total_count;
            int done = status.cover_done_count;
            if (total > 0) {
                if (done > total) done = total;
                int progress = (int)((int64_t)done * 100 / total);
                gui_busy_set_progress(library_rescan_token, progress);
                char detail[96];
                snprintf(detail, sizeof(detail),
                         library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                             ? TR("Refreshing covers\n%d of %d (%d%%)")
                             : TR("Reloading cover\n%d of %d (%d%%)"),
                         done, total, progress);
                gui_busy_set_detail(library_rescan_token, detail);
            } else {
                gui_busy_set_detail(library_rescan_token,
                    library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                        ? TR("Refreshing covers") : TR("Reloading cover"));
            }
            return;
        }
        finish_library_cover_reload(library_scan_cancel_pending() || status.cover_cancelled);
        return;
    }

    if (!library_rescan_active) {
        report_library_cache_load();
        return;
    }

    if (!atomic_load_explicit(&library_rescan_done_flag, memory_order_acquire)) {
        /* Still scanning -- total stays 0 until the initial file walk
         * finishes (see library_scan_once()), so there's nothing
         * meaningful to show yet in that window; the label just keeps
         * reading "Updating music database..." until then. */
        library_scan_progress_t progress;
        library_scan_get_progress(&progress);
        int total = progress.total;
        int done = progress.done;
        char detail[96];
        int phase = progress.phase;
        if (phase == LIBRARY_SCAN_PHASE_WALK) {
            int items = progress.walk_items;
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "%s", TR("Preparing metadata refresh"));
            else if (items > 0) snprintf(detail, sizeof(detail), TR("Looking for music files\n%d items checked"), items);
            else snprintf(detail, sizeof(detail), "%s", TR("Looking for music files"));
        } else if (phase == LIBRARY_SCAN_PHASE_TAGS && total > 0) {
            if (done > total) done = total;
            gui_busy_set_progress(library_rescan_token, (int32_t) ((int64_t) done * 100 / total));
            snprintf(detail, sizeof(detail), TR("Reading tags\n%d of %d songs (%d%%)"), done, total,
                     (int) ((int64_t) done * 100 / total));
        } else {
            gui_busy_set_progress(library_rescan_token, 100);
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "%s", TR("Saving music database"));
            else
                snprintf(detail, sizeof(detail), "%s", TR("Saving music database\nThis can take a while on large libraries"));
        }
        gui_busy_set_detail(library_rescan_token, detail);
        return;
    }

    library_rescan_mode_t completed_mode = library_rescan_mode;
    library_rescan_active = false;
    bool scan_cancelled = library_scan_cancel_pending();
    pthread_join(library_rescan_thread, NULL);

    library_scan_result_t scan_result;
    library_scan_copy_result(&scan_result);
    bool identity_changed = scan_result.identity_changed;
    bool published = scan_result.published;
    if (published) invalidate_library_views_after_publication(completed_mode, identity_changed);
    if (scan_cancelled) {
        if (published) {
            if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
            else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
        }
        gui_busy_hide(library_rescan_token);
        if (!library_setup_scan_complete) nav_reset_to_home();
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        finish_setup_scan(false);
        return;
    }

    bool overall_success = scan_result.succeeded &&
                           scan_result.operation_outcome != METADATA_DB_LOAD_FAILED &&
                           scan_result.operation_outcome != METADATA_DB_LOAD_UNMOUNTED;
    if (published) {
        if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
        else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
    }

    /* Thumbnail generation is optional cache warming. It must not become a
     * second user-visible phase after a database scan -- no progress screen,
     * no toast. The worker yields when visible rows queue lazy work so those
     * decodes stay first. */
    if (!overall_success) {
        gui_busy_hide(library_rescan_token);
        if (!library_setup_scan_complete) nav_reset_to_home();
        if (scan_result.operation_outcome == METADATA_DB_LOAD_UNMOUNTED) {
            show_error_toast(TR("No SD card"));
        } else if (scan_result.load_blocked) {
            if (scan_result.operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED)
                show_info_toast(TR(library_recovered_message));
            else
                show_error_toast(TR("Library unavailable. Use Settings > Update Music Database to rebuild"));
        } else {
            show_error_toast(TR("Library update failed. Check SD card and retry"));
        }
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        finish_setup_scan(false);
        return;
    }
    if (published && completed_mode != LIBRARY_RESCAN_TARGETED) library_thumbnails_start_warmup();
    gui_busy_hide(library_rescan_token);

    if (completed_mode != LIBRARY_RESCAN_NORMAL) {
        /* Still set from the finished job; reset only when the next starts. */
        /* Parser failures, non-ENOENT stat errors and skipped folders all
         * mean some songs were not re-read. */
        if (scan_result.failed_count > 0 || scan_result.incomplete)
            show_error_toast(TR("Some songs could not be read"));
        else show_info_toast(TR("Metadata refreshed"));
    } else if (scan_result.operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        show_info_toast(scan_result.saved
                            ? (scan_result.incomplete ? TR("Library recovered and saved, some folders could not be read")
                                                         : TR("Library recovered and saved"))
                            : TR(library_recovered_message));
    } else {
        show_info_toast(scan_result.incomplete ? TR("Library updated, some folders could not be read")
                                                  : TR("Library updated"));
    }
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    /* A targeted refresh returns to the list it was started from, refreshed
     * in place; only full scans rebuild the library and reset to Home. */
    if (completed_mode == LIBRARY_RESCAN_TARGETED) return;
    if (library_setup_scan_complete) {
        finish_setup_scan(true);
        return;
    }
    library_rescan_success_pending = true;
    library_rescan_success_since_tick = lv_tick_get();
}

/* SD mount-failure detection + Format SD Card -- see poll_sd_card_hotplug()
 * below for the detection/debounce logic and sd_format_card_worker() further
 * down for the actual format sequence. These three are declared here
 * (rather than alongside their real definitions further down) so that both
 * halves of the feature -- the gated detection logic below, and the
 * ungated popups/background thread that come after it in this file -- can
 * see them regardless of which one is defined first. */
static void show_sd_mount_failed_popup(void); /* defined below, alongside its popup */
static bool sd_mount_fail_notified = false;
static bool sd_handoff_pending = false;

static bool file_folder_job_storage_invalidated(void) {
#ifdef HOST_BUILD
    return false;
#else
    return sd_handoff_pending || sd_repair_in_progress() || !sd_card_root_is_mounted();
#endif
}

#ifndef HOST_BUILD

/* Periodic polling for SD card mount state changes.
 *
 * When an unmounted card is detected, retries mounting. On transition to
 * mounted, reloads the database cache from the card. On transition to
 * unmounted, closes the database, clears library screens, and resets
 * the file browser to root. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

static dev_t last_mounted_device;
static bool have_last_mounted_device;
static char last_card_cid[64];
static bool have_last_card_identity;
static bool prompt_card_change_seen;

static bool sd_card_mount_attached(dev_t * mounted_device) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat(MUSIC_ROOT_DIR, &root_st) != 0) return false;
    if (parent_st.st_dev == root_st.st_dev) return false;
    if (mounted_device) *mounted_device = root_st.st_dev;
    return true;
}

static sd_probe_result_t sd_kernel_device_probe(dev_t mounted_device, const char * node) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/block/%s/dev", node);
    errno = 0;
    FILE * file = fopen(path, "r");
    if (!file) return errno == ENOENT ? SD_PROBE_ABSENT : SD_PROBE_UNKNOWN;
    unsigned major_number, minor_number;
    int parsed = fscanf(file, "%u:%u", &major_number, &minor_number);
    fclose(file);
    if (parsed != 2) return SD_PROBE_UNKNOWN;
    return mounted_device == makedev(major_number, minor_number) ? SD_PROBE_MATCH : SD_PROBE_DIFFERENT;
}

static bool sd_card_root_is_mounted_observed(bool * attached_observed) {
    dev_t mounted_device;
    bool attached = sd_card_mount_attached(&mounted_device);
    if (attached_observed) *attached_observed = attached;
    if (!attached) return false;
    /* ntfs-3g uses a FUSE filesystem device rather than the underlying MMC
     * device. Its identity is checked by the card CID in the hotplug poll;
     * presence in sysfs still catches a removed card. */
    if (major(mounted_device) != 179) {
        struct stat st;
        if (stat("/sys/class/block/mmcblk0", &st) == 0) return true;
        /* A failed stat that is not "node missing" is not a pulled card. */
        return errno != ENOENT;
    }
    /* The kernel can leave an exFAT mount attached after the card is pulled.
     * /dev/mmcblk0p1 can also be stale if mdev missed the replacement event.
     * Compare the mount's actual device with sysfs, which follows the newly
     * inserted card even when its /dev node has not been recreated. A sysfs
     * read error is not that comparison and must not look like removal. */
    return !sd_card_probes_confirm_removed(
        sd_kernel_device_probe(mounted_device, "mmcblk0p1"),
        sd_kernel_device_probe(mounted_device, "mmcblk0"));
}

bool sd_card_root_is_mounted(void) {
    return sd_card_root_is_mounted_observed(NULL);
}

static int sd_card_sysfs_node_stat(const char * node, int * err) {
    char path[96];
    struct stat st;
    snprintf(path, sizeof(path), "/sys/class/block/%s", node);
    if (stat(path, &st) == 0) {
        *err = 0;
        return 0;
    }
    *err = errno;
    return -1;
}

static bool sd_card_device_node_present(void) {
    int ignored = 0;
    /* Query sysfs rather than /dev: mdev may have left stale nodes behind.
     * A partition-less card has only the whole-disk entry. */
    return sd_card_sysfs_node_stat("mmcblk0p1", &ignored) == 0 ||
           sd_card_sysfs_node_stat("mmcblk0", &ignored) == 0;
}

/* Both nodes missing. A read error on either one is not confirmation. */
static bool sd_card_device_nodes_confirmed_absent(void) {
    int partition_errno = 0, disk_errno = 0;
    int partition_rc = sd_card_sysfs_node_stat("mmcblk0p1", &partition_errno);
    int disk_rc = sd_card_sysfs_node_stat("mmcblk0", &disk_errno);
    return sd_card_sysfs_stats_confirm_absent(partition_rc, partition_errno, disk_rc, disk_errno);
}

/* Whole-disk sysfs entry -- present whenever a card is physically inserted
 * regardless of whether it has a partition table at all, so this is what
 * distinguishes "no card" from "card inserted but can't be mounted" for the
 * mount-failure/Format SD Card flow below (mount_sd_card_if_needed() itself
 * -- see main.c -- already tries every filesystem type this platform
 * actually supports, so a card that's physically present but still never
 * mounts genuinely needs a repartition/reformat, not just a different -t). */
static bool sd_card_base_device_present(void) {
    struct stat st;
    return stat("/sys/class/block/mmcblk0", &st) == 0;
}

static void sd_card_read_cid(char out[64]) {
    out[0] = '\0';
    FILE * file = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!file) return;
    if (fgets(out, 64, file)) out[strcspn(out, "\r\n")] = '\0';
    fclose(file);
}

static library_prompt_card_status_t library_prompt_card_status(void) {
    if (!sd_card_root_is_mounted()) return LIBRARY_PROMPT_CARD_UNKNOWN;
    bool have_current_device = false;
    dev_t current_device = 0;
    have_current_device = sd_card_mount_attached(&current_device);
    if (!have_current_device) return LIBRARY_PROMPT_CARD_UNKNOWN;
    char current_cid[64];
    sd_card_read_cid(current_cid);
    if (sd_card_identity_replaced(true, true, have_last_card_identity,
                                  have_last_mounted_device, last_mounted_device,
                                  have_current_device, current_device,
                                  last_card_cid, current_cid))
        return LIBRARY_PROMPT_CARD_CHANGED;
    if (have_last_card_identity && !current_cid[0]) return LIBRARY_PROMPT_CARD_UNKNOWN;
    return LIBRARY_PROMPT_CARD_CURRENT;
}

static void library_prompt_prepare_cache_load_card(void) {
    if (!sd_card_root_is_mounted()) return;
    dev_t current_device = 0;
    bool have_current_device = sd_card_mount_attached(&current_device);
    char current_cid[64];
    sd_card_read_cid(current_cid);
    bool replaced = sd_card_identity_replaced(true, true, have_last_card_identity,
                                              have_last_mounted_device, last_mounted_device,
                                              have_current_device, current_device,
                                              last_card_cid, current_cid);
    if (replaced) library_prompt_card_changed();
}

static void clear_removed_sd_library(void) {
    /* Signal the worker without joining on the hotplug/repair path. Its UI
     * completion joins later and rejects even an already-finished result. */
    gui_folder_actions_invalidate_storage_targets();
    gui_folder_actions_cancel();
    file_op_cancel(); /* a File Manager copy must not continue on a new card */
    library_thumbnails_quiesce();
    gui_player_handle_sd_unmount();
    metadata_db_close();
    refresh_library_screens_after_reload(true);
    file_browser_reset_to_root();
}

static void show_sd_repair_note(void) {
    const char * message = NULL;
    switch (sd_repair_take_note()) {
    case SD_REPAIR_NOTE_STARTED: message = TR("Checking the SD card. This may take a while"); break;
    case SD_REPAIR_NOTE_REPAIRED: message = TR("SD card repaired"); break;
    case SD_REPAIR_NOTE_STILL_READONLY: message = TR("SD card is still read-only"); break;
    case SD_REPAIR_NOTE_FAILED: message = TR("Could not repair the SD card"); break;
    case SD_REPAIR_NOTE_NEEDS_COMPUTER: message = TR("SD card may have errors. Check it on a computer"); break;
    case SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER: message = TR("SD card is read-only. Check it on a computer"); break;
    case SD_REPAIR_NOTE_NONE: break;
    }
    if (message) show_error_toast(message);
}

/* Drop file handles so umount can succeed after the kernel has remounted
 * a damaged card read-only underneath an open library. */
static void release_sd_handles_for_repair(void) {
    if (library_rescan_active) gui_library_cancel_scan();
    library_thumbnails_cancel_warmup();
    gui_player_notify_sd_unmounted_immediate();
    clear_removed_sd_library();
}

void poll_sd_card_hotplug(void) {
    /* Assume already mounted initially since mount_sd_card_if_needed() ran
     * at boot. */
    static bool was_mounted = true;
    /* See its own comment further down, where it's checked -- separate
     * one-shot guard for the boot-time-mount-race case was_mounted's own
     * confirmed-transition logic can silently miss. */
    static bool boot_library_recheck_done = false;
    static time_t last_check = 0;
    static int mount_fail_streak = 0;
    /* Require unmounted state across consecutive polls
     * (SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD) before tearing down library
     * state to debounce brief transient unmounts. */
    static int unmount_confirm_streak = 0;
#define SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD 2

    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < SD_CARD_MOUNT_POLL_SECONDS) return;
    last_check = now;
    show_sd_repair_note();

    bool mount_observed = false;
    bool mounted = sd_card_root_is_mounted_observed(&mount_observed);
    sd_repair_kick_result_t repair = { false, false };
    bool repair_kick_attempted = false;
    if (mounted && !sd_repair_in_progress()) {
        /* Only a card the kernel remounted read-only is checked; see sd_fsck.h. */
        repair_kick_attempted = true;
        repair = sd_readonly_repair_kick(release_sd_handles_for_repair);
    }
    if (repair_kick_attempted) {
        if (repair.started || !sd_card_root_is_mounted()) {
            mounted = false;
            mount_observed = false;
            was_mounted = false;
            sd_handoff_pending = true;
            have_last_card_identity = false;
            have_last_mounted_device = false;
            last_card_cid[0] = '\0';
        } else if (repair.released_handles) {
            /* The check could not unmount. Reload the library that was
             * closed to make the unmount attempt. */
            was_mounted = false;
            sd_handoff_pending = true;
        }
    }
    show_sd_repair_note();
    if (sd_handoff_pending || sd_repair_in_progress()) {
        gui_folder_actions_invalidate_storage_targets();
        gui_folder_actions_cancel();
    }
    /* Only an attached mount that the kernel probes proved stale may bypass
     * the normal two-poll absence debounce. If the initial mount stat was
     * inconclusive, a successful retry is not evidence of a stale mount. */
    bool stale_mount = sd_card_stale_mount_confirmed(mount_observed, mounted);
    dev_t current_mounted_device = 0;
    /* The mount check above already stat'd this card. This reread can fail
     * on its own; a leftover 0 is not a different device number. */
    bool have_current_device = mounted && sd_card_mount_attached(&current_mounted_device);
    char current_card_cid[64];
    sd_card_read_cid(current_card_cid);
    bool replaced_between_polls = sd_card_identity_replaced(
        mounted, was_mounted, have_last_card_identity, have_last_mounted_device, last_mounted_device,
        have_current_device, current_mounted_device, last_card_cid, current_card_cid);
    if (replaced_between_polls) {
        if (!prompt_card_change_seen) library_prompt_card_changed();
        prompt_card_change_seen = true;
    } else if (!mounted) {
        library_prompt_requeue_visible();
    }
    /* Unconfirmed absence still cancels a scan and, below, blocks writes.
     * It does not latch sd_handoff_pending; that waits for confirmation. */
    if (sd_card_sample_cancels_scan(mounted, replaced_between_polls, library_rescan_active))
        gui_library_cancel_scan();
    if (replaced_between_polls && !library_rescan_active && current_card_cid[0]) {
        /* An old card may have been unmounted and a new one mounted wholly
         * between polls. The mount is valid, but the in-memory library and
         * queue still belong to the previous card. */
        library_thumbnails_cancel_warmup();
        gui_player_notify_sd_unmounted_immediate();
        clear_removed_sd_library();
        was_mounted = false;
    }
    /* Node-missing is decided before umount changes `mounted`. A sysfs
     * read error is not node-missing; only ENOENT on both nodes is. */
    bool forced_removal = mounted && sd_card_device_nodes_confirmed_absent();
    if (stale_mount || forced_removal) {
        /* -l (lazy): the node's already gone, so there's no real device
         * left to flush to, and a plain umount would fail with EBUSY if
         * this app (or anything else) still has an fd open on a file under
         * MUSIC_ROOT_DIR from right before the card was pulled -- the same
         * reasoning mass_storage_removing.sh already applies to external
         * USB media on this firmware (see its own "umount -l" call). */
        char * argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
        subprocess_run(argv, NULL, 0);
        mounted = false;
        if (library_rescan_active) gui_library_cancel_scan();
        if (!prompt_card_change_seen) library_prompt_card_changed();
        prompt_card_change_seen = true;
    }

    group_probe_storage_available = mounted;
    if (!mounted) {
        gui_folder_actions_invalidate_storage_targets();
        gui_folder_actions_cancel();
        if (group_probe_job && group_probe_job_generation == group_probe_generation)
            cancel_group_song_probes();
        gui_player_notify_sd_unmounted_immediate();
        bool removal_confirmed = sd_card_absence_confirmed(
            stale_mount || forced_removal, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
        if (was_mounted) {
            /* Stop post-scan artwork reads/writes as soon as removal is
             * observed. Joining is deliberately deferred to the next
             * generator start/reap poll so this hotplug callback never
             * blocks the UI on an in-progress image decode. */
            library_thumbnails_invalidate_storage();
            unmount_confirm_streak++;
            removal_confirmed = sd_card_absence_confirmed(
                stale_mount || forced_removal, unmount_confirm_streak,
                SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
            if (removal_confirmed && !prompt_card_change_seen) {
                library_prompt_card_changed();
                prompt_card_change_seen = true;
            }
            if (removal_confirmed && !library_rescan_active) {
                /* Close old handles before loading the next card. */
                clear_removed_sd_library();
                was_mounted = false;
                unmount_confirm_streak = 0;
                have_last_card_identity = false;
                have_last_mounted_device = false;
                last_card_cid[0] = '\0';
            }
            /* Else: not yet confirmed (or a rescan from something else,
             * e.g. Settings > Update Music Database, is already running)
             * -- was_mounted deliberately stays true, so a flicker that
             * remounts before reaching the threshold is not a handoff.
             * The latch below follows removal_confirmed, which was sampled
             * before the streak reset, so a confirmed removal stays pending
             * while the scan still holds the database. */
        }
        sd_handoff_pending = sd_card_handoff_after_sample(
            sd_handoff_pending, removal_confirmed, replaced_between_polls);

        /* Tear down state tied to the old card before mounting a replacement.
         * Otherwise a rapid swap can hide the unmounted edge from the next
         * poll and leave the previous card's library and queue in memory. */
        mount_sd_card_if_needed();
        if (sd_card_root_is_mounted()) {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
            return;
        }
        /* A read-only card is unmounted while fsck runs. That is not a
         * missing or unmountable card, so it must not offer Format. */
        if (sd_repair_in_progress()) {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
            return;
        }

        /* Card is physically there (the whole-disk node exists) but still
         * won't mount after repeated retries. mount_sd_card_if_needed()
         * (main.c) already tries every supported filesystem type against
         * both the first-partition node and, as a fallback, the whole-disk
         * node itself (covering a "superfloppy" card with no partition
         * table/MBR at all) -- so reaching here means either the partition
         * exists but holds something none of those attempts could mount
         * (corruption, or a filesystem this platform genuinely doesn't
         * support), or the raw disk itself does too. Both need the same fix
         * -- reformat -- so this doesn't try to tell them apart. Debounced
         * (SD_MOUNT_FAIL_STREAK_THRESHOLD
         * consecutive failed polls, SD_CARD_MOUNT_POLL_SECONDS apart) so
         * the kernel's own brief card-enumeration window right after
         * physical insertion -- where the node legitimately isn't there
         * *yet* -- doesn't fire a false alarm; not re-shown on every poll
         * once shown once (sd_mount_fail_notified), so the user isn't
         * nagged again until the card actually changes (removed, or a
         * format attempt runs -- both reset it below/in poll_sd_format()). */
        if (sd_card_base_device_present()) {
            mount_fail_streak++;
            if (mount_fail_streak >= SD_MOUNT_FAIL_STREAK_THRESHOLD && !sd_mount_fail_notified && !sd_format_active) {
                sd_mount_fail_notified = true;
                show_sd_mount_failed_popup();
            }
        } else {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
        }
        return;
    }

    mount_fail_streak = 0;
    sd_mount_fail_notified = false;
    unmount_confirm_streak = 0; /* seeing "mounted" again cancels any not-yet-confirmed removal */
    /* Same card again does not drop a latch. Only confirmation or a real
     * replacement sets one; passing a cleared streak here leaves a real
     * pending handoff untouched. */
    sd_handoff_pending = sd_card_handoff_after_sample(
        sd_handoff_pending,
        sd_card_absence_confirmed(false, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD),
        replaced_between_polls);
    if (have_last_card_identity && !current_card_cid[0]) {
        /* A transient CID read failure is an uncertain identity, never a
         * license to restore the old card's queue or cache. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    if (library_rescan_active && (sd_handoff_pending || !was_mounted || replaced_between_polls)) {
        /* Keep the old identity until the scan worker releases its database
         * handles; the next poll must still perform the card handoff. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    /* Every poll that observes the card mounted un-sticks the checkpoint
    * target, even for a transient blip that never reached the confirmed-
     * removal streak above. */
    gui_player_notify_sd_mounted();
    /* Reset file browser and reload fallback fonts before triggering the
     * rescan screen switch. */
    if (sd_handoff_pending && !library_rescan_active) {
        if (was_mounted) clear_removed_sd_library();
        was_mounted = false;
        gui_folder_actions_invalidate_storage_targets();
        file_browser_reset_to_root();
        fallback_font_on_sd_mounted();
        reload_library_on_sd_reinsert(true);
        gui_player_restore_sd_queue(false);
        sd_handoff_pending = false;
    } else if (!boot_library_recheck_done) {
        boot_library_recheck_done = true;
        fallback_font_on_sd_mounted();
        if (metadata_db_get_load_outcome() == METADATA_DB_LOAD_UNMOUNTED && !library_rescan_active) {
            gui_folder_actions_invalidate_storage_targets();
            file_browser_reset_to_root();
            reload_library_on_sd_reinsert(false);
        }
        /* Covers the boot-time mount race this whole branch exists for: the
         * card genuinely was NOT YET mounted when gui_init() ran its own
         * SD-restore attempt (both use the same sd_card_root_is_mounted()
         * check), but became mounted by this first poll. This is the only
         * other place a "the card actually showed up right after boot"
         * restore can happen --
         * boot_library_recheck_done's one-shot guard means this fires at
         * most once per run. */
        gui_player_restore_sd_queue(true);
    }
    /* Handoff above may have cleared was_mounted. A missed reread must not
     * be stored as device 0, or the next poll looks like a different card. */
    have_last_mounted_device = sd_card_remember_mounted_device(
        have_current_device, current_mounted_device, replaced_between_polls, was_mounted,
        &last_mounted_device, have_last_mounted_device);
    was_mounted = true;
    if (current_card_cid[0]) snprintf(last_card_cid, sizeof(last_card_cid), "%s", current_card_cid);
    /* An empty CID after a handoff must not resurrect the previous card's
     * CID. A known device number remains a valid identity baseline, so a
     * later device change is still detected while CID reads recover. */
    have_last_card_identity = sd_card_identity_is_known(have_last_mounted_device, current_card_cid);
    prompt_card_change_seen = false;
}
#else
bool sd_card_root_is_mounted(void) {
    struct stat st;
    return stat(MUSIC_ROOT_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

static library_prompt_card_status_t library_prompt_card_status(void) {
    return sd_card_root_is_mounted() ? LIBRARY_PROMPT_CARD_CURRENT : LIBRARY_PROMPT_CARD_UNKNOWN;
}

static void library_prompt_prepare_cache_load_card(void) {
}

void poll_sd_card_hotplug(void) {
}
#endif

/* Actual "Format SD Card" sequence -- runs on a background thread (see
 * start_sd_format()/poll_sd_format() below), since fdisk/mkdosfs can take
 * real time on a slow card and this must never block the UI thread.
 *
 * UNVERIFIED ON REAL HARDWARE. Every command here was chosen from static
 * analysis of the real firmware's own busybox binary (`strings` confirmed
 * mkdosfs/fdisk/dd/umount/partprobe are all compiled-in applets, including
 * fdisk's own interactive prompt text, proving a standard util-linux-style
 * command menu), not from an actual run -- this is a MIPS binary and can't
 * be executed on the dev host, and no live device was available while this
 * was written. Test with a spare/non-critical card before trusting it with
 * a card that matters.
 *
 * Sequence: best-effort unmount, zero the first sector (so fdisk always
 * sees a blank disk regardless of whatever partition table, or lack of
 * one, the card came in with -- avoids needing to conditionally script
 * fdisk's own delete-partition flow for "already has a stale/foreign
 * table"), script fdisk to create one primary partition spanning the whole
 * disk typed W95 FAT32 (LBA), partprobe to force the kernel to notice the
 * new partition, wait for its device node to appear, mkdosfs it, then hand
 * off to the normal mount path and confirm it actually mounted. */
#ifndef HOST_BUILD
static bool sd_format_card_worker(void) {
    char * umount_argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
    subprocess_run(umount_argv, NULL, 0);

    if (!sd_card_base_device_present()) return false; /* card pulled before/during format */

    char * dd_argv[] = { (char *) "dd", (char *) "if=/dev/zero", (char *) "of=/dev/mmcblk0",
                          (char *) "bs=512", (char *) "count=1", NULL };
    if (!subprocess_run_timeout(dd_argv, NULL, 0, 15000)) return false;

    pid_t fdisk_pid;
    int fdisk_write_fd;
    char * fdisk_argv[] = { (char *) "fdisk", (char *) "/dev/mmcblk0", NULL };
    if (!subprocess_popen_stdin(fdisk_argv, &fdisk_pid, &fdisk_write_fd)) return false;
    /* n(ew) -> p(rimary) -> partition 1 -> default first sector [Enter] ->
     * default last sector [Enter] -> t(ype) -> c (W95 FAT32, LBA) -> w(rite)+exit. */
    const char * fdisk_cmds = "n\np\n1\n\n\nt\nc\nw\n";
    ssize_t ignored = write(fdisk_write_fd, fdisk_cmds, strlen(fdisk_cmds));
    (void) ignored;
    close(fdisk_write_fd);
    subprocess_terminate(fdisk_pid); /* reaps it -- `w` alone isn't guaranteed to have taken effect yet */

    char * partprobe_argv[] = { (char *) "partprobe", (char *) "/dev/mmcblk0", NULL };
    subprocess_run_timeout(partprobe_argv, NULL, 0, 10000);

    bool got_partition = false;
    for (int i = 0; i < 20 && !got_partition; i++) {
        if (sd_card_device_node_present()) { got_partition = true; break; }
        usleep(250000);
    }
    if (!got_partition) return false;

    char * mkdosfs_argv[] = { (char *) "mkdosfs", (char *) "-F", (char *) "32", (char *) "-n",
                               (char *) "MUSIC", (char *) "/dev/mmcblk0p1", NULL };
    if (!subprocess_run_timeout(mkdosfs_argv, NULL, 0, 120000)) return false;

    mount_sd_card_if_needed();
    return sd_card_root_is_mounted();
}
#else
static bool sd_format_card_worker(void) {
    return false;
}
#endif

static pthread_t sd_format_thread;
static atomic_bool sd_format_done_flag = false;
static volatile bool sd_format_succeeded = false;

static void * sd_format_thread_func(void * arg) {
    (void) arg;
    sd_format_succeeded = sd_format_card_worker();
    atomic_store_explicit(&sd_format_done_flag, true, memory_order_release); /* written last -- poll_sd_format() only checks this flag */
    return NULL;
}

static void start_sd_format(void) {
    if (sd_repair_in_progress()) {
        show_error_toast(TR("SD card repair is still running"));
        return;
    }
    if (file_op_running()) {
        show_error_toast(TR("Another file operation is still running"));
        return;
    }
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    atomic_store_explicit(&sd_format_done_flag, false, memory_order_relaxed);
    sd_format_active = true;
    sd_format_token = gui_busy_show(TR("Formatting\nSD Card..."), "");
        if (pthread_create(&sd_format_thread, NULL, sd_format_thread_func, NULL) != 0) {
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_sd_format(void) {
    if (!sd_format_active || !atomic_load_explicit(&sd_format_done_flag, memory_order_acquire)) return;

    sd_format_active = false;
    pthread_join(sd_format_thread, NULL);
    gui_busy_hide(sd_format_token);

    if (sd_format_succeeded) {
        show_error_toast(TR("SD card formatted"));
        sd_mount_fail_notified = false; /* give the freshly-formatted card a clean slate */
        if (!library_rescan_active) {
            start_library_auto_rescan();
            gui_folder_actions_invalidate_storage_targets();
            file_browser_reset_to_root();
        }
    } else {
        show_error_toast(TR("SD card format failed"));
    }
}

static gui_popup_t sd_mount_failed_popup;
static gui_popup_t sd_format_confirm_popup;

static void hide_sd_mount_failed_popup(void) {
    gui_popup_hide(&sd_mount_failed_popup);
}

static void hide_sd_format_confirm_popup(void) {
    gui_popup_hide(&sd_format_confirm_popup);
}

static void sd_mount_failed_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_mount_failed_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_format_confirm_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_format_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_mount_failed_format_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
    gui_popup_show(&sd_format_confirm_popup);
}

static void sd_format_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
    start_sd_format();
}

/* Displayed when persistent SD card mount failure is detected. */
static void show_sd_mount_failed_popup(void) {
    gui_popup_show(&sd_mount_failed_popup);
}

static void build_sd_mount_failed_popup(void) {
    sd_mount_failed_popup.popup = build_confirm_popup(
        TR("SD card couldn't be read"), LV_LABEL_LONG_WRAP, NULL,
        TR("It may have no partition table or a file system this player can't use. "
        "Formatting will erase it and set it up for this player."),
        TR("Format SD Card"), lv_color_hex(GUI_BREEZE_NEGATIVE), sd_mount_failed_format_btn_cb, NULL, TR("Dismiss"),
        accent_lv_color(), sd_mount_failed_dismiss_cb, NULL, sd_mount_failed_popup_backdrop_cb,
        &sd_mount_failed_popup.backdrop);
}

static void build_sd_format_confirm_popup(void) {
    sd_format_confirm_popup.popup = build_confirm_popup(
        TR("Erase and format SD card?"), LV_LABEL_LONG_WRAP, NULL,
        TR("This permanently deletes everything on the card. This cannot be undone."), TR("Format"),
        lv_color_hex(GUI_BREEZE_NEGATIVE), sd_format_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), sd_format_cancel_cb,
        NULL, sd_format_confirm_popup_backdrop_cb, &sd_format_confirm_popup.backdrop);
}

void update_music_database_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_library_rescan();
}

/* No whole-library load anywhere in this app anymore -- all paged/DB-
 * backed now, see build_artists_screen()'s own comment. Drilling into an
 * artist/album-artist's own albums (artist_row_click_cb()/album_artist_
 * row_click_cb()), an album's own songs (album_row_click_cb()), search,
 * and the A-Z index are all direct, targeted DB queries. */
static void artists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(artists_screen);
}

static void albums_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* metadata_db_get_group_counts() is a real DB query, worth skipping
     * outright (not just leaving DB_LOG to no-op on the result) since this
     * runs on every ordinary tap into Albums, not just during a scan. */
    if (db_log_enabled()) {
        int artist_count = 0, album_artist_count = 0, album_count = 0;
        metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
        library_thumbnails_note_album_page_open();
        library_thumbnail_status_t thumb_status;
        library_thumbnails_get_status(&thumb_status);
        DB_LOG("ALBUMS_PAGE", "open_requested albums=%d cache_worker_active=%d lazy_active=%d rss_kb=%ld",
               album_count, (int) thumb_status.warmup_active,
               (int) thumb_status.lazy_active,
               db_log_rss_kb());
    }
    nav_push(albums_screen);
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(album_artist_screen);
}

static void genres_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(genres_screen);
}

static void playlists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_playlists_open_saved_screen();
}

static void music_files_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(files_screen);
}

static void music_screen_playback_settings_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_playback();
}

static void music_screen_sound_settings_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_sound();
}

static void music_screen_library_settings_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_library();
}

static void music_plugin_item_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    plugin_manager_music_list_item_clicked((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * build_music_screen(void) {
    const icon_grid_item_t items[UI_MUSIC_COUNT] = {
        { "submenu/files.png", NULL, TR("Files"), music_files_tile_cb, NULL },
        { "submenu/artists.png", NULL, TR("Artists"), artists_tile_cb, NULL },
        { "submenu/albums.png", NULL, TR("Albums"), albums_tile_cb, NULL },
        { "submenu/album_artist.png", NULL, TR("Album Artist"), album_artist_tile_cb, NULL },
        { "submenu/genres.png", NULL, TR("Genres"), genres_tile_cb, NULL },
        { "submenu/all_songs.png", NULL, TR("All Songs"), all_songs_tile_cb, NULL },
        { "submenu/playlists.png", NULL, TR("Playlists"), playlists_tile_cb, NULL },
    };
    const ui_customization_t customization = ui_customization_effective();
    icon_grid_item_t ordered_items[UI_MUSIC_COUNT + PLUGIN_MAX_MUSIC_LIST_ITEMS];
    int ordered_count = 0;
    bool music_seen[UI_MUSIC_COUNT] = { false };
    int configured_music_count = customization.music.count;
    if (configured_music_count > UI_ORDER_MAX) configured_music_count = UI_ORDER_MAX;
    for (int i = 0; i < configured_music_count; ++i) {
        int id = customization.music.ids[i];
        if (id < 0 || id >= UI_MUSIC_COUNT || music_seen[id]) continue;
        music_seen[id] = true;
        ordered_items[ordered_count++] = items[id];
    }

    /* Plugin actions live alongside library categories. Native order and
     * visibility preferences continue to apply to the built-in entries. */
    int plugin_count = plugin_manager_get_music_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_MUSIC_LIST_ITEMS; ++i) {
        const char * icon, * text_size;
        int32_t height, width;
        plugin_manager_get_music_list_item_options(i, &icon, &height, &width, &text_size);
        icon_grid_item_t * item = &ordered_items[ordered_count++];
        *item = (icon_grid_item_t){ "submenu/albums.png", NULL,
            plugin_manager_get_music_list_item_label(i), music_plugin_item_cb,
            (void *) (intptr_t) i };
        if (height > 0) { item->has_row_height = true; item->row_height = height; }
        if (width > 0) { item->has_row_width = true; item->row_width = width; }
        item->text_size = text_size;
        item->plugin_icon = icon;
    }

    /* Native Music remains a category list, with launcher theme styling
     * applied by the same builder as before. With no native or plugin
     * entries, build_pill_list_screen keeps its title and
     * native scroll area without inventing a placeholder row. */
    lv_obj_t * scr = ordered_count > 0
        ? build_category_menu_screen(TR("Music"), generic_back_cb, ordered_items, ordered_count,
                                     &launcher_layout_config.music)
        : build_pill_list_screen(TR("Music"), generic_back_cb, NULL, 0,
                                 gui_theme_accent_style(),
                                 launcher_layout_config.music.row_gap > 0
                                     ? launcher_layout_config.music.row_gap : GUI_ROW_GAP,
                                 100);
    if (!scr) return NULL;

    static const char * toolbar_icons[UI_MUSIC_TOOLBAR_COUNT] = {
        "settings/sound.png", "settings/playback.png", "submenu/music_database.png"
    };
    static lv_event_cb_t toolbar_callbacks[UI_MUSIC_TOOLBAR_COUNT] = {
        music_screen_sound_settings_cb, music_screen_playback_settings_cb,
        music_screen_library_settings_cb
    };
    lv_obj_t * toolbar_buttons[UI_MUSIC_TOOLBAR_COUNT] = { NULL };
    bool toolbar_seen[UI_MUSIC_TOOLBAR_COUNT] = { false };
    int toolbar_count = 0;
    int configured_toolbar_count = customization.music_toolbar.count;
    if (configured_toolbar_count > UI_ORDER_MAX) configured_toolbar_count = UI_ORDER_MAX;
    for (int i = 0; i < configured_toolbar_count && toolbar_count < UI_MUSIC_TOOLBAR_COUNT; ++i) {
        int id = customization.music_toolbar.ids[i];
        if (id < 0 || id >= UI_MUSIC_TOOLBAR_COUNT || toolbar_seen[id]) continue;
        toolbar_seen[id] = true;
        toolbar_buttons[toolbar_count] = build_top_right_icon_button(
            scr, asset_path(toolbar_icons[id]), toolbar_callbacks[id]);
        toolbar_count++;
    }
    /* The configured sequence is left-to-right. Since each action is
     * aligned from the right edge, place later entries farther right. */
    for (int i = 0; i < toolbar_count; ++i) {
        lv_obj_align(toolbar_buttons[i], LV_ALIGN_TOP_RIGHT,
                     -(toolbar_count - 1 - i) * HEADER_BUTTON_WIDTH,
                     STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - HEADER_BUTTON_WIDTH) / 2);
    }

    /* build_top_right_icon_button() gives a title room for one icon while
     * constructing each button. Recompute against the actual leftmost
     * visible button, or restore the normal full title width if none remain. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t * child = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_USER_4) && !lv_obj_get_user_data(child)) {
            if (toolbar_count > 0) {
                reserve_title_width_before(child, toolbar_buttons[0]);
            }
        }
    }
    finalize_screen_navigation(scr);
    return scr;
}

void gui_library_refresh_music_screen(void) {
    lv_obj_t * old = music_screen;
    lv_obj_t * fresh = build_music_screen();
    if (!fresh) return;
    music_screen = fresh;
    gui_navigation_replace_static_screen(1, old, fresh);
    if (old) lv_obj_delete(old);
}

/* Shared click handler for every plugin-registered Stream Media tile below
 * -- user_data is the tile's index into plugin_manager's own
 * plugin_stream_tiles[] (not an LVGL object), same index-not-object shape
 * plugin_list_row_click_cb() already uses. */
void plugin_stream_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_stream_tile_clicked(index);
}

/* Subsonic is the one built-in, real, working option here -- Qobuz/Tidal
 * (paid regional subscriptions + a real API integration this project's
 * author couldn't personally test or verify against) and Net Radio (never
 * wired up past a placeholder) were removed rather than left as dead/
 * stalled tiles. Its icon (stream_media/subsonic.png/_s.png) isn't from
 * the stock theme pack at all (Subsonic isn't a stock HiBy feature) --
 * see assets.c's THEME_OVERRIDE_ROOT for how this app adds its own new
 * asset on top of the stock resource pack despite that living on
 * read-only storage on a real device. */

/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static int artist_albums_group_count;
/* The compact-list page provider may run on its fetch worker while a rapid
 * drill-down replaces the screen's current artist. Snapshot both fields as
 * one pair so an abandoned fetch never observes a torn query. */
static pthread_mutex_t artist_albums_state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Name/kind of the artist or album artist this screen is currently showing
 * -- set by show_artist_albums(), read back by artist_album_row_click_cb()'s
 * own "All Songs" row (index 0, see that function's own comment) to fetch
 * every song credited to them, flattened across all their albums. */
static char artist_albums_current_name[128];
static metadata_db_group_kind_t artist_albums_current_kind;

static const char * artist_albums_song_filter(void) {
    return artist_albums_current_kind == METADATA_DB_GROUP_ARTIST ? artist_albums_current_name : NULL;
}


/* Now-playing indicator bar -- same "recreated fresh every populate call"
 * lifecycle as group_songs_now_playing_bar (see its own comment):
 * populate_indexed_list()'s lv_obj_clean(artist_albums_list) destroys
 * whatever was here before, same as every row. */
/* This drill-down is a compact list too: fixed row count regardless of how
 * many albums one artist owns, and therefore able to share the exact album
 * artwork decorator/cache used by Music -> Albums. */
/* Sort scratch for artist_albums_show_all_songs() below -- carries the same
 * owned path/title an eventual group_song_entry_t needs, plus just enough
 * of each song's own tags to order the flattened list the same way a real
 * album listing would (see cmp_artist_song_sort_entry()'s own comment).
 * Freed after the final group_song_entry_t[] is built; its path/title
 * pointers are MOVED there, not copied again. */
typedef struct {
    char * path;
    char * title;
    int64_t song_id;
    uint64_t artwork_key;
    char album[128];
    char album_artist[128];
    int32_t disc_number;
    int32_t track_number;
    bool show_disc_header;
} artist_song_sort_entry_t;

/* Sorts songs by album, then album artist, then disc number, track number,
 * and file path. Grouping by both album and album artist prevents collision
 * between same-titled albums from different artists, while sorting by disc
 * and track preserves listening order within each album. */
static int cmp_artist_song_sort_entry(const void * a, const void * b) {
    const artist_song_sort_entry_t * ea = (const artist_song_sort_entry_t *) a;
    const artist_song_sort_entry_t * eb = (const artist_song_sort_entry_t *) b;
    int c = strcasecmp(ea->album, eb->album);
    if (c) return c;
    c = strcasecmp(ea->album_artist, eb->album_artist);
    if (c) return c;
    int32_t da = ea->disc_number > 0 ? ea->disc_number : 1;
    int32_t db = eb->disc_number > 0 ? eb->disc_number : 1;
    if (da != db) return da < db ? -1 : 1;
    int32_t ta = ea->track_number, tb = eb->track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(ea->path, eb->path);
}

/* Fetches every song credited to artist_albums_current_name (flattened
 * across all its albums) and hands it to show_group_songs_take_ownership()
 * -- the "All Songs" row prepended at index 0 by show_artist_albums().
 *
 * Exact track count is queried via metadata_db_get_group_offset() /
 * metadata_db_get_groups_page() so buffer sizing matches accurately. */
#define ARTIST_ALBUMS_ALL_SONGS_CAP 4000

static bool artist_albums_show_all_songs_internal(bool push_screen, const char * artist_name,
                                                    metadata_db_group_kind_t artist_kind) {
    int64_t offset = metadata_db_get_group_offset(artist_kind, artist_name, NULL);
    group_row_t artist_row;
    int real_total = (offset >= 0 && metadata_db_get_groups_page(artist_kind, (int) offset, 1,
                                                                  &artist_row) == 1)
                          ? artist_row.song_count
                          : 0;
    if (real_total <= 0) return false;
    int total = real_total > ARTIST_ALBUMS_ALL_SONGS_CAP ? ARTIST_ALBUMS_ALL_SONGS_CAP : real_total;

    /* calloc, not malloc -- a failure partway through the fetch loop below
     * frees every one of these `total` slots unconditionally (see that
     * branch's own comment), relying on the not-yet-reached ones staying
     * NULL from this zero-init rather than holding garbage. */
    artist_song_sort_entry_t * sort_entries = calloc((size_t) total, sizeof(*sort_entries));
    if (!sort_entries) return false;

    int n = 0;
    bool failed = false;
    song_row_t page[64];
    while (n < total) {
        int want = total - n;
        if (want > 64) want = 64;
        int got = artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                      ? metadata_db_get_album_artist_songs(artist_name, n, page, want)
                      : metadata_db_get_artist_songs(artist_name, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[192];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            artist_song_sort_entry_t * dst = &sort_entries[n + i];
            dst->path = strdup(page[i].path);
            dst->title = strdup(title);
            dst->song_id = page[i].id;
            dst->artwork_key = song_album_thumbnail_key(&page[i]);
            snprintf(dst->album, sizeof(dst->album), "%s", page[i].tags.album);
            snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", page[i].tags.album_artist);
            dst->disc_number = page[i].tags.disc_number;
            dst->track_number = page[i].tags.track_number;
            if (!dst->path || !dst->title) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        n += got;
        if (got < want) break;
    }

    if (failed || n <= 0) {
        /* `total`, not `n` -- covers the current, partially-filled batch
         * (whatever this failure landed on) plus every never-reached slot,
         * all still NULL/zeroed from the calloc above. free(NULL) is a
         * no-op, so this is safe regardless of exactly how far the loop
         * got. */
        for (int i = 0; i < total; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }

    /* Sort songs in disc and track order within each album across the artist's catalog. */
    qsort(sort_entries, (size_t) n, sizeof(*sort_entries), cmp_artist_song_sort_entry);

    /* Mark disc boundaries independently for each contiguous album identity.
     * Untagged tracks belong to disc 1, matching the single-album path. */
    for (int start = 0; start < n; ) {
        int end = start + 1;
        while (end < n && strcasecmp(sort_entries[end].album, sort_entries[start].album) == 0 &&
               strcasecmp(sort_entries[end].album_artist, sort_entries[start].album_artist) == 0)
            end++;
        bool multi_disc = false;
        int first_disc = sort_entries[start].disc_number > 0 ? sort_entries[start].disc_number : 1;
        for (int i = start + 1; i < end; i++) {
            int disc = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
            if (disc != first_disc) { multi_disc = true; break; }
        }
        int previous_disc = first_disc;
        for (int i = start; i < end; i++) {
            int disc = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
            sort_entries[i].show_disc_header = multi_disc && (i == start || disc != previous_disc);
            previous_disc = disc;
        }
        start = end;
    }

    /* Transfer ownership of path and title strings into a group_song_entry_t
     * array for show_group_songs_take_ownership() to avoid duplicate allocations. */
    group_song_entry_t * entries = malloc(sizeof(*entries) * (size_t) n);
    if (!entries) {
        for (int i = 0; i < n; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }
    for (int i = 0; i < n; i++) {
        entries[i].path = sort_entries[i].path;
        entries[i].title = sort_entries[i].title;
        entries[i].song_id = sort_entries[i].song_id;
        entries[i].artwork_key = sort_entries[i].artwork_key;
        entries[i].disc_number = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
        entries[i].show_disc_header = sort_entries[i].show_disc_header;
    }
    free(sort_entries);

    /* Disclose truncation via a toast if the songs exceed the display cap. */
    if (real_total > ARTIST_ALBUMS_ALL_SONGS_CAP) {
        char msg[96];
        snprintf(msg, sizeof(msg), TR("Showing first %d of %d songs"), n, real_total);
        show_info_toast(msg);
    }

    group_songs_refresh_source = GROUP_SONGS_REFRESH_ARTIST;
    if (artist_name != group_songs_refresh_artist_name)
        snprintf(group_songs_refresh_artist_name, sizeof(group_songs_refresh_artist_name), "%s",
                 artist_name);
    group_songs_refresh_artist_kind = artist_kind;
    show_group_songs_take_ownership(artist_name, entries, n, push_screen);
    /* entries is now owned by group_songs_entries -- do not free here. */
    return true;
}

static bool artist_albums_show_all_songs(void) {
    return artist_albums_show_all_songs_internal(true, artist_albums_current_name,
                                                 artist_albums_current_kind);
}

static void artist_album_row_click_cb(int index) {
    if (library_rescan_active) return;
    if (index == 0) {
        (void) artist_albums_show_all_songs();
        return;
    }
    int group_index = index - 1;
    if (group_index < 0 || group_index >= artist_albums_group_count) return;
    group_row_t group;
    if (metadata_db_get_albums_for_group_sorted(artist_albums_current_kind, artist_albums_current_name,
                                                effective_group_album_sort(artist_albums_current_kind), group_index, 1, &group) != 1) return;
    (void) show_album_group_filtered(&group, artist_albums_song_filter());
}

static void hide_collection_menu(void) {
    if (collection_menu_popup) lv_obj_add_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (collection_menu_backdrop) lv_obj_add_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_popup) lv_obj_add_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_backdrop) lv_obj_add_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void collection_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_collection_menu();
}

static void show_collection_menu(bool album) {
    if (library_rescan_active) return;
    lv_obj_t * popup = album ? album_collection_menu_popup : collection_menu_popup;
    lv_obj_t * backdrop = album ? album_collection_menu_backdrop : collection_menu_backdrop;
    if (!popup || !backdrop) return;
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(backdrop);
    lv_obj_move_foreground(popup);
}

static bool play_current_group(play_mode_t mode) {
    if (group_songs_count <= 0) return false;
    char ** paths = calloc((size_t) group_songs_count, sizeof(*paths));
    if (!paths) return false;
    for (int i = 0; i < group_songs_count; i++) {
        paths[i] = strdup(group_songs_entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < group_songs_count; j++) free(paths[j]);
            free(paths);
            return false;
        }
    }
    mode = collection_play_mode(mode);
    int start = (mode == PLAY_MODE_SHUFFLE || mode == PLAY_MODE_SHUFFLE_ONCE)
        ? rand() % group_songs_count : 0;
    gui_player_set_play_mode(mode);
    set_player_source_group_songs(start);
    on_file_selected(paths, group_songs_count, start);
    return true;
}

static bool load_collection_for_playback(void) {
    if (!collection_menu_is_album) {
        /* The artist menu now also opens from the Artists and Album Artists
         * lists, so play the artist the menu was opened for, not whichever
         * artist's album list was visited last. */
        return artist_albums_show_all_songs_internal(true, collection_menu_name,
                                                     collection_menu_artist_kind);
    }
    group_row_t group = {0};
    snprintf(group.name, sizeof(group.name), "%s", collection_menu_name);
    snprintf(group.album_artist, sizeof(group.album_artist), "%s", collection_menu_album_artist);
    group.song_count = collection_menu_song_count;
    return show_album_group_filtered(&group, collection_menu_artist[0] ? collection_menu_artist : NULL);
}

static void collection_play_requested(play_mode_t mode) {
    if (!load_collection_for_playback()) return;
    if (collection_menu_is_album) {
        album_load_request_t * request = album_load_pending ? album_load_pending : album_load_job;
        if (request && request->generation == album_load_generation) request->autoplay_mode = mode;
    } else {
        (void) play_current_group(mode);
    }
}

static void collection_play_shuffled_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    collection_play_requested(PLAY_MODE_SHUFFLE);
}

static void collection_play_sequential_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    collection_play_requested(PLAY_MODE_SEQUENTIAL);
}

static bool collection_song_at(int offset, song_row_t * song) {
    if (collection_menu_is_album) {
        /* collection_menu_song_count is the filtered total when the menu was
         * opened from an Artists drill-down, so the rows must be filtered the
         * same way or the offset indexes a different list. */
        if (collection_menu_artist[0])
            return metadata_db_get_songs_filtered_page(NULL, collection_menu_artist,
                                                       collection_menu_album_artist,
                                                       collection_menu_name, NULL, offset, 1, song) == 1;
        return metadata_db_get_album_songs(collection_menu_name, collection_menu_album_artist,
                                            offset, song, 1) == 1;
    }
    return (collection_menu_artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                ? metadata_db_get_album_artist_songs(collection_menu_name, offset, song, 1)
                : metadata_db_get_artist_songs(collection_menu_name, offset, song, 1)) == 1;
}

static void collection_add_random_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_song_count <= 0) return;
    song_row_t song;
    if (collection_song_at(rand() % collection_menu_song_count, &song))
        gui_player_queue_add(song.path);
}

static void collection_add_album_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    int count = 0;
    group_song_entry_t * entries = load_album_entries_filtered(collection_menu_name,
                                                                collection_menu_album_artist,
                                                                collection_menu_song_count,
                                                                collection_menu_artist[0] ? collection_menu_artist : NULL,
                                                                &count, NULL);
    if (!entries) return;
    const char ** paths = malloc(sizeof(*paths) * (size_t) count);
    if (paths) {
        for (int i = 0; i < count; i++) paths[i] = entries[i].path;
        gui_player_queue_add_many(paths, count);
        free(paths);
    }
    free_group_song_entries(entries, count);
}

static void collection_refresh_metadata_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_is_album)
        start_library_metadata_refresh_album(collection_menu_name, collection_menu_album_artist);
    else
        start_library_metadata_refresh_artist(collection_menu_artist_kind, collection_menu_name);
}

static void collection_reload_cover_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED || !collection_menu_is_album)
        return;
    hide_collection_menu();
    start_library_cover_reload_album(collection_menu_name, collection_menu_album_artist);
}

static void open_album_collection_menu(const group_row_t * group, const char * artist_filter) {
    collection_menu_is_album = true;
    snprintf(collection_menu_artist, sizeof(collection_menu_artist), "%s", artist_filter ? artist_filter : "");
    /* Match the row's own listing, so "add album" never queues tracks the
     * screen does not show. */
    if (collection_menu_artist[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, collection_menu_artist,
                                                            group->album_artist, group->name, NULL);
        collection_menu_song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    } else {
        collection_menu_song_count = group->song_count;
    }
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    snprintf(collection_menu_album_artist, sizeof(collection_menu_album_artist), "%s", group->album_artist);
    show_collection_menu(true);
}

static void open_artist_collection_menu(const group_row_t * group, metadata_db_group_kind_t kind) {
    collection_menu_is_album = false;
    collection_menu_artist_kind = kind;
    collection_menu_song_count = group->song_count;
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    collection_menu_album_artist[0] = '\0';
    collection_menu_artist[0] = '\0';
    show_collection_menu(false);
}

static void open_artist_collection_menu_at(metadata_db_group_kind_t kind,
                                           search_binding_id_t binding_id, int index) {
    index = search_remap_index(binding_id, index);
    group_row_t group;
    if (metadata_db_get_groups_page(kind, index, 1, &group) == 1)
        open_artist_collection_menu(&group, kind);
}

static void artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

static void artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

static void album_artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

static void album_artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

static void open_album_collection_menu_at(int index) {
    group_row_t group;
    if (resolve_album_display_row(index, &group))
        open_album_collection_menu(&group, NULL);
}

static void album_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

static void album_row_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

static void artist_album_more_click_cb(int index) {
    if (library_rescan_active) return;
    if (index > 0) {
        int group_index = index - 1;
        group_row_t group;
        if (group_index >= 0 && group_index < artist_albums_group_count &&
            metadata_db_get_albums_for_group_sorted(artist_albums_current_kind, artist_albums_current_name,
                                                     effective_group_album_sort(artist_albums_current_kind), group_index, 1, &group) == 1)
            open_album_collection_menu(&group, artist_albums_song_filter());
        return;
    }
    int64_t offset = metadata_db_get_group_offset(artist_albums_current_kind,
                                                   artist_albums_current_name, NULL);
    group_row_t group;
    if (offset < 0 || metadata_db_get_groups_page(artist_albums_current_kind,
                                                   (int) offset, 1, &group) != 1) return;
    open_artist_collection_menu(&group, artist_albums_current_kind);
}

static void build_collection_menus(void) {
    const menu_popup_row_t rows[] = {
        { TR("Play all shuffled"), collection_play_shuffled_cb, false },
        { TR("Play sequentially"), collection_play_sequential_cb, false },
        { TR("Add a random song to queue"), collection_add_random_cb, false },
        { TR("Refresh metadata"), collection_refresh_metadata_cb, false },
        { TR("Cancel"), collection_menu_backdrop_cb, false, true },
    };
    const menu_popup_row_t album_rows[] = {
        { TR("Play all shuffled"), collection_play_shuffled_cb, false },
        { TR("Play sequentially"), collection_play_sequential_cb, false },
        { TR("Add a random song to queue"), collection_add_random_cb, false },
        { TR("Add album to queue"), collection_add_album_cb, false },
        { TR("Reload cover"), collection_reload_cover_cb, false },
        { TR("Refresh metadata"), collection_refresh_metadata_cb, false },
        { TR("Cancel"), collection_menu_backdrop_cb, false, true },
    };
    collection_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                               collection_menu_backdrop_cb,
                                               &collection_menu_backdrop);
    album_collection_menu_popup = build_menu_popup(album_rows,
                                                     (int) (sizeof(album_rows) / sizeof(album_rows[0])),
                                                     collection_menu_backdrop_cb,
                                                     &album_collection_menu_backdrop);
}

/* A direct lv_image_dsc_t points into the bounded LRU's pixel allocation.
 * Refresh on every screen load so a list that was hidden while the other
 * album screen populated/evicted cache entries never redraws an old row
 * against an evicted descriptor. This also immediately picks up artwork
 * decoded while the list was off-screen. */
void refresh_artist_albums_now_playing_indicator(void) {
    if (!artist_albums_list) return;
    int32_t current_revision;
    bool have_revision = metadata_db_try_get_generation(&current_revision);
    int match = -1;
    pthread_mutex_lock(&now_playing_lookup_mutex);
    const now_playing_lookup_result_t * result = &now_playing_lookup_applied_result;
    if (now_playing_lookup_applied_valid &&
        strcmp(result->request.path, now_playing_path) == 0 &&
        (!have_revision || result->library_revision == current_revision) &&
        result->request.artist_albums_kind == artist_albums_current_kind &&
        result->request.artist_albums_sort == requested_group_album_sort(artist_albums_current_kind) &&
        strcmp(result->request.artist_albums_name, artist_albums_current_name) == 0 &&
        result->artist_albums_row > 0 &&
        result->artist_albums_row <= artist_albums_group_count)
        match = result->artist_albums_row;
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    compact_list_set_now_playing(artist_albums_list, match);
}

/* Paged artist-album rows keep the complete result set out of memory. Row
 * zero is the synthetic All Songs action; database offset zero starts at
 * display index one. */
static int artist_albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    char current_name[sizeof(artist_albums_current_name)];
    metadata_db_group_kind_t current_kind;
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(current_name, sizeof(current_name), "%s", artist_albums_current_name);
    current_kind = artist_albums_current_kind;
    metadata_db_album_sort_t current_sort = artist_albums_current_sort;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    int prefix = 0;
    if (offset == 0 && count > 0) {
        memset(&out_rows[0], 0, sizeof(out_rows[0]));
        snprintf(out_rows[0].label, sizeof(out_rows[0].label), "%s", TR("All Songs"));
        snprintf(out_rows[0].trailing_asset, sizeof(out_rows[0].trailing_asset), "%s", "navigation/more.png");
        prefix = 1;
        count--;
    } else {
        offset--;
    }
    if (count <= 0) return prefix;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) count);
    int n = rows ? metadata_db_get_albums_for_group_sorted(current_kind, current_name,
                                                            current_sort, offset,
                                                            count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[prefix + i].label, sizeof(out_rows[prefix + i].label), "%s", rows[i].name);
        out_rows[prefix + i].identity = rows[i].first_song_id;
        if (rows[i].album_artist[0]) {
            albumart_info_t info = {0};
            snprintf(info.albumartist, sizeof(info.albumartist), "%s", rows[i].album_artist);
            snprintf(info.album, sizeof(info.album), "%s", rows[i].name);
            out_rows[prefix + i].artwork_key = albumart_thumbnail_key(&info);
        } else {
            song_row_t representative;
            if (metadata_db_get_song_by_id(rows[i].first_song_id, &representative))
                out_rows[prefix + i].artwork_key = song_album_thumbnail_key(&representative);
        }
        snprintf(out_rows[prefix + i].trailing_asset, sizeof(out_rows[prefix + i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n + prefix;
}

static void populate_artist_albums_view(const char * name, metadata_db_group_kind_t kind) {
    metadata_db_album_sort_t sort = effective_group_album_sort(kind);
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(artist_albums_current_name, sizeof(artist_albums_current_name), "%s", name);
    artist_albums_current_kind = kind;
    artist_albums_current_sort = sort;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    artist_albums_group_count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;

    lv_label_set_text(artist_albums_title_label, name);
    compact_list_set_paged_provider(artist_albums_list, artist_albums_fetch_page, NULL,
                                    artist_albums_group_count + 1);
    compact_list_set_trailing_click(artist_albums_list, artist_album_more_click_cb);
    request_now_playing_indicator_lookup();
    refresh_artist_albums_now_playing_indicator();
}

void show_artist_albums(const char * name, metadata_db_group_kind_t kind) {
    populate_artist_albums_view(name, kind);
    nav_push(artist_albums_screen);
}

static bool library_screen_in_navigation(lv_obj_t * screen) {
    if (!screen || lv_screen_active() == screen) return screen != NULL;
    int depth = gui_navigation_get_depth();
    for (int i = 0; i < depth; i++)
        if (gui_navigation_get_screen_at(i) == screen) return true;
    return false;
}

static void refresh_group_songs_paths_in_place(void) {
    group_song_entry_t * entries = calloc((size_t)(group_songs_count > 0 ? group_songs_count : 1),
                                          sizeof(*entries));
    if (!entries) {
        set_group_songs_entries_owned(NULL, 0);
        populate_group_songs_rows();
        return;
    }
    int count = 0;
    for (int i = 0; i < group_songs_count; i++) {
        song_row_t song;
        if (!metadata_db_get_song_by_path(group_songs_entries[i].path, &song)) continue;
        char title[384];
        if (group_songs_music_submenu) format_music_submenu_identity(&song, title, sizeof(title));
        else format_song_identity(&song, title, sizeof(title));
        entries[count].path = strdup(song.path);
        entries[count].title = strdup(title);
        entries[count].song_id = song.id;
        entries[count].artwork_key = song_album_thumbnail_key(&song);
        entries[count].disc_number = group_songs_entries[i].disc_number;
        entries[count].show_disc_header = group_songs_entries[i].show_disc_header;
        if (!entries[count].path || !entries[count].title) {
            free_group_song_entries(entries, count + 1);
            set_group_songs_entries_owned(NULL, 0);
            populate_group_songs_rows();
            return;
        }
        count++;
    }
    set_group_songs_entries_owned(entries, count);
    populate_group_songs_rows();
}

static void refresh_group_songs_from_database(void) {
    group_songs_page_start = 0;
    switch (group_songs_refresh_source) {
        case GROUP_SONGS_REFRESH_FAVORITES: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_favorite_songs(&paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_MOST_PLAYED: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_M3U: {
            char ** paths = NULL;
            int count = 0;
            bool loaded = group_songs_refresh_m3u_path[0] &&
                          playlist_files_read(group_songs_refresh_m3u_path, &paths, &count);
            group_song_entry_t * entries = loaded && count ? build_group_song_entries_from_paths(paths, count) : NULL;
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            group_songs_file_stat_valid = loaded &&
                stat(group_songs_refresh_m3u_path, &group_songs_file_stat) == 0;
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ALBUM: {
            int64_t count64 = 0;
            if (group_songs_refresh_artist_filter[0]) {
                count64 = metadata_db_count_songs_filtered(NULL, group_songs_refresh_artist_filter,
                    group_songs_refresh_album_artist, group_songs_refresh_album, NULL);
            } else {
                int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                    group_songs_refresh_album, group_songs_refresh_album_artist);
                group_row_t group;
                if (offset >= 0 && metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM,
                        (int) offset, 1, &group) == 1) count64 = group.song_count;
            }
            int count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;
            int loaded_count = 0;
            group_song_entry_t * entries = load_album_entries_filtered(group_songs_refresh_album,
                group_songs_refresh_album_artist, count, group_songs_refresh_artist_filter, &loaded_count, NULL);
            set_group_songs_entries(entries, entries ? loaded_count : 0);
            free_group_song_entries(entries, loaded_count);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ARTIST:
            if (!artist_albums_show_all_songs_internal(false, group_songs_refresh_artist_name,
                                                       group_songs_refresh_artist_kind)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_GENRE:
            if (!show_genre_songs_internal(false, group_songs_refresh_genre)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_NONE:
        default:
            refresh_group_songs_paths_in_place();
            break;
    }
}

static void refresh_targeted_nested_views(void) {
    hide_collection_menu();
    if (library_screen_in_navigation(artist_albums_screen)) {
        char name[sizeof(artist_albums_current_name)];
        metadata_db_group_kind_t kind;
        pthread_mutex_lock(&artist_albums_state_mutex);
        snprintf(name, sizeof(name), "%s", artist_albums_current_name);
        kind = artist_albums_current_kind;
        pthread_mutex_unlock(&artist_albums_state_mutex);
        if (name[0]) populate_artist_albums_view(name, kind);
    }
    if (library_screen_in_navigation(group_songs_screen)) refresh_group_songs_from_database();
}

/* Paged Artists/Album Artist -- see build_artists_screen()'s own comment.
 * offset is a position in the DB's own name-sorted order (ORDER BY artist/
 * album_artist COLLATE NOCASE) -- a row tapped here (artist_row_click_cb()/
 * album_artist_row_click_cb() below) re-resolves that same offset via
 * metadata_db_get_groups_page() itself to get the tapped name, rather than
 * caching anything from this page fetch. */
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].artwork_key = albumart_artist_thumbnail_key(rows[i].name);
        snprintf(out_rows[i].artwork_name, sizeof(out_rows[i].artwork_name), "%s", rows[i].name);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n;
}

static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].artwork_key = albumart_artist_thumbnail_key(rows[i].name);
        snprintf(out_rows[i].artwork_name, sizeof(out_rows[i].artwork_name), "%s", rows[i].name);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n;
}

/* Diagnostic logging helper for teardown and initialization steps. */
static void library_teardown_diag(const char * step);

void gui_library_init(void) {
    pthread_mutex_lock(&now_playing_lookup_mutex);
    now_playing_lookup_lifetime++;
    now_playing_lookup_generation++;
    library_ui_live = true;
    now_playing_lookup_pending = false;
    now_playing_lookup_result_ready = false;
    now_playing_lookup_result_valid = false;
    now_playing_lookup_applied_valid = false;
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    library_thumbnails_init(current_settings.show_artist_images, library_thumbnail_policy,
                            library_thumbnail_genre_context, detach_group_song_thumbnail,
                            refresh_group_thumbnail_binding);
    gui_playlists_init(library_playlist_open_row, library_reload_edited_playlist_if_changed,
                       ensure_playlists_dir);
    gui_folder_actions_init(file_folder_job_storage_invalidated,
                            gui_playlists_open_for_folder, file_folder_show_page_jump);
    gui_folder_actions_build_menu();
    library_teardown_diag("az_index_drag_timer before");
    library_az_index_init(find_pointer_indev, az_index_strip_allowed);
    library_teardown_diag("build_files_screen before");
    files_screen = build_files_screen();
    library_teardown_diag("build_all_songs_screen before");
    all_songs_screen = build_all_songs_screen();
    library_teardown_diag("build_recently_added_screen before");
    recently_added_screen = build_recently_added_screen();
    library_teardown_diag("build_artists_screen before");
    artists_screen = build_artists_screen();
    library_teardown_diag("build_albums_screen before");
    albums_screen = build_albums_screen();
    library_teardown_diag("build_album_artist_screen before");
    album_artist_screen = build_album_artist_screen();
    library_teardown_diag("build_genres_screen before");
    genres_screen = build_genres_screen();
    genre_groups_screen = build_genre_browse_screen(&genre_groups_list, &genre_groups_title_label,
                                                    genre_groups_row_click_cb);
    genre_albums_screen = build_genre_browse_screen(&genre_albums_list, &genre_albums_title_label,
                                                    genre_albums_row_click_cb);
    library_teardown_diag("build_playlists_screen before");
    gui_playlists_build_saved_screen();
    gui_playlists_build_context_menu();
    library_teardown_diag("build_cue_tracks_screen before");
    cue_tracks_screen = build_cue_tracks_screen();
    library_teardown_diag("build_group_songs_screen before");
    group_songs_screen = build_group_songs_screen();

    library_teardown_diag("build_compact_list_screen(Albums) before");
    artist_albums_screen = build_compact_list_screen(TR("Albums"), generic_back_cb, NULL, 0,
                                                      artist_album_row_click_cb, NULL,
                                                      &artist_albums_list, &artist_albums_title_label,
                                                      LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    library_teardown_diag("compact_list_set_row_height before");
    compact_list_set_row_height(artist_albums_list, MUSIC_LIST_ROW_HEIGHT);
    library_teardown_diag("artist_albums event cbs before");
    library_thumbnails_bind_list(artist_albums_screen, artist_albums_list,
                                 LIBRARY_THUMB_LIST_ARTIST_ALBUMS);
    library_teardown_diag("finalize_screen_navigation(artist_albums) before");
    finalize_screen_navigation(artist_albums_screen);
    library_teardown_diag("build_add_to_playlist_screen before");
    gui_playlists_build_add_screen();
    library_teardown_diag("build_music_screen before");
    music_screen = build_music_screen();

    /* Register A-Z index & search */
    library_teardown_diag("register_az_index(artists) before");
    library_az_index_register(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    library_teardown_diag("register_az_index(albums) before");
    library_az_index_register(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    library_teardown_diag("register_az_index(album_artist) before");
    library_az_index_register(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    library_teardown_diag("register_az_index(all_songs) before");
    library_az_index_register(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    library_teardown_diag("register_search(artists) before");
    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    library_teardown_diag("register_search(albums) before");
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    library_teardown_diag("register_search(album_artist) before");
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    library_teardown_diag("register_search(all_songs) before");
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);
    library_teardown_diag("register_search(recently_added) before");
    register_search(SEARCH_BINDING_RECENTLY_ADDED, recently_added_screen, recently_added_list, NULL, NULL, false,
                    true, METADATA_DB_AZ_ALL_SONGS, recently_added_fetch_page);

    library_teardown_diag("build_compact_list_widget(files_search) before");
    files_search_list = build_compact_list_widget(files_screen, NULL, 0, files_search_row_click_cb, NULL, LIST_ROW_WIDTH_WIDE, false, NULL);
    lv_obj_set_style_bg_opa(files_search_list, LV_OPA_COVER, 0);
    lv_obj_add_style(files_search_list, &style_theme_screen_bg, 0);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    library_thumbnails_bind_list(files_screen, files_search_list, LIBRARY_THUMB_LIST_FILES_SEARCH);
    library_teardown_diag("enable_gesture_bubble_recursive(files_search_list) before");
    enable_gesture_bubble_recursive(files_search_list);

    library_teardown_diag("register_search(files) before");
    register_search(SEARCH_BINDING_FILES, files_screen, files_search_list, NULL, NULL, true,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    library_teardown_diag("build_compact_list_widget(group_songs_search) before");
    group_songs_search_list = build_compact_list_widget(group_songs_screen, NULL, 0,
                                                        group_songs_search_row_click_cb,
                                                        group_songs_search_row_long_press_cb,
                                                        LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    lv_obj_set_style_bg_opa(group_songs_search_list, LV_OPA_COVER, 0);
    lv_obj_add_style(group_songs_search_list, &style_theme_screen_bg, 0);
    lv_obj_add_flag(group_songs_search_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(group_songs_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    library_thumbnails_bind_list(group_songs_screen, group_songs_search_list,
                                 LIBRARY_THUMB_LIST_GROUP_SONGS_SEARCH);
    library_teardown_diag("enable_gesture_bubble_recursive(group_songs_search_list) before");
    enable_gesture_bubble_recursive(group_songs_search_list);

    library_teardown_diag("register_search(group_songs) before");
    register_search(SEARCH_BINDING_GROUP_SONGS, group_songs_screen, group_songs_search_list,
                    group_songs_search_name_of, &group_songs_count, true, false,
                    METADATA_DB_AZ_ALL_SONGS, NULL);
    update_group_songs_search_button(false);

    library_teardown_diag("build_sd_mount_failed_popup before");
    build_sd_mount_failed_popup();
    library_teardown_diag("build_sd_format_confirm_popup before");
    build_sd_format_confirm_popup();
    library_teardown_diag("build_collection_menus before");
    build_collection_menus();
    library_teardown_diag("build_library_update_prompt_popup before");
    build_library_update_prompt_popup();
    library_teardown_diag("gui_library_init done");
    refresh_now_playing_indicators();
}

void gui_library_set_artist_images_enabled(bool enabled) {
    library_thumbnails_set_artist_images_enabled(enabled);
    gui_navigation_invalidate_theme_snapshots();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_library_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * the A-Z drag timer (already guarded/reused correctly by library_az_index_
 * init() itself). gui_library_init() resets A-Z bindings before registering
 * the rebuilt screens, and search registration maintains its own prior state.
 * sd_mount_failed_popup/sd_format_confirm_popup
 * and their backdrops are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so they need their own explicit deletion. */
/* Diagnostic logging helper writing teardown and init steps to reload_diag.log
 * with fsync per line to pinpoint failures during theme reload. */
static void library_teardown_diag(const char * step) {
    if (!db_log_enabled()) return;
    (void) mkdir("/data/mnt/sd_0/.logs", 0755);
    int fd = open("/data/mnt/sd_0/.logs/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[96];
    int len = snprintf(line, sizeof(line), "[pid=%ld]   gui_library_teardown: %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(fd, line, (size_t) len);
        fsync(fd);
    }
    close(fd);
}

void gui_library_teardown(void) {
    pthread_mutex_lock(&now_playing_lookup_mutex);
    library_ui_live = false;
    now_playing_lookup_lifetime++;
    now_playing_lookup_generation++;
    now_playing_lookup_pending = false;
    now_playing_lookup_result_ready = false;
    now_playing_lookup_result_valid = false;
    now_playing_lookup_applied_valid = false;
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    gui_folder_actions_stop_work();
    library_setup_scan_complete = NULL;
    cancel_album_load();
    cancel_group_song_probes();
    library_az_index_reset_bindings();
    library_prompt_requeue_visible();
    gui_popup_teardown(&library_update_prompt_popup);
    gui_folder_actions_teardown();
    gui_playlist_target_clear_folder();
    gui_playlists_reset_name_target_mode();
    gui_popup_teardown(&refresh_all_metadata_popup);
    gui_popup_teardown(&refresh_all_covers_popup);
    library_update_prompt_title = NULL;
    library_update_prompt_body = NULL;
    library_update_prompt_confirm_label = NULL;
    if (playlist_start_popup) { lv_obj_delete(playlist_start_popup); playlist_start_popup = NULL; }
    if (playlist_start_backdrop) { lv_obj_delete(playlist_start_backdrop); playlist_start_backdrop = NULL; }
    gui_playlists_teardown_popups();
    library_teardown_diag("power_action_overlay before");
    gui_power_hub_teardown();
    library_teardown_diag("sd_mount_failed_popup before");
    gui_popup_teardown(&sd_mount_failed_popup);
    library_teardown_diag("sd_format_confirm_popup before");
    gui_popup_teardown(&sd_format_confirm_popup);
    library_teardown_diag("collection_menu_popup before");
    if (collection_menu_popup) { lv_obj_delete(collection_menu_popup); collection_menu_popup = NULL; }
    library_teardown_diag("collection_menu_backdrop before");
    if (collection_menu_backdrop) { lv_obj_delete(collection_menu_backdrop); collection_menu_backdrop = NULL; }
    library_teardown_diag("album_collection_menu_popup before");
    if (album_collection_menu_popup) { lv_obj_delete(album_collection_menu_popup); album_collection_menu_popup = NULL; }
    library_teardown_diag("album_collection_menu_backdrop before");
    if (album_collection_menu_backdrop) {
        lv_obj_delete(album_collection_menu_backdrop);
        album_collection_menu_backdrop = NULL;
    }
    library_teardown_diag("music_screen before");
    if (music_screen) { lv_obj_delete(music_screen); music_screen = NULL; }
    library_teardown_diag("files_screen before");
    if (files_screen) { lv_obj_delete(files_screen); files_screen = NULL; }
    library_teardown_diag("all_songs_screen before");
    if (all_songs_screen) { lv_obj_delete(all_songs_screen); all_songs_screen = NULL; }
    library_teardown_diag("recently_added_screen before");
    if (recently_added_screen) { lv_obj_delete(recently_added_screen); recently_added_screen = NULL; }
    library_teardown_diag("artists_screen before");
    if (artists_screen) { lv_obj_delete(artists_screen); artists_screen = NULL; }
    library_teardown_diag("albums_screen before");
    if (albums_screen) { lv_obj_delete(albums_screen); albums_screen = NULL; }
    library_teardown_diag("album_artist_screen before");
    if (album_artist_screen) { lv_obj_delete(album_artist_screen); album_artist_screen = NULL; }
    library_teardown_diag("genres_screen before");
    if (genres_screen) { lv_obj_delete(genres_screen); genres_screen = NULL; }
    if (genre_groups_screen) { lv_obj_delete(genre_groups_screen); genre_groups_screen = NULL; }
    if (genre_albums_screen) { lv_obj_delete(genre_albums_screen); genre_albums_screen = NULL; }
    if (genre_view_screen) { lv_obj_delete(genre_view_screen); genre_view_screen = NULL; }
    memset(genre_view_options, 0, sizeof(genre_view_options));
    library_teardown_diag("group_songs_screen before");
    if (group_songs_screen) { lv_obj_delete(group_songs_screen); group_songs_screen = NULL; }
    library_teardown_diag("artist_albums_screen before");
    if (artist_albums_screen) { lv_obj_delete(artist_albums_screen); artist_albums_screen = NULL; }
    library_teardown_diag("playlists_screen before");
    gui_playlists_teardown_saved_screen();
    library_teardown_diag("cue_tracks_screen before");
    if (cue_tracks_screen) { lv_obj_delete(cue_tracks_screen); cue_tracks_screen = NULL; }
    library_teardown_diag("add_to_playlist_screen before");
    gui_playlists_teardown_add_screen();
    /* The screens owned these children. Forget pointer identities before
     * clearing them; the component never dereferences them here. */
    library_thumbnails_forget_list(files_search_list);
    library_thumbnails_forget_list(group_songs_search_list);
    library_thumbnails_forget_list(all_songs_list);
    library_thumbnails_forget_list(recently_added_list);
    library_thumbnails_forget_list(artists_list);
    library_thumbnails_forget_list(albums_list);
    library_thumbnails_forget_list(album_artist_list);
    library_thumbnails_forget_list(genre_groups_list);
    library_thumbnails_forget_list(genre_albums_list);
    library_thumbnails_forget_list(group_songs_list);
    library_thumbnails_forget_list(artist_albums_list);
    /* The screens owned these children; clear the borrowed pointers with
     * their parents so non-NULL remains a valid liveness check. */
    files_search_list = NULL;
    group_songs_search_list = NULL;
    all_songs_list = NULL;
    recently_added_list = NULL;
    artists_list = NULL;
    albums_list = NULL;
    album_artist_list = NULL;
    genres_list = NULL;
    genre_groups_list = genre_groups_title_label = NULL;
    genre_albums_list = genre_albums_title_label = NULL;
    group_songs_list = NULL;
    /* Borrowed children of the deleted group songs list; a later thumbnail
     * refresh after the UI rebuild must not reach them. */
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
    group_songs_title_label = NULL;
    group_songs_edit_btn = NULL;
    group_songs_now_playing_bar = NULL;
    artist_albums_list = NULL;
    artist_albums_title_label = NULL;
    cue_tracks_list = NULL;
    cue_tracks_title_label = NULL;
    /* Power Hub objects live on lv_layer_sys() and must be torn down with
     * the rebuilt library UI. Keep this teardown call in its existing slot. */
    library_teardown_diag("power_action_overlay before");
    gui_power_hub_teardown();
    library_teardown_diag("done");
}

void gui_library_resume_fast_timers(void) {
    library_az_index_resume_timer();
}

void gui_library_reset_drag_state(void) {
    /* Safe to clear even mid-press: this pauses the timer below, and it is
     * only ever resumed on a raw press-DOWN edge (gui_shell.c's indev
     * hooks), so the remainder of an in-flight press cannot be re-judged at
     * a drifted position. Leaving it set would instead disable the strip
     * for the whole of the NEXT press. */
    library_az_index_reset_drag();
}

void gui_library_cancel_scan(void) {
    library_scan_request_cancel();
    if (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM)
        library_thumbnails_cancel_cover_reload();
}

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once discovered_count
 * is known (library_scan_once(), before its spool-reading loop starts);
 * _done is advanced by that same loop, one file at a time (see
 * scan_one_song_into_db()'s own caller in library_scan_once()). */

/* Refreshes the persistent playlist cache (metadata_db.c) from
 * PLAYLISTS_DIR only (the SD card's Playlists folder), not a walk of the
 * whole music tree. Folded into library_scan_once() so it runs on Update
 * Music Database. Ordinary in-app create/delete update the cache with a
 * single insert/delete instead of calling this. */
void gui_library_poll_playlists(void) {
    gui_playlists_poll();
}

static void rescan_playlists(void) {
    /* Runs before library_scan_once()'s own mount check: never prune or
     * create folders against an unmounted, empty mount point. */
    bool mounted = sd_card_root_is_mounted();
    if (mounted) ensure_playlists_dir();
    playlist_files_reconcile(PLAYLISTS_DIR, mounted);
}

static metadata_db_load_outcome_t reload_library_cache(void) {
    library_scan_reset_progress();
    return metadata_db_reload();
}

/* Loads saved metadata without scanning and queues one UI report. */
void library_load_from_cache_only(void) {
    library_prompt_prepare_cache_load_card();
    library_cache_load_card_generation = library_prompt_card_generation;
    library_cache_load_announce_loaded = false;
    library_cache_load_outcome = reload_library_cache();
    library_cache_load_generation++;
    library_cache_load_pending = true;
}

/* Mark the boot warmer for the idle tick after the screen settles. */
void gui_library_start_boot_thumbnail_warmup(void) {
    if (boot_warmup_requested) return;
    boot_warmup_requested = true;
    boot_warmup_pending = true;
}

bool gui_library_has_background_work(void) {
    library_thumbnail_status_t thumbnails;
    library_thumbnails_get_status(&thumbnails);
    return gui_folder_actions_busy() || library_rescan_active || library_rescan_success_pending ||
           album_load_job || album_load_pending || thumbnails.lazy_active ||
           thumbnails.warmup_active || thumbnails.cover_active || sd_format_active || library_search_has_active_job() ||
           remote_collection_job || (group_probe_job && !track_probe_job_done(group_probe_job));
}

bool gui_library_navigation_blocked(void) {
    /* These operations use a modal progress/completion screen. Thumbnail
     * decoding, post-scan cache warming, and search are deliberately absent:
     * they are cancellable/optional workers and do not own the active screen. */
    return gui_folder_actions_busy() || library_rescan_active || library_rescan_success_pending || sd_format_active;
}

void gui_library_prepare_for_ui_reload(bool preserve_artwork) {
    gui_library_cancel_remote_collection();
#ifdef UI_PERF_TRACE
    library_thumbnail_status_t before_status;
    library_thumbnails_get_status(&before_status);
    unsigned decoded_before = (unsigned)before_status.decoded_cache_entries;
#endif
    cancel_album_load();
    cancel_group_song_probes();
    bool cover_reload = library_rescan_active &&
        (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
         library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM);
    if (cover_reload) {
        gui_library_cancel_scan();
        /* Join the component-owned cover runner before any list can be deleted. */
        library_thumbnails_join_cover_abort();
        library_rescan_active = false;
        gui_player_cancel_cover_reload();
        gui_busy_hide(library_rescan_token);
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    }
    library_thumbnails_quiesce();
    /* The decoded LRU and alias records are independent of the screen tree.
     * Workers are joined and their epochs invalidated above, so their results
     * cannot target deleted list objects. Keep the owned pixels for a layout
     * reload when the library itself is unchanged; full reload callers retain
     * the previous clear behavior. */
    if (!preserve_artwork) library_thumbnails_clear_cache();
#ifdef UI_PERF_TRACE
    library_thumbnail_status_t after_status;
    library_thumbnails_get_status(&after_status);
    unsigned decoded_after = (unsigned)after_status.decoded_cache_entries;
    printf("PERF artwork_reload preserved=%d decoded_before=%u decoded_after=%u\n",
           preserve_artwork, decoded_before, decoded_after);
#endif

    library_search_quiesce_for_ui_reload();
}

/* Frees the decoded thumbnail cache (up to about 2.5 MiB) before an external
 * session. Rows decode their thumbnails again from the sized files on disk as
 * lists are shown, exactly as after any library refresh. */
void gui_library_release_memory_for_external_session(void) {
    library_thumbnails_release_memory();
}

void gui_library_cancel_background_work(void) {
    remote_collection_cancel_and_finish();
    cancel_album_load();
    cancel_group_song_probes();
    if (group_probe_job) {
        track_probe_job_cancel_and_destroy(group_probe_job);
        group_probe_job = NULL;
    }
    library_thumbnails_quiesce();
    if (library_rescan_active) {
        bool cover_reload = library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
                            library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
        if (cover_reload) gui_library_cancel_scan();
        if (cover_reload) library_thumbnails_join_cover_abort();
        else pthread_join(library_rescan_thread, NULL);
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        if (cover_reload) {
            gui_player_cancel_cover_reload();
            library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        }
    }
    if (sd_format_active) {
        pthread_join(sd_format_thread, NULL);
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
    }
    library_search_join_for_shutdown();
}

lv_obj_t * gui_library_get_music_screen(void) { return music_screen; }
lv_obj_t * gui_library_get_files_screen(void) { return files_screen; }
lv_obj_t * gui_library_get_all_songs_screen(void) { return all_songs_screen; }
lv_obj_t * gui_library_get_recently_added_screen(void) { return recently_added_screen; }
lv_obj_t * gui_library_get_artists_screen(void) { return artists_screen; }
lv_obj_t * gui_library_get_albums_screen(void) { return albums_screen; }
lv_obj_t * gui_library_get_album_artist_screen(void) { return album_artist_screen; }
lv_obj_t * gui_library_get_genres_screen(void) { return genres_screen; }
lv_obj_t * gui_library_get_group_songs_screen(void) { return group_songs_screen; }
lv_obj_t * gui_library_get_playlists_screen(void) { return gui_playlists_get_saved_screen(); }
