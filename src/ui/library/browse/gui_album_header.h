#pragma once
/* Album page header: cover, title, artist, year and genre, format, Hi-Res and
 * total-time chips, and the Play / Shuffle buttons. Used only by the album
 * view of the group-songs screen (gui_library.c); playlists, genres, artists'
 * "all songs" and favourites never call it.
 *
 * The header is a child of the group-songs list, so it scrolls with the rows.
 * Everything that needs file I/O (per-track duration and codec) runs on the
 * track-probe worker; this module only reads results on the UI thread. */
#include <stdbool.h>
#include <lvgl/lvgl.h>
#include "gui_library.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../audio/engine/audio.h"

typedef struct {
    const char * album;
    const char * album_artist;
    const group_song_entry_t * entries;
    int count;
    bool show_buttons;                         /* false while loading or empty */
    lv_event_cb_t play_cb, shuffle_cb;
    const char * (*quality_asset)(const char * path); /* the rows' badge icon */
} gui_album_header_params_t;

/* Worker thread (or any thread): remembers year, genre and track numbers of an
 * album's songs, which must be in the same order as the group entries. */
void gui_album_header_record(const song_row_t * songs, int count);

/* UI thread. Builds the header as the first child of `list`. */
void gui_album_header_build(lv_obj_t * list, const gui_album_header_params_t * p);
/* Drops every pointer into the list; call before the list is cleaned. */
void gui_album_header_forget(void);

/* Restyles a row built by build_music_list_row() into a numbered album row:
 * track number on the left, duration before the Hi-Res badge. */
void gui_album_header_decorate_row(lv_obj_t * row, int index);
/* `title` with any "\n detail" tail removed, copied into buf. */
const char * gui_album_header_row_title(const char * title, char * buf, size_t size);
void gui_album_header_row_set_duration(lv_obj_t * row, unsigned seconds);

/* Probe results of the page the group screen already probes. */
void gui_album_header_note_probe(int index, const audio_current_format_info_t * info);
/* Called on every probe tick. `visible` lets the cover decode start. `allowed` is true while the album page is the
 * visible screen and its own page probe has finished. */
void gui_album_header_poll(bool visible, bool allowed);
/* Frees the sharp cover buffer; call once the album page is left or cleaned. */
void gui_album_header_release_cover(void);
/* Blocking final-shutdown cleanup of the whole-album probe. */
void gui_album_header_shutdown(void);

/* Cover decoration, driven by the shared thumbnail pipeline. */
void gui_album_header_refresh_cover(lv_obj_t * list);
bool gui_album_header_detach_cover(const lv_image_dsc_t * dsc);
