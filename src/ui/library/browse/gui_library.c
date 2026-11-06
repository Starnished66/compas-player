#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "gui_library.h"
#include "gui_library_internal.h"
#include "gui_folder_actions.h"
#include "../../network/subsonic/gui_subsonic.h"
#include "gui_album_header.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "gui_artist_data.h"
#include "gui_artist_page.h"
#include "gui_page_durations.h"
#include "../../../library/metadata/durations/duration_prune.h"
#include "../../../library/metadata/durations/duration_cache.h"
#include "../../../core/process/worker_thread.h"
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
#include "../../../network/bluetooth/control/bluetooth_control.h"
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
#include "gui_library_async.h"
#include "../../widgets/lists/scroll_blit.h"
#include "../../diagnostics/ui_frame_trace.h"
#include <strings.h>
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
#include <sys/resource.h>

#define COMPACT_LIST_PAGE_CACHE_SIZE 64
/* STATUS_BAR_CLEARANCE is provided by screen_builders.h. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)


static metadata_db_album_sort_t configured_album_order(int mode, bool reverse, int maximum) {
    if (mode < 0 || mode > maximum) mode = 0;
    return (metadata_db_album_sort_t)(mode | (reverse ? METADATA_DB_ALBUM_SORT_REVERSE : 0));
}

/* Scalar settings snapshots also identify asynchronous work and stale results. */
static metadata_db_album_sort_t requested_album_sort(void) {
    return configured_album_order(current_settings.album_sort_mode, current_settings.album_sort_reverse,
                                  METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR);
}

metadata_db_album_sort_t effective_album_sort(void) {
    metadata_db_album_sort_t sort = requested_album_sort();
    return metadata_db_album_sort_available(sort) ? sort :
        (metadata_db_album_sort_t)(sort & METADATA_DB_ALBUM_SORT_REVERSE);
}

metadata_db_album_sort_t requested_group_album_sort(metadata_db_group_kind_t kind) {
    bool artist = kind == METADATA_DB_GROUP_ARTIST;
    return configured_album_order(artist ? current_settings.artist_albums_sort_mode :
                                           current_settings.album_artist_albums_sort_mode,
                                  artist ? current_settings.artist_albums_sort_reverse :
                                           current_settings.album_artist_albums_sort_reverse,
                                  METADATA_DB_ALBUM_SORT_YEAR);
}

metadata_db_album_sort_t effective_group_album_sort(metadata_db_group_kind_t kind) {
    metadata_db_album_sort_t sort = requested_group_album_sort(kind);
    return metadata_db_album_sort_available(sort) ? sort :
        (metadata_db_album_sort_t)(sort & METADATA_DB_ALBUM_SORT_REVERSE);
}

metadata_db_album_sort_t gui_library_group_album_sort(metadata_db_group_kind_t kind) {
    return effective_group_album_sort(kind);
}

pthread_mutex_t now_playing_lookup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t now_playing_lookup_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t now_playing_lookup_once = PTHREAD_ONCE_INIT;
static now_playing_lookup_request_t now_playing_lookup_request;
static now_playing_lookup_result_t now_playing_lookup_result;
now_playing_lookup_result_t now_playing_lookup_applied_result;
static uint64_t now_playing_lookup_generation;
static uint64_t now_playing_lookup_lifetime;
static bool now_playing_lookup_pending;
static bool now_playing_lookup_result_ready;
static bool now_playing_lookup_result_valid;
bool now_playing_lookup_applied_valid;
static bool now_playing_lookup_worker_started;
static bool library_ui_live;

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
                    ? request.album_sort : (metadata_db_album_sort_t)(request.album_sort & METADATA_DB_ALBUM_SORT_REVERSE);
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

