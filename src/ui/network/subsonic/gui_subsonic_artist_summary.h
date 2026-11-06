#pragma once
#include "../../library/browse/gui_artist_data.h"
#include "../../../network/services/subsonic_client.h"

/* Album metadata supplies the initial artist header, including offline views. */
void gui_subsonic_artist_summary(const subsonic_album_t * albums, int count, gui_artist_summary_t * out);

/* Use fetched counts; retain the album duration if any song duration is missing. */
void gui_subsonic_artist_summary_songs(const subsonic_song_t * songs, int count, gui_artist_summary_t * out);
