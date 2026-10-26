#define _GNU_SOURCE
#include "../db/metadata_db.h"
#include "../../tagcache/core/tagcache.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void path_cache_drop(void) {}
void remote_state_drop(void) {}
__attribute__((weak)) int tagcache_title_order_version(void) { return 0; }
bool remote_state_get(const char *path, int32_t *rating, int32_t *count, int32_t *last) {
    (void)path; (void)rating; (void)count; (void)last;
    return false;
}
bool remote_state_set_rating(const char *path, int32_t rating) {
    (void)path; (void)rating;
    return false;
}
bool remote_state_take(const char *path, int32_t *rating, int32_t *count, int32_t *last) {
    (void)path; (void)rating; (void)count; (void)last;
    return false;
}

static void must(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "metadata catalog title order: %s\n", what);
        exit(1);
    }
}

static void assert_revision_getter_matches(const char *expected, const char *what) {
    char revision[METADATA_DB_CATALOG_REVISION_SIZE] = "stale-output";
    must(metadata_db_catalog_get_revision(revision, sizeof(revision)), what);
    must(strcmp(revision, expected) == 0, "revision getter matches the catalog page token");
}

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    must(fd >= 0 && close(fd) == 0, "create fixture song file");
}

static void put_song(const char *path, const char *title, const char *album_artist) {
    cached_tags_t tags = {0};
    snprintf(tags.title, sizeof(tags.title), "%s", title);
    snprintf(tags.artist, sizeof(tags.artist), "Artist");
    snprintf(tags.album, sizeof(tags.album), "Album");
    snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", album_artist);
    tags.track_number = 1;
    tags.disc_number = -1;
    must(metadata_db_put(path, 1234, 10, &tags), "insert fixture song");
}

static void expected_paths(char *shared_fallback_path, size_t path_size,
                           char expected[6][160]) {
    char shared_prefix[41];
    memset(shared_prefix, 'x', sizeof(shared_prefix) - 1);
    shared_prefix[sizeof(shared_prefix) - 1] = '\0';
    char shared_tagged_title[64];
    snprintf(shared_tagged_title, sizeof(shared_tagged_title), "%sA", shared_prefix);
    snprintf(shared_fallback_path, path_size, "music/%sB.flac", shared_prefix);
    snprintf(expected[0], 160, "music/tagged-alpha.flac");
    snprintf(expected[1], 160, "music/aLpHa.flac");
    snprintf(expected[2], 160, "music/tagged-bravo.flac");
    snprintf(expected[3], 160, "music/tagged-charlie.flac");
    snprintf(expected[4], 160, "music/tagged-shared.flac");
    snprintf(expected[5], 160, "%s", shared_fallback_path);
}

static void create_fixture(const char *shared_fallback_path) {
    must(mkdir("music", 0755) == 0, "create nested music directory");
    touch("music/tagged-alpha.flac");
    touch("music/aLpHa.flac");
    touch("music/tagged-bravo.flac");
    touch("music/tagged-charlie.flac");
    touch("music/tagged-shared.flac");
    touch(shared_fallback_path);

    char shared_prefix[41];
    memset(shared_prefix, 'x', sizeof(shared_prefix) - 1);
    shared_prefix[sizeof(shared_prefix) - 1] = '\0';
    char shared_tagged_title[64];
    snprintf(shared_tagged_title, sizeof(shared_tagged_title), "%sA", shared_prefix);
    metadata_db_begin_update();
    put_song("music/tagged-alpha.flac", "Alpha", "Ángel");
    put_song("music/aLpHa.flac", "", "Zed");
    put_song("music/tagged-bravo.flac", "BRAVO", "¿Bolero");
    put_song("music/tagged-charlie.flac", "charlie", "Zed");
    put_song("music/tagged-shared.flac", shared_tagged_title, "Ángel");
    put_song(shared_fallback_path, "", "Zed");
    must(metadata_db_end_update(), "commit fixture songs");
}

