#pragma once
/* Declarations shared between the gui_library*.c files; private to the library browser. */
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "gui_library.h"
#include "../playlists/gui_playlists.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../audio/probe/track_probe.h"
#include "../../../core/system/runtime/play_mode.h"
#include "../../application/gui.h"
#include "../library_thumbnails.h"

typedef enum {
    GROUP_SONGS_REFRESH_NONE,
    GROUP_SONGS_REFRESH_FAVORITES,
    GROUP_SONGS_REFRESH_MOST_PLAYED,
    GROUP_SONGS_REFRESH_M3U,
    GROUP_SONGS_REFRESH_ALBUM,
    GROUP_SONGS_REFRESH_ARTIST,
    GROUP_SONGS_REFRESH_GENRE
} group_songs_refresh_source_t;

typedef enum {
    LIBRARY_PROMPT_CARD_CURRENT,
    LIBRARY_PROMPT_CARD_CHANGED,
    LIBRARY_PROMPT_CARD_UNKNOWN,
} library_prompt_card_status_t;

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define MOST_PLAYED_LIMIT 20
#define RECENTLY_ADDED_LIMIT 50

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
    /* Started when a row was touched, before it was tapped: it reads the album
     * and shows nothing. A tap on the same album adopts it (prefetch cleared);
     * a scroll, a lost press or a TTL without a tap drops it. */
    bool prefetch;
    uint32_t prefetch_tick;
    /* An in-place refresh of the album page already shown (after a targeted metadata publish):
     * the song count is read on the worker, the result replaces the page's rows in place (an
     * empty album clears them), and a stale or failed result is dropped without a toast. */
    bool refresh;
    atomic_int fin; /* worker/owner handoff, see gui_library_async.h */
} album_load_request_t;

/* A deferred push (genre songs, artist songs) lands as the result of the tap that asked for it,
 * never on top of what the user did since: it waits while a finger is down or a gesture runs
 * (at most LANDING_WAIT_MAX_MS), and is dropped when the origin screen is no longer shown or a
 * menu, popup or busy screen is open. */
typedef enum { LANDING_WAIT, LANDING_PUSH, LANDING_DROP } landing_verdict_t;

/* Poll period of a background-read poller, and the slow one while a kind is saturated (the tap waits
 * for a stalled read to end; the worker's exit wakes the loop, so the wait costs nothing). */
#define POLL_FAST_MS 25

#define POLL_SATURATED_MS 250

/* The row a touch resolved at press time, kept for the tap that follows so the click does not
 * run the same single-row lookup again. compact_list reports pressed=false whenever the press
 * can no longer be that tap (scroll, lost press, long press, list rebound, another item), and
 * that drops it; a stale entry also lapses after PRESS_RESOLVED_TTL_MS. */
enum { PRESS_SRC_ALBUMS = 1, PRESS_SRC_ARTIST_ALBUMS, PRESS_SRC_GENRES, PRESS_SRC_ARTISTS, PRESS_SRC_ALBUM_ARTISTS };

/* What the Play and Shuffle buttons start: the open group (album, artist, playlist),
 * Recently Added, or All Songs. */
enum { PLAY_ALL_SOURCE_GROUP, PLAY_ALL_SOURCE_RECENTLY_ADDED, PLAY_ALL_SOURCE_ALL_SONGS };

/* Genre browser (issue #191), defined with the Genres screen. */
enum { GENRE_VIEW_SONGS, GENRE_VIEW_ARTISTS, GENRE_VIEW_ALBUM_ARTISTS, GENRE_VIEW_ALBUMS, GENRE_VIEW_COUNT };

/* The Play / Shuffle bar above the rows of All Songs and Recently Added: a header of the compact list (it
 * scrolls away with it and is hidden while search results are shown). */
typedef struct { lv_obj_t * obj; int64_t tracks; } songs_bar_t;

