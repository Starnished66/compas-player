#define HOST_BUILD 1
#define LV_CONF_INCLUDE_SIMPLE 1
#include "gui_subsonic.c"
#include <assert.h>

static void append_repeated(char * out, size_t out_size, const char * unit, int count) {
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(unit);
        assert(used + len < out_size);
        memcpy(out + used, unit, len);
        used += len;
    }
    out[used] = '\0';
}

static int is_valid_utf8(const char * text) {
    const unsigned char * p = (const unsigned char *) text;
    while (*p) {
        size_t len = *p < 0x80 ? 1 : (*p >= 0xC2 && *p <= 0xDF ? 2 :
                     (*p >= 0xE0 && *p <= 0xEF ? 3 : (*p >= 0xF0 && *p <= 0xF4 ? 4 : 0)));
        if (!len) return 0;
        p++;
        for (size_t i = 1; i < len; i++, p++) if (!*p || (*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

int main(void) {
    subsonic_song_t song = {0};
    char name[512];
    snprintf(song.album_artist, sizeof(song.album_artist), "Artist");
    snprintf(song.artist, sizeof(song.artist), "Artist");
    snprintf(song.suffix, sizeof(song.suffix), "mp3");
    song.track = 1;
    subsonic_build_download_filename(&song, "Title", name, sizeof(name));
    assert(strcmp(name, "01 - Title.mp3") == 0);

    song.disc = 2;
    subsonic_build_download_filename(&song, "Title", name, sizeof(name));
    assert(strcmp(name, "2-01 - Title.mp3") == 0);

    song.track = 0;
    subsonic_build_download_filename(&song, "Title", name, sizeof(name));
    assert(strcmp(name, "2 - Title.mp3") == 0);

    song.disc = 1;
    song.track = 1;
    snprintf(song.artist, sizeof(song.artist), "Guest/Artist:One");
    subsonic_build_download_filename(&song, "Title", name, sizeof(name));
    assert(strcmp(name, "01 - Title - Guest_Artist_One.mp3") == 0);
    char first_artist_name[512];
    snprintf(first_artist_name, sizeof(first_artist_name), "%s", name);
    snprintf(song.artist, sizeof(song.artist), "Another Artist");
    subsonic_build_download_filename(&song, "Title", name, sizeof(name));
    assert(strcmp(first_artist_name, name) != 0);

    song.track = 0;
    snprintf(song.artist, sizeof(song.artist), "Guest/Artist:One");
    subsonic_build_download_filename(&song, "Untitled", name, sizeof(name));
    assert(strcmp(name, "Untitled - Guest_Artist_One.mp3") == 0);

    char long_title[160];
    append_repeated(long_title, sizeof(long_title), "猫", 40);
    song.track = 1;
    song.disc = 1;
    song.artist[0] = '\0';
    append_repeated(song.artist, sizeof(song.artist), "猫", 40);
    strncat(song.artist, "ArtistA", sizeof(song.artist) - strlen(song.artist) - 1);
    snprintf(song.id, sizeof(song.id), "long-id-a");
    subsonic_build_download_filename(&song, long_title, name, sizeof(name));
    assert(strlen(name) <= 255);
    assert(is_valid_utf8(name));
    char long_artist_a[512];
    snprintf(long_artist_a, sizeof(long_artist_a), "%s", name);

    song.artist[0] = '\0';
    append_repeated(song.artist, sizeof(song.artist), "猫", 40);
    strncat(song.artist, "ArtistB", sizeof(song.artist) - strlen(song.artist) - 1);
    snprintf(song.id, sizeof(song.id), "long-id-b");
    subsonic_build_download_filename(&song, long_title, name, sizeof(name));
    assert(strlen(name) <= 255);
    assert(is_valid_utf8(name));
    assert(strcmp(long_artist_a, name) != 0);
    puts("Subsonic download filenames: PASS");
    return 0;
}