static void assert_index_version(uint32_t wanted) {
    int32_t generation = metadata_db_get_generation();
    char path[160];
    snprintf(path, sizeof(path), ".compas/database_idx.tcd.g%d.imeta", generation);
    int fd = open(path, O_RDONLY);
    must(fd >= 0, "open derived-index metadata");
    uint32_t header[2] = {0};
    must(read(fd, header, sizeof(header)) == (ssize_t)sizeof(header) && close(fd) == 0,
         "read derived-index version");
    must(header[1] == wanted, "derived-index metadata carries expected cache version");
}

static void assert_album_artist_order(const char *first, const char *second, const char *third) {
    group_row_t groups[3];
    must(metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, 0, 3, groups) == 3,
         "read migration album-artist group page");
    must(strcmp(groups[0].name, first) == 0 && strcmp(groups[1].name, second) == 0 &&
         strcmp(groups[2].name, third) == 0,
         "album-artist group page has expected collation order");
}

/* This fixture exercises the shared display-title collation independently of
 * the legacy-index migration fixture below. Punctuation-only prefixes are
 * ignored, digits retain their own leading bucket, Latin accents and the AE/OE
 * ligatures fold with their base letters, and Cyrillic follows Latin. */
static void test_unicode_title_order(void) {
    static const struct { const char *path, *title; } rows[] = {
        { "music/u01.flac", "Ángel" },
        { "music/u02.flac", "Él" },
        { "music/u03.flac", "Étude" },
        { "music/u04.flac", "Única" },
        { "music/u05.flac", "Œsophage" },
        { "music/u06.flac", "Æther" },
        { "music/u07.flac", "Борис" },
        { "music/u08.flac", "’Til" },
        { "music/u09.flac", "...and Soon" },
        { "music/u10.flac", "2nd Movement" },
        { "music/u11.flac", "Éclair" },
        { "music/u12.flac", "Eclair" },
        { "music/Álbum fallback.flac", "" },
        { "music/u14.flac", "¡Ángel!" },
        { "music/u15.flac", "¿Bolero yo?" },
        { "music/u16.flac", "¿Por qué regresa Abril?" },
    };
    static const char *const expected[] = {
        "music/u10.flac", "music/u06.flac", "music/Álbum fallback.flac", "music/u09.flac",
        "music/u01.flac", "music/u14.flac", "music/u15.flac", "music/u11.flac",
        "music/u12.flac", "music/u02.flac", "music/u03.flac", "music/u05.flac",
        "music/u16.flac", "music/u08.flac", "music/u04.flac", "music/u07.flac",
    };
    char parent[512];
    must(getcwd(parent, sizeof(parent)) != NULL, "save fixture working directory");
    metadata_db_close();
    must(mkdir("unicode-titles", 0755) == 0 && chdir("unicode-titles") == 0,
         "create isolated Unicode title catalog");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh Unicode title catalog");
    must(mkdir("music", 0755) == 0, "create Unicode title music directory");
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) touch(rows[i].path);
    char previous_revision[METADATA_DB_CATALOG_REVISION_SIZE];
    must(metadata_db_catalog_get_revision(previous_revision, sizeof(previous_revision)),
         "revision getter works before catalog update");
    metadata_db_begin_update();
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        cached_tags_t tags = {0};
        snprintf(tags.title, sizeof(tags.title), "%s", rows[i].title);
        snprintf(tags.artist, sizeof(tags.artist), "Unicode Artist");
        snprintf(tags.album, sizeof(tags.album), "Unicode Album");
        snprintf(tags.album_artist, sizeof(tags.album_artist), "Unicode Artist");
        tags.track_number = (int)i + 1;
        tags.disc_number = -1;
        must(metadata_db_put(rows[i].path, 1234, 10, &tags), "insert Unicode title fixture");
    }
    must(metadata_db_end_update(), "commit Unicode title fixture");

    metadata_db_catalog_page_t page;
    metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(NULL, 0, (int)(sizeof(expected) / sizeof(expected[0])),
                                        &page, songs) == METADATA_DB_CATALOG_OK &&
         page.count == (int)(sizeof(expected) / sizeof(expected[0])),
         "read Unicode title order fixture");
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (strcmp(songs[i].song.path, expected[i]) != 0) {
            for (size_t j = 0; j < sizeof(expected) / sizeof(expected[0]); ++j)
                fprintf(stderr, "  unicode actual[%zu]=%s expected=%s\n", j, songs[j].song.path, expected[j]);
            must(false, "Unicode title order folds accents, ligatures and punctuation consistently");
        }
    }
    must(!songs[2].song.tags.title[0], "fallback row remains untagged");
    must(strcmp(strrchr(page.revision, ':') + 1, "4") == 0,
         "catalog revision identifies title collation version 4");
    assert_revision_getter_matches(page.revision, "revision getter works after catalog update");
    must(strcmp(previous_revision, page.revision) != 0,
         "committed catalog generation changes the revision");

    char revision[METADATA_DB_CATALOG_REVISION_SIZE];
    snprintf(revision, sizeof(revision), "%s", page.revision);
    metadata_db_catalog_song_t first[4], second[4], third[4], fourth[4];
    must(metadata_db_catalog_songs_page(revision, 0, 4, &page, first) == METADATA_DB_CATALOG_OK && page.count == 4,
         "read first Unicode title page");
    must(metadata_db_catalog_songs_page(revision, 4, 4, &page, second) == METADATA_DB_CATALOG_OK && page.count == 4,
         "read second Unicode title page");
    must(metadata_db_catalog_songs_page(revision, 8, 4, &page, third) == METADATA_DB_CATALOG_OK && page.count == 4,
         "read third Unicode title page");
    must(metadata_db_catalog_songs_page(revision, 12, 4, &page, fourth) == METADATA_DB_CATALOG_OK && page.count == 4,
         "read final Unicode title page");
    for (int i = 0; i < 4; ++i) must(strcmp(first[i].song.path, expected[i]) == 0, "first Unicode page order");
    for (int i = 0; i < 4; ++i) must(strcmp(second[i].song.path, expected[i + 4]) == 0, "second Unicode page order");
    for (int i = 0; i < 4; ++i) must(strcmp(third[i].song.path, expected[i + 8]) == 0, "third Unicode page order");
    for (int i = 0; i < 4; ++i) must(strcmp(fourth[i].song.path, expected[i + 12]) == 0, "final Unicode page order");

    metadata_db_close();
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen persisted Unicode title catalog");
    static const char *const reopened_first[] = {
        "music/u10.flac", "music/u06.flac", "music/Álbum fallback.flac", "music/u09.flac",
    };
    must(metadata_db_catalog_songs_page(NULL, 0, 4, &page, songs) == METADATA_DB_CATALOG_OK && page.count == 4,
         "read persisted Unicode title catalog after reopen");
    for (int i = 0; i < 4; ++i)
        must(strcmp(songs[i].song.path, reopened_first[i]) == 0, "reopened Unicode index retains collation order");
    metadata_db_close();
    must(chdir(parent) == 0, "restore fixture working directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen original ASCII catalog");
}

