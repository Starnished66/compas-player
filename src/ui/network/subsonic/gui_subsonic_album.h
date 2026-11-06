#pragma once
#include <stdbool.h>
#include <lvgl/lvgl.h>
#include "../../../network/services/subsonic_client.h"
#include "../../../library/remote/subsonic/subsonic_cache.h"

/* The Subsonic album page: the local album page's look (big cover, title,
 * artist, year and genre, chips, Play and Shuffle, numbered rows) built from
 * the album's song list. It lives as a second list on the Songs screen, shown
 * instead of the compact list for an album of up to GUI_SUBSONIC_ALBUM_MAX_TRACKS
 * tracks. UI thread only; the cover is read and decoded on a worker. */

#define GUI_SUBSONIC_ALBUM_MAX_TRACKS 200 /* the local album page's page size */

typedef struct {
    const subsonic_server_t * server;
    const char * title, * artist, * genre, * cover_id;
    int year;
} gui_subsonic_album_info_t;

/* The page's scrolling list, hidden, as a child of the Songs screen. */
lv_obj_t * gui_subsonic_album_list_build(lv_obj_t * screen);

/* True when an album of `count` tracks gets this page rather than the compact list. */
bool gui_subsonic_album_fits(int count);

/* Builds the page from `songs` (1..MAX tracks) and shows its list. keep_scroll
 * puts a refreshed page back where the reader was. */
void gui_subsonic_album_show(const gui_subsonic_album_info_t * info, const subsonic_song_t * songs, int count,
                             bool keep_scroll);

/* Empties and hides the page (the compact list takes over, or the screen is reused). */
void gui_subsonic_album_clear(void);

/* The count line with Play and Shuffle above the rows of the compact songs list
 * (playlists, albums too long for the page); count 0 removes it. */
void gui_subsonic_songs_bar_set(lv_obj_t * compact, int count);

/* Songs screen LOADED / UNLOADED handler: asks for the cover again, or frees its buffer. */
void gui_subsonic_album_screen_event_cb(lv_event_t * e);

/* Forgets the list before its screen is deleted. */
void gui_subsonic_album_teardown(void);

/* Main loop: shows the cover once the worker has it. */
void gui_subsonic_album_poll(void);
