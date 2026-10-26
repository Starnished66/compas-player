#pragma once

#include <stdbool.h>
#include <lvgl/lvgl.h>

typedef enum {
    GUI_PLAYLIST_ROW_FAVORITES = 0,
    GUI_PLAYLIST_ROW_MOST_PLAYED = 1,
    GUI_PLAYLIST_ROW_QUEUE = 2,
    GUI_PLAYLIST_ROW_RECENTLY_ADDED = 3,
    GUI_PLAYLIST_ROW_SAVED_M3U = -1
} gui_playlist_row_kind_t;

/* For SAVED_M3U, name/path/songs are borrowed only for the synchronous call;
 * the playlist owner frees songs immediately after the callback returns. */
typedef void (*gui_playlists_open_row_cb_t)(gui_playlist_row_kind_t kind,
                                           const char *name, const char *path,
                                           char **songs, int count);
typedef void (*gui_playlists_reload_edited_cb_t)(lv_obj_t *screen);
typedef void (*gui_playlists_ensure_dir_cb_t)(void);

void gui_playlists_init(gui_playlists_open_row_cb_t open_row,
                        gui_playlists_reload_edited_cb_t reload_edited,
                        gui_playlists_ensure_dir_cb_t ensure_dir);
lv_obj_t *gui_playlists_build_saved_screen(void);
lv_obj_t *gui_playlists_build_add_screen(void);
void gui_playlists_build_context_menu(void);
void gui_playlists_open_saved_screen(void);
void gui_playlists_open_for_song(const char *path);
void gui_playlists_open_for_folder(const char *path);
void gui_playlists_reset_name_target_mode(void);
lv_obj_t *gui_playlists_get_saved_screen(void);
void gui_playlists_poll(void);
void gui_playlists_teardown_popups(void);
void gui_playlists_teardown_saved_screen(void);
void gui_playlists_teardown_add_screen(void);
bool gui_playlists_menu_visible(void);