static void assert_order(const char *const expected[6], const char *revision) {
    metadata_db_catalog_page_t page;
    metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(revision, 0, 6, &page, songs) == METADATA_DB_CATALOG_OK &&
         page.count == 6, "read complete song catalog");
    for (int i = 0; i < 6; ++i) {
        if (strcmp(songs[i].song.path, expected[i]) != 0) {
            for (int j = 0; j < 6; ++j) fprintf(stderr, "  actual[%d]=%s\n", j, songs[j].song.path);
            fprintf(stderr, "catalog row %d: got %s, expected %s\n", i,
                    songs[i].song.path, expected[i]);
            must(false, "catalog follows case-insensitive displayed-title order across tagged and untagged songs");
        }
    }
    must(!songs[1].song.tags.title[0] && !songs[5].song.tags.title[0],
         "fallback rows remain untagged in catalog output");
    char current_revision[METADATA_DB_CATALOG_REVISION_SIZE];
    snprintf(current_revision, sizeof(current_revision), "%s", page.revision);
    must(metadata_db_catalog_songs_page(current_revision, 0, 3, &page, songs) == METADATA_DB_CATALOG_OK &&
         page.count == 3, "read first bounded title-order page");
    for (int i = 0; i < page.count; ++i)
        must(strcmp(songs[i].song.path, expected[i]) == 0, "first page matches full catalog order");
    metadata_db_catalog_song_t next_page[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(current_revision, 3, 3, &page, next_page) == METADATA_DB_CATALOG_OK &&
         page.count == 3, "read second bounded title-order page");
    for (int i = 0; i < page.count; ++i)
        must(strcmp(next_page[i].song.path, expected[i + 3]) == 0,
             "concatenated pages preserve full catalog title order");

}

