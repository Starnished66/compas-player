#pragma once
#include "gui_subsonic_internal.h"

void gui_subsonic_artist_open(lv_obj_t * list, const char * name, const char * cover_id);
void gui_subsonic_artist_clear(lv_obj_t * list);
void gui_subsonic_artist_poll(void);
bool gui_subsonic_artist_accept_songs(subsonic_song_t * songs, int count);
