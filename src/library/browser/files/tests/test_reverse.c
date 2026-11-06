#include <assert.h>
#include "../file_browser.c"

static void expect_name(file_browser_index_t *index, unsigned row, const char *expected) {
    char name[256];
    bool is_dir;
    assert(file_browser_index_entry_name(index, row, name, sizeof(name), &is_dir));
    assert(strcmp(name, expected) == 0);
}

int main(void) {
    dir_entry_t rows[] = {
        {.name = "A", .is_dir = true, .playable_ordinal = -1},
        {.name = "Z", .is_dir = true, .playable_ordinal = -1},
        {.name = "1.mp3", .playable_ordinal = 0},
        {.name = "2.m3u", .is_playlist = true, .playable_ordinal = -1},
        {.name = "3.mp3", .playable_ordinal = 1},
    };
    dir_entry_t playable[] = {rows[2], rows[4]};
    file_browser_index_t index = {
        .fd = -1, .playable_fd = -1, .directory_fd = open(".", O_RDONLY),
        .memory_backed = true, .memory_rows = rows, .memory_playable_rows = playable,
        .count = 5, .directory_count = 2, .playable_count = 2, .reverse = true,
        .directory = "/music"
    };
    expect_name(&index, 0, "Z");
    expect_name(&index, 1, "A");
    expect_name(&index, 2, "3.mp3");
    expect_name(&index, 3, "2.m3u");
    expect_name(&index, 4, "1.mp3");
    dir_entry_t row;
    assert(index_entry_at(&index, 2, &row) && row.playable_ordinal == 0);
    assert(index_entry_at(&index, 4, &row) && row.playable_ordinal == 1);
    char path[256];
    assert(file_browser_index_path_at(&index, 2, path, sizeof(path)));
    assert(strcmp(path, "/music/3.mp3") == 0);
    assert(file_browser_index_playable_path_at(&index, 0, path, sizeof(path)));
    assert(strcmp(path, "/music/3.mp3") == 0);
    file_browser_index_t *retained;
    assert(file_browser_index_retain(&index, &retained));
    index.reverse = false;
    atomic_store(&current_sort_reverse, false);
    expect_name(retained, 2, "3.mp3");
    assert(file_browser_index_playable_path_at(retained, 1, path, sizeof(path)));
    assert(strcmp(path, "/music/1.mp3") == 0);
    file_browser_index_close(retained);
    char disk_name[] = "/tmp/compas-browser-reverse-XXXXXX";
    int fd = mkstemp(disk_name);
    assert(fd >= 0);
    unlink(disk_name);
    assert(write(fd, rows, sizeof(rows)) == (ssize_t)sizeof(rows));
    char playable_name[] = "/tmp/compas-playable-reverse-XXXXXX";
    int playable_fd = mkstemp(playable_name);
    assert(playable_fd >= 0);
    unlink(playable_name);
    assert(write(playable_fd, playable, sizeof(playable)) == (ssize_t)sizeof(playable));
    index.memory_backed = false;
    index.fd = fd;
    index.playable_fd = playable_fd;
    index.reverse = true;
    expect_name(&index, 2, "3.mp3");
    assert(file_browser_index_retain(&index, &retained));
    close(fd);
    close(playable_fd);
    expect_name(retained, 4, "1.mp3");
    assert(file_browser_index_playable_path_at(retained, 0, path, sizeof(path)));
    assert(strcmp(path, "/music/3.mp3") == 0);
    file_browser_index_close(retained);
    close(index.directory_fd);

    dir_entry_t modified[] = {
        {.name = "old.mp3", .mtime = 1, .mtime_known = true},
        {.name = "new.mp3", .mtime = 2, .mtime_known = true},
        {.name = "dir", .is_dir = true, .mtime = 3, .mtime_known = true},
    };
    qsort(modified, 3, sizeof(*modified), sort_comparator(FILE_BROWSER_SORT_NEWEST));
    assert(strcmp(modified[reverse_row(3, 1, 0, true)].name, "dir") == 0);
    assert(strcmp(modified[reverse_row(3, 1, 1, true)].name, "old.mp3") == 0);
    assert(strcmp(modified[reverse_row(3, 1, 2, true)].name, "new.mp3") == 0);
    for (unsigned dirs = 0; dirs <= 5; dirs++) {
        for (unsigned r = 0; r < 5; r++) {
            unsigned mapped = reverse_row(5, dirs, r, true);
            assert(mapped < 5 && (r < dirs) == (mapped < dirs));
            assert(reverse_row(5, dirs, mapped, true) == r);
            assert(reverse_row(5, dirs, r, false) == r);
        }
    }
    puts("file browser reverse mapping: PASS");
    return 0;
}
