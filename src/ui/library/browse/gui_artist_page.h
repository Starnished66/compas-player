#pragma once
/* The artist page header: a mosaic of the first albums' covers, name, counts,
 * genre, year-span and total-time chips, Play / Shuffle, the "Songs" row and
 * the "Albums" label. It is a child of the artist's album list (a compact
 * list) and scrolls with it; the album rows start below it.
 *
 * Facts come from gui_artist_data.c (one background pass per open); this
 * module only draws them on the UI thread. */
#include <stdbool.h>
#include <lvgl/lvgl.h>
#include "../../../library/metadata/db/metadata_db.h"

typedef struct {
    const char * name;
    int albums, songs;         /* from the database counts, known at once */
    void (*on_play)(void);
    void (*on_shuffle)(void);
    void (*on_songs)(void);    /* opens the All Songs list */
    void (*on_menu)(void);     /* long press on the Songs row: the artist's menu */
    void (*on_songs_press)(bool pressed); /* optional: touch on the Songs row (true) / cannot become a tap (false) */
} gui_artist_page_params_t;

/* Builds the header (replacing any earlier one) and reserves its height at the
 * top of `list`. Call after the list's paged provider is set. */
void gui_artist_page_build(lv_obj_t * list, const gui_artist_page_params_t * params);
/* Drops every pointer into the list; call before the list is deleted. */
void gui_artist_page_forget(void);
/* Every UI tick. Redraws the header when new facts arrived and fills the
 * mosaic tiles from the shared thumbnail cache. Returns true when the album
 * rows should refetch their subtitles. */
bool gui_artist_page_poll(lv_obj_t * list, bool visible);
/* Frees the mosaic pixels (page left for good / final shutdown). */
void gui_artist_page_release(void);
