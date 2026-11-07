#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "gui_subsonic.h"
#include "../../shell/gui_notifications.h"
#include "gui_subsonic_album.h"
#include "../../../library/remote/subsonic/subsonic_refresh.h"

/* Shared foreground request deadline (connect and browse). */
#define SUBSONIC_REQUEST_TIMEOUT_MS 30000U

/* gui_subsonic.c */
bool gui_subsonic_wifi_connected_guard(void);
bool subsonic_foreground_busy(void);
void subsonic_dismiss_busy(int depth_before, gui_busy_handle_t token);
void subsonic_active_server_key(char out[SUBSONIC_CACHE_KEY_SIZE]);
void subsonic_copy_request_error(char * dest, size_t size, const char * fallback);

/* gui_subsonic_settings.c */
void gui_subsonic_settings_build(void);
void gui_subsonic_settings_delete(void);
void gui_subsonic_settings_open_cb(lv_event_t * e);

/* gui_subsonic_download.c */
void gui_subsonic_downloads_cancel(void);
void gui_subsonic_downloads_join(void);
void gui_subsonic_download_track(const char * url, bool verify_tls, const char * dest_path, const char * display_title);
void gui_subsonic_download_library(subsonic_song_t * songs, int song_count, char ** album_ids,
                                   int album_to_expand_count, const char * playlist_name, const char * progress_label);
char ** gui_subsonic_copy_album_ids(const subsonic_album_t * albums, int count);
bool gui_subsonic_downloads_active(void);
bool gui_subsonic_append_songs(subsonic_song_t ** songs, int * count, int * capacity,
                               const subsonic_song_t * album_songs, int album_song_count);

/* gui_subsonic_connect.c */
extern bool subsonic_connect_active;
void gui_subsonic_connect_build(void);
void gui_subsonic_connect_delete(void);
void gui_subsonic_connect_cancel(void);
void gui_subsonic_connect_join(void);

/* gui_subsonic_actions.c */
void gui_subsonic_download_confirm_build(void);
void gui_subsonic_download_confirm_teardown(void);
void gui_subsonic_download_songs_btn_cb(lv_event_t * e);
void gui_subsonic_download_artist_btn_cb(lv_event_t * e);
void gui_subsonic_play_from(int index);

/* gui_subsonic_browse.c */
typedef enum {
    SUBSONIC_BROWSE_ARTISTS,
    SUBSONIC_BROWSE_ALBUM_SONGS,
    SUBSONIC_BROWSE_ARTIST_ALBUMS,
    SUBSONIC_BROWSE_PLAYLIST_SONGS,
    SUBSONIC_BROWSE_PLAYLISTS,
    SUBSONIC_BROWSE_ALL_ALBUMS,
    /* id is the getAlbumList2 type; a genre's name is the title */
    SUBSONIC_BROWSE_ALBUM_LIST,
    SUBSONIC_BROWSE_GENRES,
    SUBSONIC_BROWSE_RANDOM_SONGS,
    SUBSONIC_BROWSE_STARRED,
    SUBSONIC_BROWSE_ARTIST_SONGS,
} subsonic_browse_kind_t;

void gui_subsonic_browse_cancel(void);
void gui_subsonic_browse_join(void);
extern bool subsonic_browse_active;
void start_subsonic_browse(subsonic_browse_kind_t kind, const char * id, const char * title);
bool subsonic_install_transient_songs(subsonic_song_t * items, int count, const char * title);
bool subsonic_open_transient_songs(subsonic_song_t * items, int count, const char * title);

/* gui_subsonic_sync.c */
void subsonic_clear_browse_caches(void);
void subsonic_ping_or_queue(void);
void gui_subsonic_sync_reset(void);
void gui_subsonic_sync_stop_network(void);
void gui_subsonic_sync_shutdown(void);
bool gui_subsonic_sync_busy(void);
extern subsonic_cache_entry_t subsonic_entries[SUBSONIC_LIST_ALL_KINDS];
extern subsonic_next_job_t subsonic_next;
extern subsonic_list_kind_t subsonic_songs_kind;
extern char subsonic_songs_id[64];
extern bool subsonic_songs_prune_due;
extern bool subsonic_cover_flow_pending;
void subsonic_sync_songs_context(subsonic_list_kind_t kind, const char * id);
void subsonic_songs_stamp(subsonic_list_kind_t kind, const char * id, subsonic_library_stamp_t * stamp);
void subsonic_refresh_parent_list(bool album);
void subsonic_menu_artists_row_cb(lv_event_t * e);
void subsonic_menu_playlists_row_cb(lv_event_t * e);
void subsonic_menu_cover_flow_row_cb(lv_event_t * e);
void subsonic_cover_flow_open_album(const char * id, const char * title);
void subsonic_refresh_button_cb(lv_event_t * e);
void subsonic_open_songs(subsonic_list_kind_t kind, const char * id, const char * title);

/* gui_subsonic_views.c */
typedef enum { SUBSONIC_ALBUMS_ALL, SUBSONIC_ALBUMS_ARTIST, SUBSONIC_ALBUMS_LIST } subsonic_albums_mode_t;

void subsonic_show_list(subsonic_list_kind_t kind);
void subsonic_restore_artists_view(void);
extern lv_obj_t * subsonic_artists_screen;
extern lv_obj_t * subsonic_albums_screen;
extern lv_obj_t * subsonic_songs_screen;
extern lv_obj_t * subsonic_playlists_screen;
extern subsonic_song_t * subsonic_songs_cache;
extern int subsonic_songs_count;
extern const subsonic_album_t * subsonic_albums_cache;
extern int subsonic_albums_count;
extern char subsonic_albums_context_artist[128];
extern bool subsonic_songs_context_is_playlist;
extern char subsonic_songs_context_playlist_name[128];
extern lv_obj_t * subsonic_songs_title_label;
extern lv_obj_t * subsonic_refresh_buttons[SUBSONIC_LIST_KINDS];
extern subsonic_playlist_t * subsonic_playlists_cache;
extern int subsonic_playlists_count;
bool subsonic_show_albums(const subsonic_album_t * albums, int count, const char * title, subsonic_albums_mode_t mode, const char * artist, const char * artist_id);
void subsonic_keep_albums(bool all, subsonic_album_t * fresh, int count);
bool subsonic_install_all_albums(subsonic_album_t * items, int count, bool keep_scroll);
void subsonic_set_songs_context(subsonic_list_kind_t kind, const char * id, const char * title);
bool subsonic_install_songs(subsonic_song_t * items, int count, bool keep_scroll);
void subsonic_clear_songs(void);
bool subsonic_install_artists(subsonic_artist_t * items, int count, bool keep_scroll);
bool subsonic_install_playlists(subsonic_playlist_t * items, int count, bool keep_scroll);
void subsonic_views_clear(void);
void subsonic_reset_browse_data(void);
void gui_subsonic_views_build(void);
void gui_subsonic_views_build_headers(void);
void subsonic_delete_browse_screens(void);
bool subsonic_list_loaded(subsonic_list_kind_t kind);
void subsonic_open_cover_flow(void);
void subsonic_songs_album_info(const subsonic_server_t * server, gui_subsonic_album_info_t * info);
