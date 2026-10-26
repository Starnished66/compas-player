#define _GNU_SOURCE
#include "../../src/library/artwork/albumart/albumart.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    assert(strlen(path) < sizeof(tmp));
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; assert(mkdir(tmp, 0755) == 0 || errno == EEXIST); *p = '/'; }
    }
    assert(mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

static void put_file(const char *path) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite("x", 1, 1, f) == 1);
    assert(fclose(f) == 0);
}

static void expect_search(albumart_info_t *info, const char *size, const char *expected,
                          bool source_only) {
    char found[PATH_MAX], absolute_expected[PATH_MAX];
    if (expected[0] == '/') snprintf(absolute_expected, sizeof(absolute_expected), "%s", expected);
    else {
        char cwd[PATH_MAX];
        assert(getcwd(cwd, sizeof(cwd)));
        assert(snprintf(absolute_expected, sizeof(absolute_expected), "%s/%s", cwd, expected) < (int) sizeof(absolute_expected));
    }
    bool ok = source_only ? albumart_search_source_files(info, size, found, sizeof(found))
                          : albumart_search_files(info, size, found, sizeof(found));
    assert(ok);
    if (strcmp(found, absolute_expected) != 0)
        fprintf(stderr, "search %s (source=%d): got %s, expected %s\n", size, source_only, found, absolute_expected);
    assert(strcmp(found, absolute_expected) == 0);
}

static albumart_info_t make_info(const char *path) {
    albumart_info_t info = {0};
    if (path[0] == '/') snprintf(info.path, sizeof(info.path), "%s", path);
    else {
        char cwd[PATH_MAX];
        assert(getcwd(cwd, sizeof(cwd)));
        assert(snprintf(info.path, sizeof(info.path), "%s/%s", cwd, path) < (int) sizeof(info.path));
    }
    strcpy(info.artist, "Artist");
    strcpy(info.album, "Album");
    strcpy(info.albumartist, "Album Artist");
    return info;
}

static void lookup_priority(void) {
    mkdir_p("priority/local");
    put_file("priority/local/track.mp3");
    put_file("priority/local/track.jpg");
    put_file("priority/local/cover.jpg");
    albumart_info_t info = make_info("priority/local/track.mp3");
    expect_search(&info, "", "priority/local/cover.jpg", false);
    expect_search(&info, "", "priority/local/cover.jpg", true);

    unlink("priority/local/cover.jpg");
    put_file("priority/local/Album.png");
    expect_search(&info, "", "priority/local/Album.png", false);
    unlink("priority/local/Album.png");
    put_file("priority/local/folder.jpg");
    expect_search(&info, "", "priority/local/folder.jpg", false);

    mkdir_p("fallback/parent/Disc2");
    put_file("fallback/parent/Disc2/song.mp3");
    put_file("fallback/parent/Disc2/song.jpg");
    put_file("fallback/parent/cover.png");
    info = make_info("fallback/parent/Disc2/song.mp3");
    expect_search(&info, "", "fallback/parent/Disc2/song.jpg", false);
    mkdir_p("fallback/parent/Disc1");
    put_file("fallback/parent/Disc1/Album.jpg");
    expect_search(&info, "", "fallback/parent/Disc2/song.jpg", false);
    unlink("fallback/parent/Disc2/song.jpg");
    expect_search(&info, "", "fallback/parent/cover.png", false);
    unlink("fallback/parent/cover.png");
    expect_search(&info, "", "fallback/parent/Disc1/Album.jpg", false);

    mkdir_p("parent-fallback/AlbumDir");
    put_file("parent-fallback/AlbumDir/song.mp3");
    put_file("parent-fallback/Album.jpg");
    info = make_info("parent-fallback/AlbumDir/song.mp3");
    expect_search(&info, "", "parent-fallback/Album.jpg", false);

    mkdir_p("sized");
    put_file("sized/t.mp3");
    put_file("sized/t.72x72.jpg");
    put_file("sized/cover.72x72.png");
    info = make_info("sized/t.mp3");
    expect_search(&info, ".72x72", "sized/cover.72x72.png", false);
    expect_search(&info, ":.72x72", "sized/cover.72x72.png", false);
    unlink("sized/cover.72x72.png");
    expect_search(&info, ":.72x72", "sized/t.72x72.jpg", false);
    expect_search(&info, ".72x72", "sized/t.72x72.jpg", true);
}

