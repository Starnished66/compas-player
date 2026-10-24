#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/library/metadata_db.h"
#include "../src/library/tagcache.h"

/* Genre browsing does not consult these optional sidecar stores. */
void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *path, int32_t *rating, int32_t *playcount, int32_t *last_played) {
    (void)path; (void)rating; (void)playcount; (void)last_played;
    return false;
}

typedef struct {
    const char *genre, *artist, *album, *album_artist;
} song_t;

/* Issue #191's examples, an album without an album artist (falls back to the
 * artist), a lower-case spelling of the genre and a song of another genre. */
static const song_t songs[] = {
    { "Rock", "The Beatles", "Abbey Road", "The Beatles" },
    { "Rock", "The Beatles", "Abbey Road", "The Beatles" },
    { "rock", "The Beatles", "Let It Be", "The Beatles" },
    { "Rock", "Joan Baez", "Songs of Protest", "Various Artists" },
    { "Rock", "Pete Seeger", "Songs of Protest", "Various Artists" },
    { "Rock", "Queen", "Jazz", "" },
    { "Jazz", "Miles Davis", "Kind of Blue", "Miles Davis" },
};

static void add_song(int i, const song_t *song) {
    char path[TAGCACHE_PATH_MAX];
    cached_tags_t tags;
    memset(&tags, 0, sizeof(tags));
    snprintf(path, sizeof(path), "genre-%02d.flac", i);
    snprintf(tags.title, sizeof(tags.title), "Track %02d", i);
    snprintf(tags.artist, sizeof(tags.artist), "%s", song->artist);
    snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", song->album_artist);
    snprintf(tags.album, sizeof(tags.album), "%s", song->album);
    snprintf(tags.genre, sizeof(tags.genre), "%s", song->genre);
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fclose(file) == 0);
    assert(metadata_db_put(path, 1, 1, &tags));
}

typedef struct { const char *name, *album_artist; int songs; } want_t;

static void check(metadata_db_group_kind_t kind, metadata_db_group_kind_t artist_kind, const char *artist,
                  const want_t *want, int count) {
    group_row_t rows[8];
    int total = -1;
    int n = metadata_db_get_genre_groups_page(kind, "Rock", artist_kind, artist, 0, 8, rows, &total);
    assert(n == count && total == count);
    for (int i = 0; i < n; i++) {
        assert(strcmp(rows[i].name, want[i].name) == 0);
        assert(strcmp(rows[i].album_artist, want[i].album_artist) == 0);
        assert(rows[i].song_count == want[i].songs);
        assert(rows[i].first_song_id > 0);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH);
    metadata_db_begin_update();
    for (int i = 0; i < (int)(sizeof(songs) / sizeof(songs[0])); i++) add_song(i, &songs[i]);
    assert(metadata_db_end_update());

    const want_t artists[] = { { "Joan Baez", "", 1 }, { "Pete Seeger", "", 1 }, { "Queen", "", 1 },
                               { "The Beatles", "", 3 } };
    check(METADATA_DB_GROUP_ARTIST, METADATA_DB_GROUP_ARTIST, NULL, artists, 4);

    const want_t album_artists[] = { { "Queen", "", 1 }, { "The Beatles", "", 3 }, { "Various Artists", "", 2 } };
    check(METADATA_DB_GROUP_ALBUM_ARTIST, METADATA_DB_GROUP_ARTIST, NULL, album_artists, 3);

    const want_t albums[] = { { "Abbey Road", "The Beatles", 2 }, { "Jazz", "Queen", 1 },
                              { "Let It Be", "The Beatles", 1 }, { "Songs of Protest", "Various Artists", 2 } };
    check(METADATA_DB_GROUP_ALBUM, METADATA_DB_GROUP_ARTIST, NULL, albums, 4);

    /* Rock -> The Beatles -> Abbey Road, Let It Be. */
    const want_t beatles[] = { { "Abbey Road", "The Beatles", 2 }, { "Let It Be", "The Beatles", 1 } };
    check(METADATA_DB_GROUP_ALBUM, METADATA_DB_GROUP_ARTIST, "The Beatles", beatles, 2);
    /* Rock -> Various Artists -> Songs of Protest, and by one of its artists. */
    const want_t protest_all[] = { { "Songs of Protest", "Various Artists", 2 } };
    check(METADATA_DB_GROUP_ALBUM, METADATA_DB_GROUP_ALBUM_ARTIST, "Various Artists", protest_all, 1);
    const want_t protest_baez[] = { { "Songs of Protest", "Various Artists", 1 } };
    check(METADATA_DB_GROUP_ALBUM, METADATA_DB_GROUP_ARTIST, "Joan Baez", protest_baez, 1);

    /* Paging returns the same order; the total ignores the page size. */
    group_row_t page[2];
    int total = 0;
    assert(metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, "Rock", METADATA_DB_GROUP_ARTIST, NULL,
                                             1, 2, page, &total) == 2 && total == 4);
    assert(strcmp(page[0].name, "Jazz") == 0 && strcmp(page[1].name, "Let It Be") == 0);
    assert(metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ARTIST, "Rock", METADATA_DB_GROUP_ARTIST, NULL,
                                             0, 0, NULL, &total) == 0 && total == 4);

    /* Unknown genre and unsupported kinds return nothing. */
    assert(metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, "Polka", METADATA_DB_GROUP_ARTIST, NULL,
                                             0, 2, page, &total) == 0 && total == 0);
    assert(metadata_db_get_genre_groups_page(METADATA_DB_GROUP_GENRE, "Rock", METADATA_DB_GROUP_ARTIST, NULL,
                                             0, 2, page, &total) == 0 && total == 0);
    metadata_db_close();
    return 0;
}
