#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/library/metadata_db.h"
#include "../src/library/tagcache.h"

/* Album browsing does not consult these optional sidecar stores. */
void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *path, int32_t *rating, int32_t *playcount, int32_t *last_played) {
    (void)path; (void)rating; (void)playcount; (void)last_played;
    return false;
}

#define ALBUM_SORT_ARTIST METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR

typedef struct {
    const char *album;
    const char *album_artist;
    int year;
} expected_album_t;

typedef struct {
    uint32_t magic, version;
    int32_t generation, serial, commitid, count;
} album_order_meta_t;

static const expected_album_t expected[] = {
    { "Zeta", "alpha", 1980 },
    { "Aardvark", "alpha", 2000 },
    { "Aardvark", "Beta", 1970 },
    { "Same", "Beta", 1970 },
    { "Same", "Gamma", 2010 },
    { "Zebra", "Omega", 1999 },
    { "Aardvark", "Omega", -1 },
};

static void add_album(int i, const expected_album_t *album) {
    char path[TAGCACHE_PATH_MAX];
    cached_tags_t tags;
    const char *artist_source = i == 1 ? "Alpha" : album->album_artist;
    memset(&tags, 0, sizeof(tags));
    snprintf(path, sizeof(path), "sort-%02d.flac", i);
    snprintf(tags.title, sizeof(tags.title), "Track %02d", i);
    snprintf(tags.artist, sizeof(tags.artist), "%s", artist_source);
    snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", artist_source);
    snprintf(tags.album, sizeof(tags.album), "%s", album->album);
    snprintf(tags.genre, sizeof(tags.genre), "%s", i < 2 ? "Indie" : "Other");
    tags.release_year = album->year;
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fclose(file) == 0);
    assert(metadata_db_put(path, 1, 1, &tags));
}

static void check_page(int offset, int limit, const int *expected_indices) {
    group_row_t rows[4];
    int count = metadata_db_get_albums_page_sorted(NULL, ALBUM_SORT_ARTIST,
                                                   offset, limit, rows);
    assert(count == limit);
    for (int i = 0; i < count; i++) {
        const expected_album_t *want = &expected[expected_indices[i]];
        assert(strcmp(rows[i].name, want->album) == 0);
        assert(strcmp(rows[i].album_artist, want->album_artist) == 0);
    }
}

static int committed_generation(void) {
    int fd = open(".compas/tagcache.gen", O_RDONLY);
    assert(fd >= 0);
    char text[32] = {0};
    ssize_t n = read(fd, text, sizeof(text) - 1);
    assert(n > 0 && close(fd) == 0);
    return atoi(text);
}

static void emulate_version_one_album_order(int generation) {
    char path[128];
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_meta", generation);
    int fd = open(path, O_RDWR);
    assert(fd >= 0);
    album_order_meta_t meta;
    assert(read(fd, &meta, sizeof(meta)) == (ssize_t)sizeof(meta));
    assert(meta.version == 2 && meta.generation == generation);
    meta.version = 1;
    assert(pwrite(fd, &meta, sizeof(meta), 0) == (ssize_t)sizeof(meta));
    assert(close(fd) == 0);
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_artist_year", generation);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_artist_year_inverse", generation);
    assert(unlink(path) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH);
    metadata_db_begin_update();
    for (int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++)
        add_album(i, &expected[i]);
    assert(metadata_db_end_update());
    assert(metadata_db_album_sort_available(ALBUM_SORT_ARTIST));

    /* Artist is case-folded; within each artist release year is ascending,
     * with missing years after dated releases. Distinct same-title albums
     * remain distinct by album artist. */
    const int order[] = { 0, 1, 2, 3, 4, 5, 6 };
    check_page(0, 3, order);
    check_page(3, 4, order + 3);
    for (int rank = 0; rank < 7; rank++) {
        int canonical = metadata_db_album_sorted_to_canonical(ALBUM_SORT_ARTIST, rank);
        assert(canonical >= 0);
        assert(metadata_db_album_canonical_to_sorted(ALBUM_SORT_ARTIST, canonical) == rank);
        int offset = metadata_db_get_album_sorted_offset(ALBUM_SORT_ARTIST,
            expected[order[rank]].album, expected[order[rank]].album_artist);
        assert(offset == rank);
    }

    group_row_t filtered[4];
    int filtered_count = metadata_db_get_albums_page_sorted("Omega", ALBUM_SORT_ARTIST,
                                                            0, 4, filtered);
    assert(filtered_count == 2);
    assert(strcmp(filtered[0].name, "Zebra") == 0);
    assert(strcmp(filtered[1].name, "Aardvark") == 0);

    metadata_db_close();
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    check_page(0, 3, order);
    metadata_db_close();

    /* A version 1 album-order cache predates this index. It must retain both
     * existing sort modes while leaving the new mode unavailable until the
     * next ordinary library update rebuilds the derived indices. */
    int generation = committed_generation();
    emulate_version_one_album_order(generation);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT));
    assert(metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR));
    assert(!metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    group_row_t original_sort[3];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_YEAR,
                                               0, 3, original_sort) == 3);
    metadata_db_begin_update();
    assert(metadata_db_end_update());
    assert(metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT));
    assert(metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR));
    assert(metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    check_page(0, 3, order);
    group_row_t rebuilt_year_sort[3];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_YEAR,
                                               0, 3, rebuilt_year_sort) == 3);
    for (int i = 0; i < 3; i++) {
        assert(strcmp(original_sort[i].name, rebuilt_year_sort[i].name) == 0);
        assert(strcmp(original_sort[i].album_artist, rebuilt_year_sort[i].album_artist) == 0);
    }
    metadata_db_close();
    puts("PASS: album artist sort, paging, rank round trips, filtering and reopen");
    return 0;
}