/* A pathological album can contain tens of thousands of tracks. Building one
 * LVGL object per track caused a large post-scan RSS spike and could make ADB
 * unresponsive. Keep the backing group intact for playback, but materialize
 * only one bounded page of rows at a time. */
#define GROUP_SONGS_PAGE_SIZE 200

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

/* Defined elsewhere in the application */
extern void mount_sd_card_if_needed(void);

/* gui_library.c */
extern lv_obj_t * album_artist_list;
bool album_songs_are_multi_disc(const song_row_t * songs, int count);
extern lv_obj_t * albums_list;
extern lv_obj_t * all_songs_list;
extern lv_obj_t * all_songs_screen;
extern lv_obj_t * artist_albums_list;
extern lv_obj_t * artist_albums_screen;
extern lv_obj_t * artist_albums_title_label;
extern lv_obj_t * artists_list;
int cmp_song_disc_track(const void * a, const void * b);
extern lv_obj_t * cue_tracks_list;
extern lv_obj_t * cue_tracks_screen;
extern lv_obj_t * cue_tracks_title_label;
metadata_db_album_sort_t effective_album_sort(void);
metadata_db_album_sort_t effective_group_album_sort(metadata_db_group_kind_t kind);
void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row);
void format_music_submenu_identity(const song_row_t * song, char * out, size_t out_size);
extern lv_obj_t * genres_list;
extern lv_obj_t * group_songs_list;
extern lv_obj_t * group_songs_screen;
extern lv_obj_t * group_songs_search_list;
extern char * group_songs_source_title;
extern lv_obj_t * group_songs_title_label;
void invalidate_library_views_after_publication(library_rescan_mode_t completed_mode,
                                                       bool identity_changed);
const char * library_codec_name(audio_codec_t codec);
bool library_screen_in_navigation(lv_obj_t * screen);
extern now_playing_lookup_result_t now_playing_lookup_applied_result;
extern bool now_playing_lookup_applied_valid;
extern pthread_mutex_t now_playing_lookup_mutex;
int64_t orphan_wait_deadline(void);
void quiesce_thumbnails(void);
void rearm_library_list(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, int total_count);
extern lv_obj_t * recently_added_list;
extern lv_obj_t * recently_added_screen;
void refresh_library_lists_in_place(void);
void refresh_library_screens_after_reload(bool clear_thumbnail_cache);
void refresh_targeted_nested_views(void);
void request_now_playing_indicator_lookup(void);
metadata_db_album_sort_t requested_group_album_sort(metadata_db_group_kind_t kind);
bool resolve_album_display_row(int index, group_row_t * out);
void set_album_disc_headers(const song_row_t * songs, int count,
                                   bool multi_disc, group_song_entry_t * entries);
const char * song_quality_asset_for_path(const char * path);

/* gui_library_collections.c */
lv_obj_t * build_cue_tracks_screen(void);
group_song_entry_t * build_group_song_entries_from_paths(char ** paths, int count);
void ensure_playlists_dir(void);
void library_playlist_open_row(gui_playlist_row_kind_t kind, const char * name,
                                      const char * path, char ** songs, int count);
bool remote_collection_busy(void);
void remote_collection_cancel_and_finish(void);

/* gui_library_sd.c */
void build_sd_format_confirm_popup(void);
void build_sd_mount_failed_popup(void);
bool file_folder_job_storage_invalidated(void);
void gui_library_sd_cancel_format(void);
bool gui_library_sd_format_is_active(void);
library_prompt_card_status_t library_prompt_card_status(void);
void library_prompt_prepare_cache_load_card(void);
extern gui_popup_t sd_format_confirm_popup;
extern gui_popup_t sd_mount_failed_popup;

