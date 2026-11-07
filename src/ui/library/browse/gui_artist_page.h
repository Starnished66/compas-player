#pragma once
/* The artist page header: a mosaic of the first albums' covers, name, counts,
 * genre, year-span and total-time chips, Play / Shuffle, the "Songs" row and
 * the "Albums" label. It is a child of the artist's album list (a compact
 * list) and scrolls with it; the album rows start below it.
 *
 * Facts default to gui_artist_data.c, or come from the caller's source; this
 * module only draws them on the UI thread. */
#include <stdbool.h>
#include "gui_artist_data.h"
#include <lvgl/lvgl.h>
#include "../../../library/metadata/db/metadata_db.h"

typedef struct {
    /* Optional external facts and copied RGB565 thumbnails (ALBUM_THUMBNAIL_PX). */
    void * source;
    void (*snapshot)(void *, gui_artist_summary_t *, gui_artist_revisions_t *);
    bool (*copy_artist)(void *, uint16_t * pixels, bool * no_art);
    bool (*copy_tile)(void *, int album, uint16_t * pixels, bool * no_art);
    const char * name;
    int albums, songs;         /* from the database counts, known at once */
    bool counts_from_summary; /* update song count when external facts change */
    void (*on_play)(void);
    void (*on_shuffle)(void);
    void (*on_songs)(void);    /* opens the All Songs list */
    void (*on_menu)(void);     /* long press on the Songs row: the artist's menu */
    void (*on_songs_press)(bool pressed); /* optional: touch on the Songs row (true) / cannot become a tap (false) */
} gui_artist_page_params_t;

/* Builds the header (replacing any earlier one) and reserves its height at the
 * top of `list`. Call after the list's paged provider is set. */
void gui_artist_page_build(lv_obj_t * list, const gui_artist_page_params_t * params);
/* Deletes the header and drops its pointers; call before deleting the list. */
void gui_artist_page_forget(lv_obj_t * list);
/* Every UI tick. Redraws the header when new facts arrived and fills the
 * mosaic tiles from the shared thumbnail cache. Returns true when the album
 * rows should refetch their subtitles. */
bool gui_artist_page_poll(lv_obj_t * list, bool visible);
/* Frees the mosaic pixels (page left for good / final shutdown). */
void gui_artist_page_release(lv_obj_t * list);

void gui_artist_page_album_subtitle(char * text, size_t size, int year, int tracks);
