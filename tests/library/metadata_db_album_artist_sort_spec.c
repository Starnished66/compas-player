#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/library/metadata/db/metadata_db.h"
#include "../../src/library/tagcache/core/tagcache.h"

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

static void assert_album_order_cache_version(int wanted) {
    int generation = committed_generation();
    char path[128];
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_meta.v4", generation);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    album_order_meta_t meta;
    assert(read(fd, &meta, sizeof(meta)) == (ssize_t)sizeof(meta));
    assert(close(fd) == 0);
    assert(meta.version == (uint32_t)wanted && meta.generation == generation);
}

static void emulate_version_one_album_order(int generation) {
    char path[128];
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_meta.v4", generation);
    int fd = open(path, O_RDWR);
    assert(fd >= 0);
    album_order_meta_t meta;
    assert(read(fd, &meta, sizeof(meta)) == (ssize_t)sizeof(meta));
    assert(meta.version == 4 && meta.generation == generation);
    meta.version = 1;
    assert(pwrite(fd, &meta, sizeof(meta), 0) == (ssize_t)sizeof(meta));
    assert(close(fd) == 0);
    static const char *const versioned[] = {
        ".ialbum_recent.v4", ".ialbum_recent_inverse.v4",
        ".ialbum_year.v4", ".ialbum_year_inverse.v4",
    };
    static const char *const legacy[] = {
        ".ialbum_recent", ".ialbum_recent_inverse", ".ialbum_year", ".ialbum_year_inverse",
    };
    for (size_t i = 0; i < sizeof(versioned) / sizeof(versioned[0]); ++i) {
        char old_path[128], new_path[128];
        snprintf(old_path, sizeof(old_path), ".compas/database_idx.tcd.g%d%s", generation, versioned[i]);
        snprintf(new_path, sizeof(new_path), ".compas/database_idx.tcd.g%d%s", generation, legacy[i]);
        assert(rename(old_path, new_path) == 0);
    }
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_artist_year.v4", generation);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.ialbum_artist_year_inverse.v4", generation);
    assert(unlink(path) == 0);
}

static void test_unicode_group_identity(void) {
    static const struct { const char *path, *artist, *album, *album_artist; } rows[] = {
        { "unicode-group-u01.flac", "Angel", "Eclair", "Angel" },
        { "unicode-group-u02.flac", "Ángel", "Éclair", "Ángel" },
        { "unicode-group-u03.flac", "Elodie", "...and Soon", "Elodie" },
        { "unicode-group-u04.flac", "Élodie", "’Til", "Élodie" },
        { "unicode-group-u05.flac", "The International Society of Æther", "Long Album", "The International Society of Æther" },
        { "unicode-group-u06.flac", "The International Society of Ángel", "Long Album", "The International Society of Ángel" },
    };
    metadata_db_begin_update();
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        cached_tags_t tags = {0};
        snprintf(tags.title, sizeof(tags.title), "Track %zu", i + 1);
        snprintf(tags.artist, sizeof(tags.artist), "%s", rows[i].artist);
        snprintf(tags.album, sizeof(tags.album), "%s", rows[i].album);
        snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", rows[i].album_artist);
        tags.track_number = i + 1;
        tags.disc_number = -1;
        FILE *file = fopen(rows[i].path, "wb");
        assert(file && fclose(file) == 0);
        assert(metadata_db_put(rows[i].path, 1, 1, &tags));
    }
    assert(metadata_db_end_update());

    const char *const want_artists[] = { "Angel", "Ángel", "Elodie", "Élodie" };
    for (metadata_db_group_kind_t kind = METADATA_DB_GROUP_ARTIST;
         kind <= METADATA_DB_GROUP_ALBUM_ARTIST; ++kind) {
        for (int i = 0; i < 4; ++i) {
            int64_t offset = metadata_db_get_group_offset(kind, want_artists[i], NULL);
            assert(offset >= 0);
            group_row_t row;
            assert(metadata_db_get_groups_page(kind, (int)offset, 1, &row) == 1);
            assert(strcmp(row.name, want_artists[i]) == 0);
        }
        int64_t long_a = metadata_db_get_group_offset(kind, "The International Society of Æther", NULL);
        int64_t long_b = metadata_db_get_group_offset(kind, "The International Society of Ángel", NULL);
        assert(long_a >= 0 && long_b == long_a + 1);
        group_row_t long_page[2];
        assert(metadata_db_get_groups_page(kind, (int)long_a, 2, long_page) == 2);
        assert(strcmp(long_page[0].name, "The International Society of Æther") == 0);
        assert(strcmp(long_page[1].name, "The International Society of Ángel") == 0);
    }

    /* Collation-equivalent spellings remain distinct group keys. Their
     * secondary identity comparison gives paging a stable total order. */
    int64_t eclair = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, "Eclair", "Angel");
    int64_t eclair_accent = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, "Éclair", "Ángel");
    assert(eclair >= 0 && eclair_accent == eclair + 1);
    group_row_t page[2];
    assert(metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int)eclair, 2, page) == 2);
    assert(strcmp(page[0].name, "Eclair") == 0 && strcmp(page[0].album_artist, "Angel") == 0);
    assert(strcmp(page[1].name, "Éclair") == 0 && strcmp(page[1].album_artist, "Ángel") == 0);
}