void request_now_playing_indicator_lookup(void) {
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
    metadata_db_group_kind_t artist_albums_kind;
    artist_albums_snapshot(&artist_albums_kind, now_playing_lookup_request.artist_albums_name,
                           sizeof(now_playing_lookup_request.artist_albums_name), NULL);
    now_playing_lookup_request.artist_albums_kind = artist_albums_kind;
    now_playing_lookup_request.artist_albums_sort = requested_group_album_sort(artist_albums_kind);
    now_playing_lookup_pending = now_playing_lookup_worker_started;
    now_playing_lookup_result_ready = false;
    now_playing_lookup_result_valid = false;
    pthread_cond_signal(&now_playing_lookup_cond);
    pthread_mutex_unlock(&now_playing_lookup_mutex);
}
static void music_files_tile_cb(lv_event_t * e);

/* Screen pointers owned by this module */
static lv_obj_t * music_screen = NULL;
static lv_obj_t * files_screen = NULL;
static lv_obj_t * files_search_list = NULL;
static file_browser_t * music_files_browser = NULL;
static lv_obj_t * files_title_label = NULL;
static lv_obj_t * files_options_btn = NULL;

lv_obj_t * all_songs_screen = NULL;
lv_obj_t * all_songs_list = NULL;

lv_obj_t * recently_added_screen = NULL;
lv_obj_t * recently_added_list = NULL;

static lv_obj_t * artists_screen = NULL;
lv_obj_t * artists_list = NULL;

static lv_obj_t * albums_screen = NULL;
lv_obj_t * albums_list = NULL;

static lv_obj_t * album_artist_screen = NULL;
lv_obj_t * album_artist_list = NULL;

static lv_obj_t * genres_screen = NULL;
lv_obj_t * genres_list = NULL;

lv_obj_t * group_songs_screen = NULL;
lv_obj_t * group_songs_list = NULL;
lv_obj_t * group_songs_search_list = NULL;
lv_obj_t * group_songs_title_label = NULL;
char * group_songs_source_title;

lv_obj_t * artist_albums_screen = NULL;
lv_obj_t * artist_albums_list = NULL;
lv_obj_t * artist_albums_title_label = NULL;

lv_obj_t * cue_tracks_screen = NULL;
lv_obj_t * cue_tracks_list = NULL;
lv_obj_t * cue_tracks_title_label = NULL;

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

    file_browser_set_sort_reverse(current_settings.file_sort_reverse);
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


/* Paged All Songs -- see build_all_songs_screen()'s own comment. offset is
 * a position in the DB's own title-sorted order (metadata_db_get_songs_
 * filtered_page() with every filter NULL), the exact same order playlist_
 * lazy_sort_order/on_file_selected_lazy_all_songs() already assume, so a
 * row tapped here and the playback queue it starts always agree on what
 * comes next. Heap-allocated, not a stack array -- see remote_control.c's
 * own build_library_json() comment on why a COMPACT_LIST_PAGE_CACHE_SIZE-
 * sized array of song_row_t (~1.25KB each) has no business on a stack. */
