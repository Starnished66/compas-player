#include "gui_subsonic_artist_summary.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char * common_genre(const subsonic_album_t * albums, int count) {
    const char * best = "";
    int best_count = 0;
    for (int i = 0; i < count; i++) {
        const char * genre = albums[i].genre;
        if (!genre || !genre[0] || !strcmp(genre, "Unknown Genre")) continue;
        int matches = 0;
        for (int j = 0; j < count; j++)
            if (albums[j].genre && !strcmp(genre, albums[j].genre)) matches++;
        if (matches > best_count) { best = genre; best_count = matches; }
    }
    return best;
}

void gui_subsonic_artist_summary(const subsonic_album_t * albums, int count, gui_artist_summary_t * out) {
    *out = (gui_artist_summary_t){ .ready = true, .albums = count, .duration_known = count > 0 };
    for (int i = 0; i < count; i++) {
        const subsonic_album_t * a = &albums[i];
        if (a->song_count > 0)
            out->songs = a->song_count > INT_MAX - out->songs ? INT_MAX : out->songs + a->song_count;
        if (a->year > 0) {
            if (!out->year_min || a->year < out->year_min) out->year_min = a->year;
            if (a->year > out->year_max) out->year_max = a->year;
        }
        if (a->duration_seconds > 0) out->total_seconds += a->duration_seconds;
        else out->duration_known = false;
    }
    snprintf(out->genre, sizeof(out->genre), "%s", common_genre(albums, count));
    out->mosaic_count = count < ARTIST_PAGE_MOSAIC_ALBUMS ? count : ARTIST_PAGE_MOSAIC_ALBUMS;
}

void gui_subsonic_artist_summary_songs(const subsonic_song_t * songs, int count, gui_artist_summary_t * out) {
    out->songs = count;
    double seconds = 0;
    for (int i = 0; i < count; i++) {
        if (songs[i].duration_seconds <= 0) return;
        seconds += songs[i].duration_seconds;
    }
    out->total_seconds = seconds;
    out->duration_known = true;
}
