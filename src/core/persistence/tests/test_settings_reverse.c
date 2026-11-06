/* Exercise the real loader and file writer in an isolated working directory. */
#include <assert.h>
#include "src/core/persistence/settings.c"

/* Keep the saved-server sidecar independent of the settings fixture. */
void subsonic_saved_servers_load(subsonic_saved_server_t ** rows, int * count) {
    *rows = NULL;
    *count = 0;
}
void subsonic_saved_servers_upsert(const char * url, const char * username,
                                 const char * password, bool verify_tls) {
    (void) url; (void) username; (void) password; (void) verify_tls;
}

static void check_reverse(const player_settings_t *settings, bool expected) {
    assert(settings->file_sort_reverse == expected);
    assert(settings->album_sort_reverse == expected);
    assert(settings->artist_albums_sort_reverse == expected);
    assert(settings->album_artist_albums_sort_reverse == expected);
}
int main(void) {
    assert(mkdir(INTERNAL_COMPAS_DIR, 0700) == 0);
    player_settings_t settings, loaded;
    set_defaults(&settings);
    check_reverse(&settings, false);
    FILE *file = fopen(SETTINGS_FILE_PATH, "w");
    assert(file);
    fputs("album_sort_mode=2\nfile_sort_mode=1\n", file);
    assert(fclose(file) == 0);
    assert(settings_load(&loaded));
    check_reverse(&loaded, false);
    assert(loaded.album_sort_mode == 2 && loaded.file_sort_mode == 1);
    settings.file_sort_reverse = settings.album_sort_reverse = true;
    settings.artist_albums_sort_reverse = settings.album_artist_albums_sort_reverse = true;
    settings_write_file(&settings);
    assert(settings_load(&loaded));
    check_reverse(&loaded, true);
    settings_write_file(&loaded);
    assert(settings_load(&settings));
    check_reverse(&settings, true);
    puts("reverse settings: ok");
    return 0;
}