/* gui_library_artist.c */
void album_artist_collection_long_press_cb(int index);
void album_artist_collection_more_click_cb(int index);
extern lv_obj_t * album_collection_menu_backdrop;
extern lv_obj_t * album_collection_menu_popup;
void album_more_click_cb(int index);
void album_row_long_press_cb(int index);
bool artist_album_rederive_row(int index, compact_list_page_row_t * row, void * ctx);
void artist_album_row_click_cb(int index);
void artist_album_row_press_cb(int index, bool pressed);
void artist_albums_scroll_begin_cb(lv_event_t * e);
bool artist_albums_show_all_songs_internal(bool push_screen, const char * artist_name,
                                                    metadata_db_group_kind_t artist_kind);
void artist_albums_snapshot(metadata_db_group_kind_t * kind, char * name, size_t name_size, int * group_count);
void artist_collection_long_press_cb(int index);
void artist_collection_more_click_cb(int index);
extern int artist_open_song_hint;
bool artist_songs_busy(void);
void artist_songs_ready_timer(void);
void build_collection_menus(void);
void cancel_artist_songs_load(void);
extern lv_obj_t * collection_menu_backdrop;
extern lv_obj_t * collection_menu_popup;
void hide_collection_menu(void);
bool library_collection_menu_visible(void);
bool play_current_group(play_mode_t mode);
bool play_group_entries(const group_song_entry_t * entries, int count, const char * title,
                               bool is_album, play_mode_t mode);
void populate_artist_albums_view(const char * name, metadata_db_group_kind_t kind);

/* gui_library_browse.c */
void fill_album_cover_key(compact_list_page_row_t * out, const group_row_t * row);
int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
void album_load_bump_generation(void);
extern uint64_t album_load_generation;
extern album_load_request_t * album_load_job;
extern album_load_request_t * album_load_pending;
extern lv_timer_t * album_load_timer;
extern atomic_int album_orphans;
void album_prefetch_cancel(void);
void album_prefetch_start(const group_row_t * group, const char * artist_filter);
extern songs_bar_t all_songs_bar;
int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
void all_songs_tile_cb(lv_event_t * e);
int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
lv_obj_t * build_album_artist_screen(void);
lv_obj_t * build_albums_screen(void);
lv_obj_t * build_all_songs_screen(void);
lv_obj_t * build_artists_screen(void);
lv_obj_t * build_genre_browse_screen(lv_obj_t ** out_list, lv_obj_t ** out_title,
                                            compact_list_click_cb_t on_click);
lv_obj_t * build_genres_screen(void);
lv_obj_t * build_recently_added_screen(void);
void cancel_album_load(void);
int count_genres_from_database(void);
void format_song_identity(const song_row_t * row, char * out, size_t size);
extern lv_obj_t * genre_albums_list;
void genre_albums_row_click_cb(int index);
extern lv_obj_t * genre_albums_screen;
extern lv_obj_t * genre_albums_title_label;
int genre_browse_view(void);
extern lv_obj_t * genre_groups_list;
void genre_groups_row_click_cb(int index);
extern lv_obj_t * genre_groups_screen;
extern lv_obj_t * genre_groups_title_label;
bool genre_load_busy(void);
void genre_load_ready_timer(void);
extern lv_obj_t * genre_view_options[GENRE_VIEW_COUNT];
extern lv_obj_t * genre_view_screen;
int genres_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
landing_verdict_t landing_verdict(lv_obj_t * origin, bool * waiting, uint32_t * since);
group_song_entry_t * load_album_entries_filtered(const char * name, const char * album_artist,
                                                         int song_count, const char * artist_filter,
                                                         int * out_count, const atomic_bool * cancel);
void open_recently_added_screen(void);
void press_resolved_clear(void);
void press_resolved_set(int source, int index, const group_row_t * group);
bool press_resolved_take(int source, int index, group_row_t * out);
void rearm_genre_browse_lists(void);
extern songs_bar_t recent_bar;
int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
bool show_album_group_ex(const group_row_t * group, const char * artist_filter, bool * built_now);
bool album_group_resolve(const char * name, const char * album_artist, group_row_t * out);
int album_group_song_count(const char * name, const char * album_artist);
bool album_refresh_in_place_start(const char * name, const char * album_artist, const char * artist_filter);
bool show_album_group_filtered(const group_row_t * group, const char * artist_filter);
bool show_genre_songs_internal(bool push_screen, const char * genre_name);
void songs_play_bar_refresh(lv_obj_t * list, songs_bar_t * bar, int source);

