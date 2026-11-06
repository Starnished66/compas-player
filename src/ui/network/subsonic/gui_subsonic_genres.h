#pragma once
/* The Subsonic genre list: one row per genre with its album count. */
#include <lvgl/lvgl.h>
#include "../../../network/services/subsonic_client.h"

/* pick is called with the tapped genre's name. */
void gui_subsonic_genres_init(void (*pick)(const char * name));
/* Takes the genres and shows the screen. */
bool gui_subsonic_genres_show(subsonic_genre_t * genres, int count);
void gui_subsonic_genres_teardown(void);
