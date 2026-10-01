#define HOST_BUILD 1
#define LV_CONF_INCLUDE_SIMPLE 1
#include "gui_subsonic.c"
#include "../core/settings.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void test_download_folder_settings(void) {
    char out[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    assert(settings_validate_subsonic_download_subfolder("", out, sizeof(out)) && out[0] == '\0');
    assert(settings_validate_subsonic_download_subfolder("Music/Offline", out, sizeof(out)) &&
           strcmp(out, "Music/Offline") == 0);
    const char * invalid[] = {
        "/absolute", ".", "..", "./hidden", "Music/.cache", "Music/../Other",
        "Music//Other", "Music/", "Music\\Other", "Music\nOther", "Music\x01Other",
        "Music:Other", "Music*Other", "Music?Other", "Music\"Other", "Music<Other",
        "Music>Other", "Music|Other"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        strcpy(out, "preserve");
        assert(!settings_validate_subsonic_download_subfolder(invalid[i], out, sizeof(out)));
        assert(strcmp(out, "preserve") == 0);
    }
    char long_path[300];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    assert(!settings_validate_subsonic_download_subfolder(long_path, out, sizeof(out)));
    const char malformed_utf8[] = {'M','u','s','i','c','/',(char)0xE2,(char)0x82,'\0'};
    assert(!settings_validate_subsonic_download_subfolder(malformed_utf8, out, sizeof(out)));
    const char truncated_utf8[] = {'M','u','s','i','c','/',(char)0xE2,'\0'};
    assert(!settings_validate_subsonic_download_subfolder(truncated_utf8, out, sizeof(out)));
}

static void test_download_folder_creation(void) {
    char temp[] = "/tmp/subsonic-download-layout.XXXXXX";
    assert(mkdtemp(temp) != NULL);
    assert(chdir(temp) == 0);
    assert(mkdir("music", 0755) == 0);
    char path[PATH_MAX];
    assert(subsonic_ensure_library_album_dir("Offline/Server", 0, "Artist", "Album",
                                              path, sizeof(path)));
    assert(strcmp(path, "./music/Offline/Server/Artist/Album") == 0);
    assert(subsonic_ensure_library_album_dir("Offline/Server", 0, "Artist", "Album",
                                              path, sizeof(path)));
    assert(subsonic_ensure_library_album_dir("Offline", 1, "Artist", "Album",
                                              path, sizeof(path)));
    assert(strcmp(path, "./music/Offline/Artist - Album") == 0);
    assert(!subsonic_ensure_library_album_dir("", 1, "Artist", "Album", path, 8));
    assert(subsonic_ensure_library_album_dir("", 0, "_Hidden", "_5", path, sizeof(path)));

    int fd = open("music/file-collision", O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0 && close(fd) == 0);
    assert(!subsonic_ensure_library_album_dir("file-collision/child", 0, "Artist", "Album",
                                               path, sizeof(path)));
    assert(mkdir("outside", 0755) == 0);
    assert(symlink("../outside", "music/escape") == 0);
    assert(!subsonic_ensure_library_album_dir("escape/child", 0, "Artist", "Album",
                                               path, sizeof(path)));
    assert(access("outside/child", F_OK) != 0);
}

static void test_download_component_sanitizing_and_long_layout(void) {
    char safe[256];
    sanitize_path_component(".5", safe, sizeof(safe));
    assert(strcmp(safe, "_5") == 0);
    sanitize_path_component("...And Justice for All", safe, sizeof(safe));
    assert(strcmp(safe, "_..And Justice for All") == 0);
    sanitize_path_component("A\001B\177C", safe, sizeof(safe));
    assert(strcmp(safe, "A_B_C") == 0);

    char temp[] = "/tmp/subsonic-download-sanitized.XXXXXX";
    assert(mkdtemp(temp) != NULL && chdir(temp) == 0 && mkdir("music", 0755) == 0);
    char path[1024], safe_artist[256], safe_album[256];
    sanitize_path_component(".5", safe_artist, sizeof(safe_artist));
    sanitize_path_component("...And Justice for All", safe_album, sizeof(safe_album));
    assert(subsonic_ensure_library_album_dir("", 0, safe_artist, safe_album, path, sizeof(path)));
    assert(strcmp(path, "./music/_5/_..And Justice for All") == 0);

    char artist[256], album_a[256], album_b[256], path_a[1024], path_b[1024];
    append_repeated(artist, sizeof(artist), "猫", 70);
    append_repeated(album_a, sizeof(album_a), "猫", 50);
    snprintf(album_a + strlen(album_a), sizeof(album_a) - strlen(album_a), " First");
    append_repeated(album_b, sizeof(album_b), "猫", 50);
    snprintf(album_b + strlen(album_b), sizeof(album_b) - strlen(album_b), " Second");
    assert(subsonic_ensure_library_album_dir("", 1, artist, album_a, path_a, sizeof(path_a)));
    assert(subsonic_ensure_library_album_dir("", 1, artist, album_b, path_b, sizeof(path_b)));
    const char * leaf_a = strrchr(path_a, '/') + 1;
    const char * leaf_b = strrchr(path_b, '/') + 1;
    assert(strlen(leaf_a) <= 255 && strlen(leaf_b) <= 255);
    assert(is_valid_utf8(leaf_a) && is_valid_utf8(leaf_b));
    assert(strcmp(leaf_a, leaf_b) != 0);
    assert(strchr(leaf_a, '~') != NULL && strchr(leaf_b, '~') != NULL);
    assert(chdir("/") == 0);
}

int main(void) {
    test_download_folder_settings();
    test_download_folder_creation();
    test_download_component_sanitizing_and_long_layout();
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
