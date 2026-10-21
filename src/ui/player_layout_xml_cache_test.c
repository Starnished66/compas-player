#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char * player_layouts_cache_test_read(const char * path);
unsigned int player_layouts_cache_test_disk_reads(void);
size_t player_layouts_cache_test_entry_count(void);
void player_layouts_cache_test_clear(void);

static void write_text(const char * path, const char * text) {
    FILE * file = fopen(path, "wb");
    assert(file);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void check_text(const char * path, const char * expected) {
    char * text = player_layouts_cache_test_read(path);
    assert(text && strcmp(text, expected) == 0);
    free(text);
}

int main(void) {
    char dir[] = "build/player_layout_xml_cache_XXXXXX";
    assert(mkdtemp(dir));
    char a[256], b[256], c[256], missing[256], oversized[256], replacement[256];
    snprintf(a, sizeof(a), "%s/a.xml", dir);
    snprintf(b, sizeof(b), "%s/b.xml", dir);
    snprintf(c, sizeof(c), "%s/c.xml", dir);
    snprintf(missing, sizeof(missing), "%s/missing.xml", dir);
    snprintf(oversized, sizeof(oversized), "%s/oversized.xml", dir);
    snprintf(replacement, sizeof(replacement), "%s/replacement.xml", dir);

    write_text(a, "alpha");
    check_text(a, "alpha");
    assert(player_layouts_cache_test_disk_reads() == 1);
    char * owned_copy = player_layouts_cache_test_read(a);
    assert(owned_copy && strcmp(owned_copy, "alpha") == 0);
    assert(player_layouts_cache_test_disk_reads() == 1);
    owned_copy[0] = 'X';
    free(owned_copy);
    check_text(a, "alpha");
    assert(player_layouts_cache_test_disk_reads() == 1);

    struct stat old_stat;
    assert(stat(a, &old_stat) == 0);
    write_text(a, "bravo"); /* Same size; force a distinct nanosecond timestamp. */
    struct timespec times[2] = { old_stat.st_atim, { old_stat.st_mtim.tv_sec + 10, 123456789 } };
    assert(utimensat(AT_FDCWD, a, times, 0) == 0);
    check_text(a, "bravo");
    assert(player_layouts_cache_test_disk_reads() == 2);

    struct stat before_replace, after_replace;
    assert(stat(a, &before_replace) == 0);
    write_text(replacement, "delta");
    assert(rename(replacement, a) == 0);
    assert(stat(a, &after_replace) == 0 && before_replace.st_ino != after_replace.st_ino);
    check_text(a, "delta");
    assert(player_layouts_cache_test_disk_reads() == 3);

    write_text(b, "bravo");
    write_text(c, "charlie");
    check_text(b, "bravo");
    check_text(c, "charlie");
    assert(player_layouts_cache_test_entry_count() == 2);
    unsigned int reads = player_layouts_cache_test_disk_reads();
    check_text(a, "delta"); /* LRU eviction forces a fresh open. */
    assert(player_layouts_cache_test_disk_reads() == reads + 1);
    assert(player_layouts_cache_test_entry_count() == 2);

    player_layouts_cache_test_clear();
    assert(player_layouts_cache_test_entry_count() == 0);
    reads = player_layouts_cache_test_disk_reads();
    check_text(a, "delta");
    assert(player_layouts_cache_test_disk_reads() == reads + 1);

    assert(player_layouts_cache_test_read(missing) == NULL);
    FILE * big = fopen(oversized, "wb");
    assert(big);
    for (size_t i = 0; i < 256 * 1024 + 1; ++i) assert(fputc('x', big) != EOF);
    assert(fclose(big) == 0);
    assert(player_layouts_cache_test_read(oversized) == NULL);

    assert(unlink(a) == 0 && unlink(b) == 0 && unlink(c) == 0 && unlink(oversized) == 0);
    assert(rmdir(dir) == 0);
    puts("player layout XML cache: passed");
    return 0;
}