static void set_bmp_reserved_mtime(const char *path, uint32_t value) {
    FILE *f = fopen(path, "r+b");
    assert(f);
    unsigned char bytes[4] = { value, value >> 8, value >> 16, value >> 24 };
    assert(fseek(f, 6, SEEK_SET) == 0);
    assert(fwrite(bytes, 1, 4, f) == 4);
    assert(fclose(f) == 0);
}

static void set_file_mtime(const char *path, time_t value) {
    struct timespec ts[2] = { { .tv_sec = value }, { .tv_sec = value } };
    assert(utimensat(AT_FDCWD, path, ts, 0) == 0);
}

static void cache_fixture_path(char *path, size_t size, const char *prefix,
                               uint64_t key, int width, int height) {
    int n = snprintf(path, size, ".compas/albumart/%s%016llx.%dx%d.bmp", prefix,
                     (unsigned long long) key, width, height);
    assert(n > 0 && (size_t) n < size);
}

static void cache_version_and_cleanup(void) {
    mkdir_p("cache");
    mkdir_p(".compas/albumart");
    put_file("cache/a.mp3");
    set_file_mtime("cache/a.mp3", 1000);
    put_file("cache/cover.jpg");
    set_file_mtime("cache/cover.jpg", 1500);
    albumart_info_t info = make_info("cache/a.mp3");

    /* The first store retires every strictly named old album cache. Its scan
     * must leave current caches, artist caches, malformed names and symlinks
     * alone. Establish these before the first cache write in the process. */
    char first_old[PATH_MAX], second_old[PATH_MAX], existing_v4[PATH_MAX];
    char existing_artist[PATH_MAX], malformed[PATH_MAX], symlink_path[PATH_MAX];
    cache_fixture_path(first_old, sizeof(first_old), "v3-", albumart_debug_thumbnail_key(&info), 72, 72);
    cache_fixture_path(second_old, sizeof(second_old), "v3-", UINT64_C(0x0123456789abcdef), 480, 480);
    cache_fixture_path(existing_v4, sizeof(existing_v4), "v4-", UINT64_C(0xfedcba9876543210), 72, 72);
    snprintf(existing_artist, sizeof(existing_artist), ".compas/albumart/artist-v1-%016llx.72x72.bmp",
             (unsigned long long) albumart_artist_thumbnail_key("Other Artist"));
    snprintf(malformed, sizeof(malformed), ".compas/albumart/v3-not-a-cache.bmp");
    snprintf(symlink_path, sizeof(symlink_path), ".compas/albumart/v3-1123456789abcdef.72x72.bmp");
    put_file(first_old); put_file(second_old); put_file(existing_v4);
    put_file(existing_artist); put_file(malformed);
    assert(symlink("../../cache/a.mp3", symlink_path) == 0);

    uint16_t *pixels = calloc(480 * 480, sizeof(*pixels));
    assert(pixels);
    assert(albumart_store_rgb565(&info, 72, 72, pixels));
    assert(access(first_old, F_OK) != 0 && access(second_old, F_OK) != 0);
    assert(access(existing_v4, F_OK) == 0 && access(existing_artist, F_OK) == 0);
    assert(access(malformed, F_OK) == 0 && access(symlink_path, F_OK) == 0);
    assert(albumart_store_rgb565(&info, 480, 480, pixels));
    char v4_72[PATH_MAX], v4_480[PATH_MAX];
    assert(albumart_generated_cache_find(&info, 72, 72, v4_72, sizeof(v4_72)));
    assert(albumart_generated_cache_find(&info, 480, 480, v4_480, sizeof(v4_480)));
    assert(strstr(v4_72, "/v4-") != NULL);
    assert(strstr(v4_480, "/v4-") != NULL);

    char v3_72[PATH_MAX], v3_480[PATH_MAX], expected[PATH_MAX];
    snprintf(v3_72, sizeof(v3_72), "%s", v4_72);
    snprintf(v3_480, sizeof(v3_480), "%s", v4_480);
    strrchr(v3_72, '/')[1] = 'v';
    strrchr(v3_72, '/')[2] = '3';
    strrchr(v3_480, '/')[1] = 'v';
    strrchr(v3_480, '/')[2] = '3';
    assert(rename(v4_72, v3_72) == 0);
    assert(rename(v4_480, v3_480) == 0);
    assert(albumart_is_generated_cache_file(v3_72));
    set_bmp_reserved_mtime(v3_72, 2000);
    set_bmp_reserved_mtime(v3_480, 2000);
    assert(!albumart_generated_cache_find(&info, 72, 72, expected, sizeof(expected)));
    assert(!albumart_generated_cache_fresh(&info, 72, 72, expected, sizeof(expected)));
    assert(!albumart_generated_cache_fresh(&info, 480, 480, expected, sizeof(expected)));
    assert(!albumart_sized_thumb_fresh_with_source_mtime(&info, 72, 72, 1500, expected, sizeof(expected)));
    /* Equal source timestamps do not prove which image the old cache used. */
    set_file_mtime("cache/cover.jpg", 2000);
    assert(!albumart_generated_cache_fresh(&info, 480, 480, expected, sizeof(expected)));
    assert(!albumart_sized_thumb_fresh_with_source_mtime(&info, 72, 72, 2000, expected, sizeof(expected)));
    set_file_mtime("cache/cover.jpg", 1500);
    assert(albumart_store_rgb565(&info, 72, 72, pixels));
    assert(albumart_store_rgb565(&info, 480, 480, pixels));
    assert(albumart_generated_cache_find(&info, 72, 72, expected, sizeof(expected)));
    assert(strstr(expected, "/v4-") != NULL);
    assert(albumart_generated_cache_fresh(&info, 72, 72, expected, sizeof(expected)));
    assert(albumart_generated_cache_fresh(&info, 480, 480, expected, sizeof(expected)));
    assert(albumart_sized_thumb_fresh_with_source_mtime(&info, 72, 72, 1000, expected, sizeof(expected)));
    unlink("cache/cover.jpg");

    albumart_info_t older = info;
    put_file("cache/b.mp3");
    set_file_mtime("cache/b.mp3", 900);
    char cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)));
    assert(snprintf(older.path, sizeof(older.path), "%s/cache/b.mp3", cwd) < (int) sizeof(older.path));
    assert(albumart_sized_thumb_fresh(&older, 72, 72, expected, sizeof(expected)));
    set_file_mtime("cache/b.mp3", 1500);
    assert(albumart_sized_thumb_fresh(&older, 72, 72, expected, sizeof(expected)));

    char user_file[PATH_MAX];
    snprintf(user_file, sizeof(user_file), "%s/user-cover.jpg", ".compas/albumart");
    put_file(user_file);

    assert(albumart_delete_generated_cache_for_album(&info, NULL, 0));
    assert(access(v3_72, F_OK) != 0 && access(v3_480, F_OK) != 0);
    assert(access(v4_72, F_OK) != 0 && access(v4_480, F_OK) != 0);
    assert(access(malformed, F_OK) == 0 && access(symlink_path, F_OK) == 0 && access(user_file, F_OK) == 0);

    assert(albumart_store_rgb565(&info, 72, 72, pixels));
    assert(albumart_generated_cache_find(&info, 72, 72, expected, sizeof(expected)));
    snprintf(v3_72, sizeof(v3_72), "%s", expected);
    strrchr(v3_72, '/')[1] = 'v';
    strrchr(v3_72, '/')[2] = '3';
    assert(rename(expected, v3_72) == 0);
    assert(albumart_store_rgb565(&info, 72, 72, pixels));
    assert(access(v3_72, F_OK) == 0); /* migration is once per process */
    assert(albumart_delete_all_generated_caches());
    assert(!albumart_generated_cache_find(&info, 72, 72, expected, sizeof(expected)));
    assert(access(v3_72, F_OK) != 0);
    assert(access(malformed, F_OK) == 0 && access(symlink_path, F_OK) == 0 && access(user_file, F_OK) == 0);
    free(pixels);
}

int main(void) {
    lookup_priority();
    cache_version_and_cleanup();
    puts("albumart priority and cache version spec: PASS");
    return 0;
}
