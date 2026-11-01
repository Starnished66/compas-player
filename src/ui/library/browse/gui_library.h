#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/remote/remote_collection.h"
#include "../../../library/artwork/support/library_player_art.h"
#include "../../settings/power/gui_power_hub.h"
#include "../../../library/metadata/scan/library_scan_storage.h"

typedef struct group_song_entry_s {
    char * path;
    char * title;
    int32_t disc_number;
    bool show_disc_header;
    int64_t song_id;
    uint64_t artwork_key;
} group_song_entry_t;

void free_group_song_entries(group_song_entry_t * entries, int count);
bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count);
void show_group_songs(const char * title, const group_song_entry_t * entries, int count);

/* Screens accessors */
lv_obj_t * gui_library_get_music_screen(void);
lv_obj_t * gui_library_get_files_screen(void);
lv_obj_t * gui_library_get_all_songs_screen(void);
lv_obj_t * gui_library_get_recently_added_screen(void);
lv_obj_t * gui_library_get_artists_screen(void);
lv_obj_t * gui_library_get_albums_screen(void);
lv_obj_t * gui_library_get_album_artist_screen(void);
lv_obj_t * gui_library_get_genres_screen(void);
lv_obj_t * gui_library_get_group_songs_screen(void);
lv_obj_t * gui_library_get_playlists_screen(void);

void gui_library_init(void);
void gui_library_start_boot_thumbnail_warmup(void);
void gui_library_boot_ready(uint32_t tick);
void gui_library_poll_boot_prompt(void);
void gui_library_request_files_changed_prompt(void);
void gui_library_invalidate_boot_prompt(void);
void gui_library_suspend_boot_prompt(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_library_init() again from a clean slate. */
void gui_library_teardown(void);
void gui_library_refresh_music_screen(void);
void gui_library_accent_changed(void);
void gui_library_set_artist_images_enabled(bool enabled);
typedef enum {
    GUI_LIBRARY_SORT_FILES,
    GUI_LIBRARY_SORT_ALBUMS,
    GUI_LIBRARY_SORT_ARTIST_ALBUMS,
    GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS
} gui_library_sort_scope_t;
/* Apply one persisted ordering without disturbing unrelated library views. */
void gui_library_apply_sorting(gui_library_sort_scope_t scope);
/* The album order an artist (or album artist) page shows, as the database can serve it. UI thread only. */
metadata_db_album_sort_t gui_library_group_album_sort(metadata_db_group_kind_t kind);

void start_library_rescan(void);
void start_library_auto_rescan(void);
/* Explicit setup scan; reports completion on the UI thread and returns to setup. */
bool gui_library_start_setup_scan(void (*on_complete)(bool success));
void start_library_metadata_refresh_paths(const char * const * paths, int count);
void start_library_metadata_refresh_album(const char * album, const char * album_artist);
void start_library_metadata_refresh_artist(metadata_db_group_kind_t kind, const char * name);
void show_library_refresh_all_metadata_prompt(void);
/* Controls automatic library rescanning on boot / SD mount. */
bool gui_library_auto_rescan_enabled(void);
void poll_library_rescan(void);
void poll_sd_format(void);
void album_thumbnail_generation_poll(void);

void open_add_to_playlist_for(const char * path);
void gui_library_poll_playlists(void);
/* Nonblocking duration/codec completion polling; updates only the current page. */
void gui_library_poll_track_probes(void);
/* Main loop, after a worker published (ui_wake_notify): make the poll timers that only
 * check "is the worker done" run in this pass (thumbnail hand-off, paged list landing,
 * album load). Their own guards (transition, scrolling) still apply. */
void gui_library_jobs_ready_timers(void);
/* True while any library worker is running or queued: page loads, lookups, probes, covers. */
bool gui_library_background_busy(void);
void on_cue_file_selected(const char * cue_path);
void show_artist_albums(const char * name, metadata_db_group_kind_t kind);
/* Opens the local artist or album group containing `path`, if the path is
 * present in the indexed library. Returns false for streams and unindexed
 * tracks. Album identity is the exact (album, album_artist) pair. */
bool gui_library_open_now_playing_group(const char * path, bool album);
void refresh_library_screens_after_rescan(void);

void poll_az_index_drag(lv_timer_t * timer);
void plugin_stream_tile_click_cb(lv_event_t * e);
/* The screen came back on: a half-built Songs page goes back to its normal build pace at once. */
void gui_library_screen_woke(void);
void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index);

bool search_close_if_active_for_screen(lv_obj_t * screen);
void refresh_now_playing_indicators(void);
/* UI-thread poll for the coalesced worker result requested by
 * refresh_now_playing_indicators(). */
void gui_library_poll_now_playing_indicators(void);

/* Shared song identity used by library and queue rows: display title on the
 * first line, then Artist · Album on the metadata line. */
void gui_library_format_song_identity(const song_row_t * row,
                                      char * title, size_t title_size,
                                      char * subtitle, size_t subtitle_size);

void poll_search_job(void);
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                              const char * album_artist_filter, const char * album_filter);
/* Starts background preparation for a catalog-wide collection play request.
 * Completion is published through remote_control_finish_collection(). */
bool gui_library_play_remote_collection(const remote_collection_play_request_t *request);
void gui_library_cancel_remote_collection(void);
void poll_sd_card_hotplug(void);

void gui_library_resume_fast_timers(void);
void gui_library_reset_drag_state(void);

bool gui_library_has_background_work(void);
/* True only while a modal library operation owns navigation. Optional
 * workers such as album-art warming remain background work for shutdown
 * coordination, but must not disable the drawer or screen gestures. */
bool gui_library_navigation_blocked(void);
/* Joins UI-pointer-bearing artwork/search jobs before a soft screen rebuild.
 * Layout-only reloads may retain decoded artwork; full reloads invalidate it. */
void gui_library_prepare_for_ui_reload(bool preserve_artwork);
void gui_library_cancel_background_work(void);
/* Frees the decoded thumbnail cache before an external session; rows refill it on demand. */
void gui_library_release_memory_for_external_session(void);
void gui_library_cancel_scan(void);
