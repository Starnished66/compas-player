#include "gui_player.c"

subsonic_stream_song_meta_t *subsonic_stream_meta;
int subsonic_stream_meta_count;
bool subsonic_library_download_active;
bool subsonic_connect_active;

/* The only retained playlist_path_at() fallbacks are unreachable for these
 * populated test slots, but provide their external symbols for the linker. */
bool metadata_db_snapshot_path_at(const struct tagcache_snapshot *snapshot, int pos, char *out, size_t size) {
    (void)snapshot; (void)pos; (void)out; (void)size; return false;
}
bool file_browser_index_playable_path_at(const file_browser_index_t *index, unsigned pos, char *out, size_t size) {
    (void)index; (void)pos; (void)out; (void)size; return false;
}

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void) {
    char *paths[] = {"https://host/slot-zero", "https://host/slot-one", "https://host/slot-two"};
    subsonic_stream_song_meta_t meta[3] = {0};
    playlist = paths;
    playlist_count = 3;
    playlist_lazy_sort_order = NULL;
    playlist_lazy_snapshot = NULL;
    playlist_lazy_file_index = NULL;
    subsonic_stream_meta = meta;
    subsonic_stream_meta_count = 3;
    snprintf(meta[0].url, sizeof(meta[0].url), "%s", paths[0]);
    snprintf(meta[0].title, sizeof(meta[0].title), "Title zero");
    snprintf(meta[0].artist, sizeof(meta[0].artist), "Artist zero");
    snprintf(meta[0].album, sizeof(meta[0].album), "Album zero");
    snprintf(meta[2].url, sizeof(meta[2].url), "%s", paths[2]);
    snprintf(meta[2].title, sizeof(meta[2].title), "Title two");
    snprintf(meta[2].artist, sizeof(meta[2].artist), "Artist two");
    snprintf(meta[2].album, sizeof(meta[2].album), "Album two");

    char title[128], artist[128], album[128];
    /* Visible queue ordering may be shuffled: slot 2 must resolve record 2,
     * independent of its displayed row position. */
    CHECK(gui_player_get_subsonic_track_identity(2, paths[2], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title two") == 0 && strcmp(artist, "Artist two") == 0 && strcmp(album, "Album two") == 0);
    CHECK(gui_player_get_subsonic_track_identity(1, paths[1], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    CHECK(gui_player_get_subsonic_track_identity(0, paths[0], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title zero") == 0 && strcmp(artist, "Artist zero") == 0 && strcmp(album, "Album zero") == 0);
    CHECK(gui_player_get_subsonic_track_identity(3, paths[2], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    CHECK(gui_player_get_subsonic_track_identity(-1, paths[0], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    CHECK(gui_player_get_subsonic_track_identity(0, "https://host/other", title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    char saved_url[sizeof(meta[0].url)];
    snprintf(saved_url, sizeof(saved_url), "%s", meta[0].url);
    snprintf(meta[0].url, sizeof(meta[0].url), "%s", "https://host/stale-metadata");
    CHECK(gui_player_get_subsonic_track_identity(0, paths[0], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    snprintf(meta[0].url, sizeof(meta[0].url), "%s", saved_url);
    CHECK(gui_player_get_subsonic_track_identity(0, NULL, title, sizeof(title), artist, sizeof(artist), album, sizeof(album)) == false);
    CHECK(title[0] == '\0' && artist[0] == '\0' && album[0] == '\0');
    CHECK(gui_player_get_subsonic_track_identity(0, paths[0], NULL, 0, NULL, 0, NULL, 0));
    snprintf(meta[1].url, sizeof(meta[1].url), "%s", paths[1]);
    snprintf(meta[1].title, sizeof(meta[1].title), "Title one");
    snprintf(meta[1].artist, sizeof(meta[1].artist), "Artist one");
    snprintf(meta[1].album, sizeof(meta[1].album), "Album one");

    /* Adding a local track shifts the original stream slots. The resolver
     * must find stream records by URL, while rejecting the inserted path. */
    char * edited[] = {paths[0], "file:///music/local.mp3", paths[1], paths[2]};
    playlist = edited;
    playlist_count = 4;
    CHECK(gui_player_get_subsonic_track_identity(2, paths[1], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title one") == 0);
    CHECK(gui_player_get_subsonic_track_identity(3, paths[2], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title two") == 0);
    CHECK(!gui_player_get_subsonic_track_identity(1, edited[1], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));

    /* Materializing a shuffled display order physically rearranges the
     * playlist; metadata still resolves to the matching URL. */
    char * shuffled[] = {paths[2], paths[0], edited[1], paths[1]};
    playlist = shuffled;
    CHECK(gui_player_get_subsonic_track_identity(0, paths[2], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title two") == 0);
    CHECK(gui_player_get_subsonic_track_identity(3, paths[1], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title one") == 0);

    /* Deleting a row shifts slots again; exact URL association remains. */
    char * deleted[] = {paths[2], paths[1]};
    playlist = deleted;
    playlist_count = 2;
    CHECK(gui_player_get_subsonic_track_identity(0, paths[2], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title two") == 0);
    CHECK(gui_player_get_subsonic_track_identity(1, paths[1], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));
    CHECK(strcmp(title, "Title one") == 0);
    CHECK(!gui_player_get_subsonic_track_identity(1, paths[0], title, sizeof(title), artist, sizeof(artist), album, sizeof(album)));

    if (failures) return 1;
    puts("Subsonic queue identity helper checks passed");
    return 0;
}
