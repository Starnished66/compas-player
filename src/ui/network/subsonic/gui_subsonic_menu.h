#pragma once
/* The Subsonic server menu: a category list like Music, with a settings button in the header. */
#include <lvgl/lvgl.h>

typedef struct {
    /* row clicks, in menu order */
    lv_event_cb_t artists, albums, genres, random_songs, playlists, favorites, cover_flow;
    lv_event_cb_t settings;                               /* header button click */
} gui_subsonic_menu_actions_t;

/* Builds the menu; the callbacks are kept for later rebuilds. */
void gui_subsonic_menu_init(const gui_subsonic_menu_actions_t * actions);
lv_obj_t * gui_subsonic_menu_screen(void);
/* Rebuilds the menu after a theme or layout change, keeping its place in navigation. */
void gui_subsonic_menu_refresh(void);
void gui_subsonic_menu_teardown(void);