/* gui_library_group_songs.c */
lv_obj_t * build_group_songs_screen(void);
void cancel_group_song_probes(void);
play_mode_t collection_play_mode(play_mode_t requested_mode);
bool detach_group_song_thumbnail(const lv_image_dsc_t *dsc);
void forget_deleted_library_lists(void);
extern uint64_t group_probe_generation;
extern track_probe_job_t * group_probe_job;
extern uint64_t group_probe_job_generation;
extern bool group_probe_storage_available;
extern int group_songs_count;
extern lv_obj_t * group_songs_edit_btn;
extern group_song_entry_t * group_songs_entries;
extern bool group_songs_loading;
extern lv_obj_t * group_songs_now_playing_bar;
extern metadata_db_group_kind_t group_songs_refresh_artist_kind;
extern char group_songs_refresh_artist_name[128];
extern char group_songs_refresh_genre[METADATA_DB_TEXT_MAX];
extern char group_songs_refresh_m3u_path[PATH_MAX];
extern group_songs_refresh_source_t group_songs_refresh_source;
const char * group_songs_search_name_of(int index);
void group_songs_search_row_click_cb(int display_index);
void group_songs_search_row_long_press_cb(int display_index);
extern bool group_songs_source_is_album;
extern lv_obj_t * group_songs_thumbnail_images[GROUP_SONGS_PAGE_SIZE];
extern lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
bool landing_pointer_busy(void);
void library_reload_edited_playlist_if_changed(lv_obj_t * screen);
library_thumbnail_context_t library_thumbnail_genre_context(void);
bool library_thumbnail_policy(library_thumbnail_policy_t policy);
void library_thumbnail_show_busy(library_thumbnail_cover_scope_t scope);
void playlist_start_selected(bool shuffle);
extern int playlist_start_source;
void populate_group_songs_rows(void);
void refresh_group_songs_from_database(void);
void group_songs_replace_refreshed_entries(group_song_entry_t * entries, int count);
void refresh_group_songs_now_playing_indicator(void);
void refresh_group_thumbnail_binding(void);
void set_group_songs_entries_owned(group_song_entry_t * entries, int count);
void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path, bool music_submenu);
void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count,
                                            bool push_screen);
void show_music_group_songs_impl(const char * name, const char * album_artist, const char * artist_filter,
                                        const group_song_entry_t * entries, int count, bool take_ownership);
void update_group_songs_search_button(bool editing);

/* gui_library_scan.c */
extern bool boot_warmup_pending;
extern bool boot_warmup_requested;
void build_library_update_prompt_popup(void);
void gui_library_scan_join_for_shutdown(void);
void library_prompt_card_changed(void);
void library_prompt_requeue_visible(void);
extern _Atomic bool library_rescan_active; /* read by the thumbnail warmer thread and the remote settings */
void library_rescan_free_target_paths(void);
extern library_rescan_mode_t library_rescan_mode;
extern bool library_rescan_success_pending;
extern gui_busy_handle_t library_rescan_token;
extern void (*library_setup_scan_complete)(bool success);
extern lv_obj_t * library_update_prompt_body;
extern lv_obj_t * library_update_prompt_confirm_label;
extern gui_popup_t library_update_prompt_popup;
extern lv_obj_t * library_update_prompt_title;
extern gui_popup_t refresh_all_covers_popup;
extern gui_popup_t refresh_all_metadata_popup;
void reload_library_on_sd_reinsert(bool announce_loaded);
void start_library_cover_reload_album(const char * album, const char * album_artist);