const char * song_quality_asset_for_path(const char * path) {
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

void gui_song_format_detail(char * out, size_t size, int seconds, const char * format) {
    const char * name = format && format[0] ? format : TR("Audio");
    if (seconds >= 0) snprintf(out, size, "%u:%02u · %s", (unsigned) seconds / 60, (unsigned) seconds % 60, name);
    else snprintf(out, size, "%s", name);
}

const char * gui_song_quality_asset(const char * suffix) {
    char path[32];
    snprintf(path, sizeof(path), ".%s", suffix ? suffix : "");
    return song_quality_asset_for_path(path);
}

const char * library_codec_name(audio_codec_t codec) {
    const char * name = audio_codec_name(codec);
    return name[0] ? name : TR("Audio");
}

void format_music_submenu_identity(const song_row_t * song, char * out, size_t out_size) {
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

bool album_songs_are_multi_disc(const song_row_t * songs, int count) {
    if (!songs || count <= 1) return false;
    int first = effective_disc_number(&songs[0]);
    for (int i = 1; i < count; i++) {
        if (effective_disc_number(&songs[i]) != first) return true;
    }
    return false;
}

int cmp_song_disc_track(const void * a, const void * b) {
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

void set_album_disc_headers(const song_row_t * songs, int count,
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

void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row) {
    gui_library_format_song_identity(row, out->label, sizeof(out->label),
                                     out->subtitle, sizeof(out->subtitle));
    out->identity = row->id;
    out->artwork_key = song_album_thumbnail_key(row);
    snprintf(out->trailing_asset, sizeof(out->trailing_asset), "%s", song_quality_asset_for_path(row->path));
}

void gui_library_jobs_ready_timers(void) {
    library_thumbnails_dispatch_ready();
    compact_list_dispatch_ready();
    /* Only a poller whose worker has finished: a notify from any other worker
     * does not need to wake them. */
    if (album_load_timer && !lv_timer_get_paused(album_load_timer) &&
        (album_load_job ? atomic_load_explicit(&album_load_job->done, memory_order_acquire)
                        : (album_load_pending != NULL && !library_async_kind_saturated(&album_orphans))))
        lv_timer_ready(album_load_timer);
    genre_load_ready_timer();
    artist_songs_ready_timer();
}

bool gui_library_background_busy(void) {
    /* A finished prefetch only waits for a tap; it is not work in progress. */
    bool album_reading = album_load_job && !(album_load_job->prefetch &&
        atomic_load_explicit(&album_load_job->done, memory_order_acquire));
    return album_reading || album_load_pending || genre_load_busy() || group_probe_job || gui_page_durations_busy() ||
           artist_songs_busy() || library_thumbnails_busy() || compact_list_any_fetch_pending() || gui_artist_data_busy() ||
           gui_album_header_busy();
}

/* A UI reload prepares and then tears down: the orphan wait is one budget across both, so a stalled
 * worker costs it once. Armed by the prepare step; honoured while the reload is still under way. */
static int64_t reload_orphan_deadline;
#define RELOAD_ORPHAN_WAIT_MS 1500
#define RELOAD_ORPHAN_VALID_MS 10000

int64_t orphan_wait_deadline(void) {
    if (reload_orphan_deadline && worker_deadline_in_ms(0) < reload_orphan_deadline + RELOAD_ORPHAN_VALID_MS)
        return reload_orphan_deadline;
    return worker_deadline_in_ms(RELOAD_ORPHAN_WAIT_MS);
}

/* library_thumbnails_quiesce() gives its prewarm read up instead of joining it: wait (bounded, the
 * shared orphan budget) for it like for the other abandoned reads. */
void quiesce_thumbnails(void) {
    library_thumbnails_quiesce();
    library_async_wait_orphans(orphan_wait_deadline());
}

typedef const char * (*az_index_name_of_t)(int display_index);

/* The A-Z widgets, visibility timer and raw pointer drag timer are owned by
 * library_az_index. This GUI supplies active-list policy and the shared raw
 * pointer-indev lookup. Keep this wrapper for the existing public callback. */
static bool az_index_strip_allowed(lv_obj_t * list) {
    return list != albums_list || (effective_album_sort() & ~METADATA_DB_ALBUM_SORT_REVERSE) == METADATA_DB_ALBUM_SORT_NAME;
}

static bool album_az_index_allowed(void) {
    return az_index_strip_allowed(albums_list);
}

static void album_az_index_table(int jump[27]) {
    metadata_db_get_album_az_table_sorted(effective_album_sort(), jump);
}

void poll_az_index_drag(lv_timer_t * timer) {
    library_az_index_poll(timer);
}

/* Row `index` of the whole Albums list in its sort order. */
static bool resolve_album_sorted_row(int index, group_row_t * out) {
    return index >= 0 && out && metadata_db_get_albums_page_sorted(NULL, effective_album_sort(), index, 1, out) == 1;
}

bool resolve_album_display_row(int index, group_row_t * out) {
    if (index < 0 || !out) return false;
    if (library_search_showing_results(SEARCH_BINDING_ALBUMS)) {
        int canonical;
        if (!library_search_try_map_filtered_row(SEARCH_BINDING_ALBUMS, index, &canonical)) return false;
        return metadata_db_get_albums_page_filtered(NULL, canonical, 1, out) == 1;
    }
    return resolve_album_sorted_row(index, out);
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
        file_browser_set_sort_reverse(current_settings.file_sort_reverse);
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
        char name[128];
        metadata_db_group_kind_t kind;
        artist_albums_snapshot(&kind, name, sizeof(name), NULL);
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
 * above the items (none in the lists today). */
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
    /* All Songs and Recently Added carry their Play / Shuffle bar as a list header, not as a row. */
    if (all_songs_list)
        compact_list_set_now_playing(all_songs_list, now_playing_list_row(SEARCH_BINDING_ALL_SONGS, all_songs_row, 0));
    if (recently_added_list)
        compact_list_set_now_playing(recently_added_list,
                                     now_playing_list_row(SEARCH_BINDING_RECENTLY_ADDED, recently_added_row, 0));

    refresh_group_songs_now_playing_indicator(); /* group_songs isn't compact_list-based -- see its own comment */
    char current_artist_albums_name[128];
    metadata_db_group_kind_t current_artist_albums_kind;
    int current_artist_albums_group_count;
    artist_albums_snapshot(&current_artist_albums_kind, current_artist_albums_name,
                           sizeof(current_artist_albums_name), &current_artist_albums_group_count);
    bool same_artist_album_view = result->request.artist_albums_kind == current_artist_albums_kind &&
        strcmp(result->request.artist_albums_name, current_artist_albums_name) == 0;
    if (artist_albums_list)
        compact_list_set_now_playing(artist_albums_list,
            same_artist_album_view && result->artist_albums_row > 0 &&
            result->artist_albums_row <= current_artist_albums_group_count
                ? result->artist_albums_row - 1 : -1);
}

void refresh_now_playing_indicators(void) {
    request_now_playing_indicator_lookup();
    /* These rows are already in memory, so keep their marker responsive while
     * the database-only lookup runs. */
    refresh_group_songs_now_playing_indicator();
    refresh_artist_albums_now_playing_indicator();
    gui_subsonic_refresh_now_playing();
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
void rearm_library_list(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, int total_count) {
    if (!list || library_search_is_active_on_list(list)) return;
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    compact_list_set_paged_provider(list, fetch_page, NULL, total_count);
    if (scroll_y > 0) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
        compact_list_refresh_visible(list);
    }
}

void refresh_library_lists_in_place(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    int song_count = (int) metadata_db_get_song_count();
    rearm_library_list(all_songs_list, all_songs_fetch_page, song_count);
    songs_play_bar_refresh(all_songs_list, &all_songs_bar, PLAY_ALL_SOURCE_ALL_SONGS);
    rearm_library_list(artists_list, artists_fetch_page, artist_count);
    rearm_library_list(albums_list, albums_fetch_page, album_count);
    rearm_library_list(album_artist_list, album_artists_fetch_page, album_artist_count);
    rearm_library_list(recently_added_list, recently_added_fetch_page, song_count);
    songs_play_bar_refresh(recently_added_list, &recent_bar, PLAY_ALL_SOURCE_RECENTLY_ADDED);
    rearm_library_list(genres_list, genres_fetch_page, count_genres_from_database());
    rearm_genre_browse_lists();
    refresh_targeted_nested_views();
}

void invalidate_library_views_after_publication(library_rescan_mode_t completed_mode,
                                                       bool identity_changed) {
    library_search_invalidate_catalog();
    library_search_invalidate_bindings();
    hide_collection_menu();
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

void refresh_library_screens_after_reload(bool clear_thumbnail_cache) {
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

    library_az_index_unregister(albums_screen);
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
    library_az_index_register_table(albums_screen, albums_list, album_az_index_table,
                                    album_az_index_allowed);
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

void update_music_database_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_library_rescan();
}

/* No whole-library load anywhere in this app anymore -- all paged/DB-
 * backed now, see build_artists_screen()'s own comment. Drilling into an
 * artist/album-artist's own albums (artist_row_click_cb()/album_artist_
 * row_click_cb()), an album's own songs (album_row_click_cb()), search,
 * and the A-Z index are all direct, targeted DB queries. */
/* A finger resting on the Artists / Album Artist row for a moment is most likely a tap: start
 * decoding the first screen's thumbnails then, so they are mostly in place when the list is built.
 * Delayed so the press highlight frame is drawn first; a drag or release cancels it. */
static void artists_tile_cb(lv_event_t * e);
static void album_artist_tile_cb(lv_event_t * e);
#define MUSIC_TILE_PREWARM_DELAY_MS 30
static lv_timer_t * music_tile_prewarm_timer;
static bool music_tile_prewarm_album_artists;

static int music_tile_prewarm_rows(void) {
    lv_display_t * disp = lv_display_get_default();
    int32_t height = disp ? lv_display_get_vertical_resolution(disp) : 0;
    return MUSIC_LIST_ROW_HEIGHT > 0 ? (int) (height / MUSIC_LIST_ROW_HEIGHT) + 1 : 0;
}

/* The first row the list will show when it opens: lists keep their scroll position, so a list left
 * scrolled reopens there and its visible rows are the ones to decode first. */
static int music_tile_prewarm_first_row(bool album_artists) {
    lv_obj_t * list = album_artists ? album_artist_list : artists_list;
    int32_t top0, top1, bottom, viewport;
    if (!list || !compact_list_row_span(list, 0, &top0, &bottom, &viewport) ||
        !compact_list_row_span(list, 1, &top1, &bottom, &viewport) || top1 <= top0) return 0;
    int32_t scroll = lv_obj_get_scroll_y(list);
    return scroll > top0 ? (int) ((scroll - top0) / (top1 - top0)) : 0;
}

static void music_tile_prewarm_cancel(void) {
    if (music_tile_prewarm_timer) lv_timer_delete(music_tile_prewarm_timer);
    music_tile_prewarm_timer = NULL;
}

static void music_tile_prewarm_timer_cb(lv_timer_t * timer) {
    music_tile_prewarm_timer = NULL;
    lv_timer_delete(timer);
    library_thumbnails_prewarm_artists(music_tile_prewarm_album_artists,
                                       music_tile_prewarm_first_row(music_tile_prewarm_album_artists),
                                       music_tile_prewarm_rows());
}

static void music_tile_prewarm_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        music_tile_prewarm_cancel();
        music_tile_prewarm_album_artists = lv_event_get_user_data(e) != NULL;
        music_tile_prewarm_timer = lv_timer_create(music_tile_prewarm_timer_cb, MUSIC_TILE_PREWARM_DELAY_MS, NULL);
    } else if (code == LV_EVENT_PRESS_LOST) { /* the finger left or scrolled the menu: no tap is coming */
        music_tile_prewarm_cancel();
        library_thumbnails_prewarm_cancel();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_DELETE) {
        music_tile_prewarm_cancel();
    }
}

/* Adds the press hook to the row whose click opens Artists / Album Artist (found by its click
 * callback, so the menu builders stay untouched). */
static void music_tile_prewarm_attach(lv_obj_t * obj) {
    uint32_t events = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < events; i++) {
        lv_event_cb_t cb = lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i));
        if (cb == artists_tile_cb || cb == album_artist_tile_cb) {
            lv_obj_add_event_cb(obj, music_tile_prewarm_event_cb, LV_EVENT_ALL,
                                cb == album_artist_tile_cb ? (void *) 1 : NULL);
            break;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) music_tile_prewarm_attach(lv_obj_get_child(obj, i));
}