static void add_browse_album(const char *path, const char *album, const char *album_artist) {
    cached_tags_t tags = {0};
    snprintf(tags.title, sizeof(tags.title), "Track %s", album);
    snprintf(tags.artist, sizeof(tags.artist), "%s", album_artist);
    snprintf(tags.album, sizeof(tags.album), "%s", album);
    snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", album_artist);
    tags.track_number = 1;
    tags.disc_number = -1;
    FILE *file = fopen(path, "wb");
    assert(file && fclose(file) == 0);
    assert(metadata_db_put(path, 1, 1, &tags));
}

static void check_browse_album_title_pages(void) {
    static const struct { const char *name, *album_artist; } expected_groups[] = {
        { "1940 Carmen", "Bizet" },
        { "Aardvark", "alpha" },
        { "Aardvark", "Beta" },
        { "Aardvark", "Omega" },
        { "L/N", "Various" },
        { "[MESSAGE FROM THE CLERGY] Ghost", "Ghost" },
        { "Same", "Beta" },
        { "Same", "Gamma" },
        { "Zebra", "Omega" },
        { "Zeta", "alpha" },
    };
    group_row_t page[4];
    const int offsets[] = { 0, 4, 8 };
    const int limits[] = { 4, 4, 2 };
    for (int p = 0; p < 3; ++p) {
        assert(metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, offsets[p], limits[p], page) == limits[p]);
        for (int i = 0; i < limits[p]; ++i) {
            const size_t at = (size_t)offsets[p] + (size_t)i;
            assert(strcmp(page[i].name, expected_groups[at].name) == 0);
            assert(strcmp(page[i].album_artist, expected_groups[at].album_artist) == 0);
        }
    }
}

static void test_browse_album_title_order(void) {
    metadata_db_begin_update();
    add_browse_album("browse-1940-carmen.flac", "1940 Carmen", "Bizet");
    add_browse_album("browse-l-n.flac", "L/N", "Various");
    add_browse_album("browse-message-from-clergy.flac", "[MESSAGE FROM THE CLERGY] Ghost", "Ghost");
    assert(metadata_db_end_update());
    assert_album_order_cache_version(4);
    check_browse_album_title_pages();
    metadata_db_close();
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert_album_order_cache_version(4);
    check_browse_album_title_pages();
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH);
    metadata_db_begin_update();
    for (int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++)
        add_album(i, &expected[i]);
    assert(metadata_db_end_update());
    assert(metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    assert_album_order_cache_version(4);

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

    group_row_t original_year_sort[3];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_YEAR,
                                               0, 3, original_year_sort) == 3);

    metadata_db_close();
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    check_page(0, 3, order);
    metadata_db_close();

    /* Version 1 metadata under a v4 filename is invalid. Fall back to name
     * ordering until an ordinary library update rebuilds all cached sorts. */
    int generation = committed_generation();
    emulate_version_one_album_order(generation);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(!metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT));
    assert(!metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR));
    assert(!metadata_db_album_sort_available(ALBUM_SORT_ARTIST));
    group_row_t fallback_sort[3], name_sort[3];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_YEAR,
                                               0, 3, fallback_sort) == 3);
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_NAME,
                                               0, 3, name_sort) == 3);
    for (int i = 0; i < 3; ++i) {
        assert(strcmp(fallback_sort[i].name, name_sort[i].name) == 0);
        assert(strcmp(fallback_sort[i].album_artist, name_sort[i].album_artist) == 0);
    }
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
        assert(strcmp(original_year_sort[i].name, rebuilt_year_sort[i].name) == 0);
        assert(strcmp(original_year_sort[i].album_artist, rebuilt_year_sort[i].album_artist) == 0);
    }
    test_browse_album_title_order();
    test_unicode_group_identity();
    metadata_db_close();
    puts("PASS: album artist sort, paging, rank round trips, filtering and reopen");
    return 0;
}
