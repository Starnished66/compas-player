#define _GNU_SOURCE
#include "playlist_files.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool accept_check(void * context, char * const * paths, int count) {
    (void) paths;
    int * expected = context;
    return count == *expected;
}

static bool refuse_check(void * context, char * const * paths, int count) {
    (void) context; (void) paths; (void) count;
    return false;
}

static int fail_rename;
int __real_rename(const char * old_path, const char * new_path);
int __wrap_rename(const char * old_path, const char * new_path) {
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(old_path, new_path);
}

static void write_text(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    assert(f);
    assert(fputs(value, f) >= 0);
    assert(fclose(f) == 0);
}

static char * read_text(const char * path) {
    FILE * f = fopen(path, "r");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
    char * value = malloc((size_t) n + 1);
    assert(value && fread(value, 1, (size_t) n, f) == (size_t) n);
    value[n] = '\0';
    fclose(f);
    return value;
}

int main(void) {
    char dir[] = ".playlist-bulk-delete-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/list.m3u", dir);
    const char * original = "#EXTM3U\n#EXTINF:1,first\n/a\n#EXTINF:2,duplicate one\n/b\n#EXTINF:3,duplicate two\n/b\n/c\n";
    write_text(path, original);
    int count = 4;
    const int one_duplicate[] = { 1 };
    assert(playlist_files_delete_entries_checked(path, one_duplicate, 1, accept_check, &count) == PLAYLIST_EDIT_OK);
    char * probe = read_text(path);
    assert(strcmp(probe, "#EXTM3U\n#EXTINF:1,first\n/a\n#EXTINF:3,duplicate two\n/b\n/c\n") == 0);
    free(probe);
    write_text(path, original);
    const int noncontiguous[] = { 0, 2 };
    assert(playlist_files_delete_entries_checked(path, noncontiguous, 2, accept_check, &count) == PLAYLIST_EDIT_OK);
    probe = read_text(path);
    assert(strcmp(probe, "#EXTM3U\n#EXTINF:2,duplicate one\n/b\n/c\n") == 0);
    free(probe);
    write_text(path, original);
    const int selected[] = { 1, 2 };
    assert(playlist_files_delete_entries_checked(path, selected, 2, accept_check, &count) == PLAYLIST_EDIT_OK);
    char * actual = read_text(path);
    assert(strcmp(actual, "#EXTM3U\n#EXTINF:1,first\n/a\n/c\n") == 0);
    free(actual);

    const int none_count = 2;
    assert(playlist_files_delete_entries_checked(path, NULL, 0, accept_check, (void *) &none_count) == PLAYLIST_EDIT_OK);
    actual = read_text(path);
    assert(strcmp(actual, "#EXTM3U\n#EXTINF:1,first\n/a\n/c\n") == 0);
    free(actual);

    const int bad_order[] = { 1, 0 };
    assert(playlist_files_delete_entries_checked(path, bad_order, 2, accept_check, (void *) &none_count) == PLAYLIST_EDIT_FAILED);
    const int out_of_range[] = { 2 };
    assert(playlist_files_delete_entries_checked(path, out_of_range, 1, accept_check, (void *) &none_count) == PLAYLIST_EDIT_FAILED);
    assert(playlist_files_delete_entries_checked(path, selected, 2, refuse_check, NULL) == PLAYLIST_EDIT_REFUSED);

    const int now_count = 2;
    const int all[] = { 0, 1 };
    assert(playlist_files_delete_entries_checked(path, all, 2, accept_check, (void *) &now_count) == PLAYLIST_EDIT_OK);
    actual = read_text(path);
    assert(strcmp(actual, "#EXTM3U\n") == 0);
    free(actual);

    write_text(path, original);
    fail_rename = 1;
    const int first[] = { 0 };
    assert(playlist_files_delete_entries_checked(path, first, 1, accept_check, &count) == PLAYLIST_EDIT_FAILED);
    fail_rename = 0;
    actual = read_text(path);
    assert(strcmp(actual, original) == 0);
    free(actual);

    unlink(path);
    rmdir(dir);
    return 0;
}