static void artists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    music_tile_prewarm_cancel();
    library_thumbnails_prewarm_artists(false, music_tile_prewarm_first_row(false), music_tile_prewarm_rows()); /* a tap faster than the delay */
    library_thumbnails_prewarm_collect();
    nav_push(artists_screen);
}

void gui_library_open_albums(void) {
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

static void albums_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_library_open_albums();
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    music_tile_prewarm_cancel();
    library_thumbnails_prewarm_artists(true, music_tile_prewarm_first_row(true), music_tile_prewarm_rows());
    library_thumbnails_prewarm_collect();
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

static void cover_flow_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !cover_flow_open_albums) return;
    cover_flow_open_albums();
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

/* Plugin Music entry `index`, built the way plugin rows always were. */
static icon_grid_item_t music_plugin_item(int index) {
    const char * icon, * text_size;
    int32_t height, width;
    plugin_manager_get_music_list_item_options(index, &icon, &height, &width, &text_size);
    icon_grid_item_t item = { "submenu/albums.png", NULL,
        plugin_manager_get_music_list_item_label(index), music_plugin_item_cb,
        (void *) (intptr_t) index };
    if (height > 0) { item.has_row_height = true; item.row_height = height; }
    if (width > 0) { item.has_row_width = true; item.row_width = width; }
    item.text_size = text_size;
    item.plugin_icon = icon;
    return item;
}

