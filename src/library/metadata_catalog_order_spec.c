#define _GNU_SOURCE
#include "metadata_db.h"
#include "tagcache.h"

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

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    must(fd >= 0 && close(fd) == 0, "create fixture song file");
}

static void put_song(const char *path, const char *title) {
    cached_tags_t tags = {0};
    snprintf(tags.title, sizeof(tags.title), "%s", title);
    snprintf(tags.artist, sizeof(tags.artist), "Artist");
    snprintf(tags.album, sizeof(tags.album), "Album");
    snprintf(tags.album_artist, sizeof(tags.album_artist), "Artist");
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
    put_song("music/tagged-alpha.flac", "Alpha");
    put_song("music/aLpHa.flac", "");
    put_song("music/tagged-bravo.flac", "BRAVO");
    put_song("music/tagged-charlie.flac", "charlie");
    put_song("music/tagged-shared.flac", shared_tagged_title);
    put_song(shared_fallback_path, "");
    must(metadata_db_end_update(), "commit fixture songs");
}

static void assert_order(const char *const expected[6], const char *revision) {
    metadata_db_catalog_page_t page;
    metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(revision, 0, 6, &page, songs) == METADATA_DB_CATALOG_OK &&
         page.count == 6, "read complete song catalog");
    for (int i = 0; i < 6; ++i)
        must(strcmp(songs[i].song.path, expected[i]) == 0,
             "catalog follows case-insensitive displayed-title order across tagged and untagged songs");
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
        must(metadata_db_catalog_songs_page(NULL, 0, 6, &page, songs) == METADATA_DB_CATALOG_OK &&
             page.count == 6, "read valid catalog after read-only migration failure");
        must(strcmp(songs[0].song.path, expected[0]) != 0 &&
             tagcache_title_order_version() == 1,
             "read-only fallback retains old ordering and reports revision format 1");
        const char *version = strrchr(page.revision, ':');
        must(version && strcmp(version, ":1") == 0,
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
             page.count == 6 && strcmp(songs[0].song.path, expected[0]) != 0,
             "legacy index demonstrates the old empty-title ordering");
        FILE *revision_file = fopen("old_revision.txt", "w");
        must(revision_file && fprintf(revision_file, "%s\n", page.revision) > 0 &&
             fclose(revision_file) == 0, "save pre-migration revision token");
        FILE *generation_file = fopen("old_generation.txt", "w");
        must(generation_file && fprintf(generation_file, "%d\n", metadata_db_get_generation()) > 0 &&
             fclose(generation_file) == 0, "save pre-migration tagcache generation");
        metadata_db_close();
        puts("metadata catalog legacy index fixture: PASS");
        return 0;
    }

    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh catalog");
    create_fixture(fallback_path);
    assert_order(expected, NULL);
    metadata_db_close();
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen persisted catalog");
    assert_order(expected, NULL);
    metadata_db_close();
    puts("metadata catalog title order: PASS");
    return 0;
}
