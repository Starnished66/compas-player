#include "../gui_subsonic_artist_summary.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_albums(void) {
    gui_artist_summary_t s;
    subsonic_album_t albums[] = {
        { .year = 1980, .song_count = 8, .duration_seconds = 1800, .genre = "Rock" },
        { .year = 1990, .song_count = 10, .duration_seconds = 2400, .genre = "Jazz" },
        { .year = 1985, .song_count = 9, .duration_seconds = 2100, .genre = "Rock" },
        { .year = 0, .song_count = 0, .duration_seconds = 0, .genre = "" },
        { .year = -1, .song_count = -1, .duration_seconds = -1, .genre = "Unknown Genre" },
    };
    gui_subsonic_artist_summary(albums, 3, &s);
    assert(s.ready && s.albums == 3 && s.songs == 27);
    assert(s.year_min == 1980 && s.year_max == 1990);
    assert(!strcmp(s.genre, "Rock") && s.total_seconds == 6300 && s.duration_known);
    assert(s.mosaic_count == 3);
    gui_subsonic_artist_summary(albums, 5, &s);
    assert(s.albums == 5 && s.songs == 27 && !s.duration_known);
    assert(s.mosaic_count == 4 && s.total_seconds == 6300);
    gui_subsonic_artist_summary(albums, 2, &s);
    assert(!strcmp(s.genre, "Rock")); /* Equal counts retain the first genre. */
    gui_subsonic_artist_summary(NULL, 0, &s);
    assert(s.ready && !s.albums && !s.songs && !s.genre[0] && !s.duration_known);
    assert(!s.year_min && !s.year_max && !s.mosaic_count);
    albums[0].song_count = INT_MAX;
    gui_subsonic_artist_summary(albums, 2, &s);
    assert(s.songs == INT_MAX);
}


static void test_fetched_songs(void) {
    subsonic_album_t album = { .song_count = 41, .duration_seconds = 4000, .year = 2020, .genre = "Rock" };
    subsonic_song_t songs[33] = {0};
    gui_artist_summary_t s;
    gui_subsonic_artist_summary(&album, 1, &s);
    for (int i = 0; i < 33; i++) songs[i].duration_seconds = 100;
    gui_subsonic_artist_summary_songs(songs, 33, &s);
    assert(s.songs == 33 && s.total_seconds == 3300 && s.duration_known);
    assert(s.albums == 1 && s.year_min == 2020 && !strcmp(s.genre, "Rock"));
    assert(s.ready && s.mosaic_count == 1);
    gui_subsonic_artist_summary_songs(NULL, 0, &s);
    assert(!s.songs && !s.total_seconds && s.duration_known && s.albums == 1);
}

static void test_missing_durations(void) {
    subsonic_song_t songs[] = { { .duration_seconds = 100 }, { .duration_seconds = 0 } };
    gui_artist_summary_t s = { .songs = 41, .total_seconds = 4000, .duration_known = true };
    gui_subsonic_artist_summary_songs(songs, 2, &s);
    assert(s.songs == 2 && s.total_seconds == 4000 && s.duration_known);
    songs[1].duration_seconds = -1;
    s.duration_known = false;
    gui_subsonic_artist_summary_songs(songs, 2, &s);
    assert(s.songs == 2 && s.total_seconds == 4000 && !s.duration_known);
    songs[1].duration_seconds = 200;
    gui_subsonic_artist_summary_songs(songs, 2, &s);
    assert(s.songs == 2 && s.total_seconds == 300 && s.duration_known);
    songs[0].duration_seconds = INT_MAX;
    songs[1].duration_seconds = INT_MAX;
    gui_subsonic_artist_summary_songs(songs, 2, &s);
    assert(s.total_seconds == (double) INT_MAX * 2 && s.duration_known);
}

int main(void) {
    test_albums();
    test_fetched_songs();
    test_missing_durations();
    puts("Subsonic artist summary tests passed");
    return 0;
}