_Static_assert(UI_MUSIC_COUNT + PLUGIN_MAX_MUSIC_LIST_ITEMS <= UI_ORDER_MAX,
               "Music order cannot hold every plugin entry");
static lv_obj_t * build_music_screen(void) {
    const icon_grid_item_t items[UI_MUSIC_COUNT] = {
        { "submenu/files.png", NULL, TR("Files"), music_files_tile_cb, NULL },
        { "submenu/artists.png", NULL, TR("Artists"), artists_tile_cb, NULL },
        { "submenu/albums.png", NULL, TR("Albums"), albums_tile_cb, NULL },
        { "submenu/album_artist.png", NULL, TR("Album Artist"), album_artist_tile_cb, NULL },
        { "submenu/genres.png", NULL, TR("Genres"), genres_tile_cb, NULL },
        { "submenu/all_songs.png", NULL, TR("All Songs"), all_songs_tile_cb, NULL },
        { "submenu/playlists.png", NULL, TR("Playlists"), playlists_tile_cb, NULL },
        /* The Cover Flow icon ships with the firmware image; an older image shows the Albums one. */
        { row_icon_or("submenu/cover_flow.png", "submenu/albums.png"), NULL, TR("Cover Flow"),
          cover_flow_tile_cb, NULL },
    };
    const ui_customization_t customization = ui_customization_effective();
    icon_grid_item_t ordered_items[UI_ORDER_MAX];
    int ordered_count = 0;
    int configured_music_count = customization.music.count;
    if (configured_music_count > UI_ORDER_MAX) configured_music_count = UI_ORDER_MAX;
    for (int i = 0; i < configured_music_count; ++i) {
        int id = customization.music.ids[i];
        if (id < UI_MUSIC_COUNT) ordered_items[ordered_count++] = items[id];
        else ordered_items[ordered_count++] = music_plugin_item(id - UI_MUSIC_COUNT);
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
    lv_obj_t * title = screen_header_title(scr);
    if (title && toolbar_count > 0) reserve_title_width_before(title, toolbar_buttons[0]);
    music_tile_prewarm_attach(scr);
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

bool library_screen_in_navigation(lv_obj_t * screen) {
    if (!screen || lv_screen_active() == screen) return screen != NULL;
    int depth = gui_navigation_get_depth();
    for (int i = 0; i < depth; i++)
        if (gui_navigation_get_screen_at(i) == screen) return true;
    return false;
}

void refresh_targeted_nested_views(void) {
    hide_collection_menu();
    if (library_screen_in_navigation(artist_albums_screen)) {
        char name[128];
        metadata_db_group_kind_t kind;
        artist_albums_snapshot(&kind, name, sizeof(name), NULL);
        if (name[0]) populate_artist_albums_view(name, kind);
    }
    if (library_screen_in_navigation(group_songs_screen)) refresh_group_songs_from_database();
}

/* Diagnostic logging helper for teardown and initialization steps. */
static void library_teardown_diag(const char * step);
static void library_teardown_diag_end(void);

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
    gui_navigation_set_background_busy_cb(gui_library_background_busy);
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
    compact_list_set_press_cb(artist_albums_list, artist_album_row_press_cb);
    lv_obj_add_event_cb(artist_albums_list, artist_albums_scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
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
    library_az_index_register_table(albums_screen, albums_list, album_az_index_table,
                                    album_az_index_allowed);
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
    library_teardown_diag_end();
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
 * with fsync per line to pinpoint failures during theme reload. The file is
 * opened on the first line of a pass and closed by library_teardown_diag_end()
 * after the pass's last line, so a pass does not reopen it per step. */
static int library_diag_fd = -1;

static void library_teardown_diag(const char * step) {
    if (!db_log_enabled()) return;
    if (library_diag_fd < 0) {
        (void) mkdir("/data/mnt/sd_0/.logs", 0755);
        library_diag_fd = open("/data/mnt/sd_0/.logs/reload_diag.log",
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (library_diag_fd < 0) return;
    }
    char line[96];
    int len = snprintf(line, sizeof(line), "[pid=%ld]   gui_library_teardown: %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(library_diag_fd, line, (size_t) len);
        fsync(library_diag_fd);
    }
}

static void library_teardown_diag_end(void) {
    if (library_diag_fd < 0) return;
    close(library_diag_fd);
    library_diag_fd = -1;
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
    reload_orphan_deadline = 0; /* the reload's budget ends with the teardown */
    cancel_group_song_probes();
    if (albums_screen) library_az_index_unregister(albums_screen);
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
    gui_artist_page_release(artist_albums_list);
    gui_artist_page_forget(artist_albums_list);
    gui_artist_data_close();
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
    all_songs_bar = (songs_bar_t) { 0 };
    recent_bar = (songs_bar_t) { 0 };
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
    library_teardown_diag_end();
}

void gui_library_resume_fast_timers(void) {
    library_az_index_resume_timer();
}

void gui_library_reset_drag_state(void) {
    /* Safe to clear even mid-press: this pauses the timer below, and it is
     * only ever resumed on a raw press-DOWN edge (gui_shell_gestures.c's indev
     * hooks), so the remainder of an in-flight press cannot be re-judged at
     * a drifted position. Leaving it set would instead disable the strip
     * for the whole of the NEXT press. */
    library_az_index_reset_drag();
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
           thumbnails.warmup_active || thumbnails.cover_active || gui_library_sd_format_is_active() || library_search_has_active_job() ||
           remote_collection_busy() || (group_probe_job && !track_probe_job_done(group_probe_job));
}

bool gui_library_navigation_blocked(void) {
    /* These operations use a modal progress/completion screen. Thumbnail
     * decoding, post-scan cache warming, and search are deliberately absent:
     * they are cancellable/optional workers and do not own the active screen. */
    return gui_folder_actions_busy() || library_rescan_active || library_rescan_success_pending || gui_library_sd_format_is_active();
}

void gui_library_prepare_for_ui_reload(bool preserve_artwork) {
    gui_library_cancel_remote_collection();
#ifdef UI_PERF_TRACE
    library_thumbnail_status_t before_status;
    library_thumbnails_get_status(&before_status);
    unsigned decoded_before = (unsigned)before_status.decoded_cache_entries;
#endif
    reload_orphan_deadline = worker_deadline_in_ms(RELOAD_ORPHAN_WAIT_MS);
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
    quiesce_thumbnails();
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
    int64_t shutdown_deadline = worker_deadline_in_ms(2000); /* one budget for all of them and the cache flush */
    gui_album_header_shutdown(shutdown_deadline);
    gui_artist_data_shutdown(shutdown_deadline);
    gui_artist_page_release(artist_albums_list);
    library_thumbnails_quiesce();
    library_async_wait_orphans(shutdown_deadline); /* a stalled prewarm read ends or is left to the exit */
    gui_library_scan_join_for_shutdown();
    gui_library_sd_cancel_format();
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
