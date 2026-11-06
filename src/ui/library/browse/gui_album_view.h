#pragma once
/* The widgets of an album page, with no data source of their own: big cover,
 * title, artist, "year · genre", format / Hi-Res / total-time chips, the
 * Play and Shuffle buttons, and numbered track rows. The local album header
 * (gui_album_header.c) and the Subsonic album page both build on it. UI thread only. */
#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>

typedef struct {
    lv_obj_t * root, * info, * chips, * cover_img;
    int32_t cover_px, info_w;
    int32_t num_w;       /* track number column: fits the album's widest number */
    char hires_icon[96]; /* quality badge asset shown on the Hi-Res chip */
} gui_album_view_t;

typedef struct {
    const char * album, * artist, * meta; /* artist and meta may be empty */
    const char * hires_icon;
    bool show_buttons;
    lv_event_cb_t play_cb, shuffle_cb;
} gui_album_view_params_t;

/* Builds the page header as the last child of `list`. The cover stays hidden until shown. */
void gui_album_view_build(gui_album_view_t * v, lv_obj_t * list, const gui_album_view_params_t * p);
/* Replaces the chips; empty text and !hires leave that chip out. */
void gui_album_view_set_chips(gui_album_view_t * v, const char * format, bool hires, const char * total);
/* "H hr M min" or "M min" for a total of `seconds`; empty when there is no time. */
void gui_album_view_total_text(double seconds, char * out, size_t size);
/* "year · genre" (either may be missing). */
void gui_album_view_meta_text(int year, const char * genre, char * out, size_t size);

/* Draws `dsc` as the cover (centred when it is exactly the cover's size). */
void gui_album_view_show_cover(gui_album_view_t * v, const lv_image_dsc_t * dsc);
/* Sizes the cover image to the cover box, touching LVGL only when something changes. */
void gui_album_view_fit_cover(gui_album_view_t * v, lv_image_align_t align);
/* Clears the cover image if it shows `dsc` (or any picture when dsc is NULL); true when it did. */
bool gui_album_view_detach_cover(gui_album_view_t * v, const lv_image_dsc_t * dsc);

/* Width of the number column for an album whose widest track number is `widest`. */
int32_t gui_album_view_number_width(int widest);
/* Restyles a build_music_list_row() into a numbered album row. seconds < 0 leaves the time blank. */
void gui_album_view_decorate_row(lv_obj_t * row, int number, int num_w, int seconds);
void gui_album_view_row_set_duration(lv_obj_t * row, unsigned seconds);
/* The quality badge at the right end of a track row. */
void gui_album_view_row_badge(lv_obj_t * row, const char * asset);