int main(int argc, char **argv) {
    bool seed_legacy = argc == 2 && strcmp(argv[1], "--seed-legacy") == 0;
    bool verify_migration = argc == 2 && strcmp(argv[1], "--verify-migration") == 0;
    bool verify_readonly_legacy = argc == 2 && strcmp(argv[1], "--verify-readonly-legacy") == 0;
    must(argc == 1 || seed_legacy || verify_migration || verify_readonly_legacy, "valid mode");

    char fallback_path[160];
    char expected_storage[6][160];
    expected_paths(fallback_path, sizeof(fallback_path), expected_storage);
    const char *expected[6];
    for (int i = 0; i < 6; ++i) expected[i] = expected_storage[i];

    if (verify_migration) {
        must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL,
             "open catalog with legacy derived indexes");
        char old_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
        FILE *revision_file = fopen("old_revision.txt", "r");
        must(revision_file && fgets(old_revision, sizeof(old_revision), revision_file),
             "read pre-migration catalog revision");
        fclose(revision_file);
        old_revision[strcspn(old_revision, "\r\n")] = '\0';
        int32_t old_generation = 0;
        FILE *generation_file = fopen("old_generation.txt", "r");
        must(generation_file && fscanf(generation_file, "%d", &old_generation) == 1,
             "read pre-migration tagcache generation");
        fclose(generation_file);
        must(metadata_db_get_generation() == old_generation,
             "derived-index migration preserves the metadata generation");
        assert_order(expected, NULL);
        must(tagcache_title_order_version() == 4,
             "migrated catalog reports title-order revision 4");
        assert_index_version(7);
        assert_album_artist_order("Ángel", "¿Bolero", "Zed");
        metadata_db_catalog_page_t stale_page;
        metadata_db_catalog_song_t stale_rows[METADATA_DB_CATALOG_PAGE_MAX];
        must(metadata_db_catalog_songs_page(old_revision, 0, 1, &stale_page, stale_rows) ==
             METADATA_DB_CATALOG_STALE,
             "revision format change rejects pre-migration rank tokens");
        metadata_db_close();
        puts("metadata catalog title order migration: PASS");
        return 0;
    }

    if (verify_readonly_legacy) {
        must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL,
             "open read-only catalog with legacy derived indexes");
        metadata_db_catalog_page_t page;
        metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
        char catalog_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
        bool id_ready = metadata_db_try_get_catalog_library_id(catalog_id, sizeof(catalog_id));
        metadata_db_catalog_result_t read_result = metadata_db_catalog_songs_page(NULL, 0, 6, &page, songs);
        if (read_result != METADATA_DB_CATALOG_OK || page.count != 6 || tagcache_title_order_version() != 3)
            fprintf(stderr, "readonly state: result=%d count=%d title_version=%d\n",
                    read_result, page.count, tagcache_title_order_version());
        if (read_result != METADATA_DB_CATALOG_OK)
            fprintf(stderr, "readonly preflight: storage=%d id_ready=%d outcome=%d\n",
                    metadata_db_storage_current(), id_ready, metadata_db_get_load_outcome());
        must(read_result == METADATA_DB_CATALOG_OK && page.count == 6 && tagcache_title_order_version() == 3,
             "read-only fallback retains old ordering and reports revision format 3");
        must(strcmp(songs[0].song.path, expected[1]) == 0 &&
             strcmp(songs[1].song.path, expected[0]) == 0,
             "read-only fixture keeps deliberately permuted legacy title ranks");
        assert_album_artist_order("Ángel", "Zed", "¿Bolero");
        int64_t old_bolero_rank = metadata_db_get_group_offset(
            METADATA_DB_GROUP_ALBUM_ARTIST, "¿Bolero", NULL);
        group_row_t old_bolero;
        must(old_bolero_rank == 2 &&
             metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST,
                                         (int)old_bolero_rank, 1, &old_bolero) == 1 &&
             strcmp(old_bolero.name, "¿Bolero") == 0,
             "version 6 lookup retains punctuation-prefixed group identity and rank");
        const char *version = strrchr(page.revision, ':');
        must(version && strcmp(version, ":3") == 0,
             "read-only fallback revision identifies its retained ordering format");
        metadata_db_close();
        puts("metadata catalog read-only legacy index: PASS");
        return 0;
    }

    if (seed_legacy) {
        must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh legacy fixture catalog");
        create_fixture(fallback_path);
        metadata_db_catalog_page_t page;
        metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
        must(metadata_db_catalog_songs_page(NULL, 0, 6, &page, songs) == METADATA_DB_CATALOG_OK &&
             page.count == 6 && tagcache_title_order_version() == 4,
             "current index exposes title-order revision 4 before legacy conversion");
        const char *version = strrchr(page.revision, ':');
        must(version && strcmp(version, ":4") == 0,
             "current catalog revision identifies title-order format 4");
        char legacy_revision[METADATA_DB_CATALOG_REVISION_SIZE];
        snprintf(legacy_revision, sizeof(legacy_revision), "%.*s:3", (int)(version - page.revision), page.revision);
        FILE *revision_file = fopen("old_revision.txt", "w");
        must(revision_file && fprintf(revision_file, "%s\n", legacy_revision) > 0 &&
             fclose(revision_file) == 0, "save pre-migration revision token");
        FILE *generation_file = fopen("old_generation.txt", "w");
        must(generation_file && fprintf(generation_file, "%d\n", metadata_db_get_generation()) > 0 &&
             fclose(generation_file) == 0, "save pre-migration tagcache generation");
        metadata_db_close();
        puts("metadata catalog legacy index fixture: PASS");
        return 0;
    }

    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh catalog");
    char empty_revision[METADATA_DB_CATALOG_REVISION_SIZE] = "stale-output";
    must(metadata_db_catalog_get_revision(empty_revision, sizeof(empty_revision)),
         "an available empty catalog has a revision");
    must(empty_revision[0] != '\0', "empty catalog revision is nonempty");
    assert_revision_getter_matches(empty_revision, "empty catalog revision is stable");
    char too_small[1] = {'x'};
    must(!metadata_db_catalog_get_revision(too_small, sizeof(too_small)) && too_small[0] == '\0',
         "undersized revision buffer fails without returning a truncated token");
    create_fixture(fallback_path);
    metadata_db_catalog_page_t initial_page;
    metadata_db_catalog_song_t initial_songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(NULL, 0, 1, &initial_page, initial_songs) == METADATA_DB_CATALOG_OK,
         "read initial populated revision");
    assert_revision_getter_matches(initial_page.revision, "getter agrees with the populated catalog page");
    assert_order(expected, NULL);
    metadata_db_close();
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen persisted catalog");
    assert_order(expected, NULL);
    test_unicode_title_order();
    metadata_db_close();
    char unavailable_revision[METADATA_DB_CATALOG_REVISION_SIZE] = "stale-output";
    must(!metadata_db_catalog_get_revision(unavailable_revision, sizeof(unavailable_revision)) &&
         unavailable_revision[0] == '\0', "closed catalog revision is unavailable and clears output");
    puts("metadata catalog title order: PASS");
    return 0;
}
