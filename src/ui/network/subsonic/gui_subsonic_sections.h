#pragma once
/* The Subsonic menu's Albums, Genres, Random Songs and Favorites entries.
 * gui_subsonic.c lends the browse requests and list views these call. */
#include <lvgl/lvgl.h>
#include "../../../network/services/subsonic_client.h"

/* Menu row clicks. */
void gui_subsonic_albums_row_cb(lv_event_t * e);
void gui_subsonic_genres_row_cb(lv_event_t * e);
void gui_subsonic_random_row_cb(lv_event_t * e);
void gui_subsonic_favorites_row_cb(lv_event_t * e);

void gui_subsonic_sections_init(void);
void gui_subsonic_sections_teardown(void);
/* Takes the starred items and shows the Artists / Albums / Songs chooser. */
void gui_subsonic_favorites_show(subsonic_starred_t * starred);
/* Drops the held starred items (new connection, teardown). */
void gui_subsonic_favorites_clear(void);
void gui_subsonic_starred_free(subsonic_starred_t * starred);

/* Provided by gui_subsonic.c. */
void gui_subsonic_open_all_albums(void);
void gui_subsonic_browse_album_list(const char * type, const char * title);
void gui_subsonic_browse_genres(void);
void gui_subsonic_browse_random_songs(void);
void gui_subsonic_browse_starred(void);
/* Borrowed views: the arrays stay owned by the caller. */
void gui_subsonic_show_artists_view(const subsonic_artist_t * artists, int count);
void gui_subsonic_show_albums_view(const subsonic_album_t * albums, int count, const char * title);
/* Copies the songs. */
void gui_subsonic_show_songs_copy(const subsonic_song_t * songs, int count, const char * title);
/* Makes any view that shows these arrays let go of them before they are freed. */
void gui_subsonic_release_views(const subsonic_artist_t * artists, const subsonic_album_t * albums);
