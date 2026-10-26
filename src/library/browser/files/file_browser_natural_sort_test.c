/* Include the implementation so this host test can exercise its private
 * comparator without linking or constructing the UI. */
#include "file_browser.c"

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

static void expect_name(const dir_entry_t *rows, size_t rank, const char *name) {
    assert(strcmp(rows[rank].name, name) == 0);
}

static void create_entry(const char *root, const char *name, bool is_dir, time_t mtime) {
    char path[4096];
    assert(snprintf(path, sizeof(path), "%s/%s", root, name) < (int)sizeof(path));
    if (is_dir) assert(mkdir(path, 0700) == 0);
    else {
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0 && close(fd) == 0);
    }
    struct utimbuf times = {.actime = mtime, .modtime = mtime};
    assert(utime(path, &times) == 0);
}

static void expect_index_name(file_browser_index_t *index, unsigned rank,
                              const char *expected, bool is_dir) {
    char name[256]; bool got_dir = !is_dir;
    assert(file_browser_index_entry_name(index, rank, name, sizeof(name), &got_dir));
    assert(got_dir == is_dir && strcmp(name, expected) == 0);
}

int main(void) {
    dir_entry_t rows[] = {
        {.name = "item10.mp3"},
        {.name = "item2.mp3"},
        {.name = "item02.mp3"},
        {.name = "item0002.mp3"},
        {.name = "Item2.mp3"},
        {.name = "item1.mp3"},
        {.name = "item999999999999999999999999999999999999.mp3"},
        {.name = "item1000000000000000000000000000000000000.mp3"},
        {.name = "item2.mp3", .is_dir = true},
        {.name = "item10.mp3", .is_dir = true},
        {.name = "z.mp3"},
        {.name = "\303\251.mp3"}, /* Non-ASCII UTF-8 bytes retain byte order. */
    };
    const size_t row_count = sizeof(rows) / sizeof(rows[0]);
    qsort(rows, row_count, sizeof(rows[0]), compare_entries_name);
    expect_name(rows, 0, "item2.mp3");
    assert(rows[0].is_dir);
    expect_name(rows, 1, "item10.mp3");
    assert(rows[1].is_dir);
    expect_name(rows, 2, "item1.mp3");
    expect_name(rows, 3, "Item2.mp3");
    expect_name(rows, 4, "item0002.mp3");
    expect_name(rows, 5, "item02.mp3");
    expect_name(rows, 6, "item2.mp3");
    expect_name(rows, 7, "item10.mp3");
    expect_name(rows, 8, "item999999999999999999999999999999999999.mp3");
    expect_name(rows, 9, "item1000000000000000000000000000000000000.mp3");
    expect_name(rows, 10, "z.mp3");
    expect_name(rows, 11, "\303\251.mp3");

    /* Newest timestamp remains primary; ties still use natural names. */
    dir_entry_t newest[] = {
        {.name = "entry10", .mtime_known = true, .mtime = 200},
        {.name = "entry2", .mtime_known = true, .mtime = 200},
        {.name = "entry1", .mtime_known = true, .mtime = 100},
        {.name = "folder2", .is_dir = true, .mtime_known = true, .mtime = 1},
        {.name = "entry20", .mtime_known = false},
        {.name = "entry3", .mtime_known = false},
    };
    qsort(newest, sizeof(newest) / sizeof(newest[0]), sizeof(newest[0]), compare_entries_newest);
    expect_name(newest, 0, "folder2");
    expect_name(newest, 1, "entry2");
    expect_name(newest, 2, "entry10");
    expect_name(newest, 3, "entry1");
    expect_name(newest, 4, "entry3");
    expect_name(newest, 5, "entry20");

    /* Every distinct case or padding spelling has a stable, nonzero order. */
    assert(compare_filename_natural("name2", "name02") == 0);
    assert(compare_filename_natural("disc2track3", "disc02track11") < 0);
    assert(compare_filename_natural("zero000", "zero0") == 0);
    assert(strcmp("name2", "name02") != 0);
    assert(compare_entries_mode(&(dir_entry_t){.name = "name2"},
                                 &(dir_entry_t){.name = "name02"},
                                 FILE_BROWSER_SORT_NAME) != 0);

    /* Exercise the real browser index in external-run and memory-fallback
     * builds. The runner compiles this source with a tiny run size. */
    char root[] = "/tmp/file-browser-natural-index-XXXXXX";
    assert(mkdtemp(root));
    create_entry(root, "dir10", true, 200);
    create_entry(root, "dir2", true, 200);
    const char *names[] = {
        "item10.mp3", "item2.mp3", "item02.mp3", "item0002.mp3",
        "Item3.mp3", "item1.mp3",
        "item999999999999999999999999999999999999.mp3",
        "item1000000000000000000000000000000000000.mp3",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        create_entry(root, names[i], false, 200);
    atomic_store_explicit(&current_sort_mode, FILE_BROWSER_SORT_NAME, memory_order_release);
    file_browser_index_t *index = NULL; unsigned count = 0;
    assert(file_browser_index_open(root, &index, &count) && count == 10);
    expect_index_name(index, 0, "dir2", true);
    expect_index_name(index, 1, "dir10", true);
    expect_index_name(index, 2, "item1.mp3", false);
    expect_index_name(index, 3, "item0002.mp3", false);
    expect_index_name(index, 4, "item02.mp3", false);
    expect_index_name(index, 5, "item2.mp3", false);
    expect_index_name(index, 6, "Item3.mp3", false);
    expect_index_name(index, 7, "item10.mp3", false);
    expect_index_name(index, 8, "item999999999999999999999999999999999999.mp3", false);
    expect_index_name(index, 9, "item1000000000000000000000000000000000000.mp3", false);
    file_browser_index_close(index);

    /* Newest is primary; equal timestamps fall back to natural name order. */
    atomic_store_explicit(&current_sort_mode, FILE_BROWSER_SORT_NEWEST, memory_order_release);
    assert(file_browser_index_open(root, &index, &count) && count == 10);
    expect_index_name(index, 0, "dir2", true);
    expect_index_name(index, 1, "dir10", true);
    expect_index_name(index, 2, "item1.mp3", false);
    expect_index_name(index, 7, "item10.mp3", false);
    file_browser_index_close(index);
    atomic_store_explicit(&current_sort_mode, FILE_BROWSER_SORT_NAME, memory_order_release);

    char picked[4096];
    assert(snprintf(picked, sizeof(picked), "%s/item10.mp3", root) < (int)sizeof(picked));
    assert(setenv("FILE_BROWSER_TEST_ENTRY_LIMIT", "1", 1) == 0);
    char **playlist = NULL; int playlist_count = 0, selected = -1;
    assert(!file_browser_build_playlist_for_path(picked, &playlist, &playlist_count, &selected));
    assert(setenv("FILE_BROWSER_TEST_ENTRY_LIMIT", "100", 1) == 0);
    assert(file_browser_build_playlist_for_path(picked, &playlist, &playlist_count, &selected));
    assert(playlist_count == 8 && selected == 5);
    for (int i = 0; i < playlist_count; i++) free(playlist[i]);
    free(playlist);
    assert(unsetenv("FILE_BROWSER_TEST_ENTRY_LIMIT") == 0);
    unsigned playable = 0, lazy_selected = 99;
    assert(file_browser_open_lazy_directory(picked, &index, &playable, &lazy_selected));
    assert(playable == 8 && lazy_selected == 5);
    char selected_path[4096];
    assert(file_browser_index_playable_path_at(index, lazy_selected, selected_path, sizeof(selected_path)));
    assert(strcmp(selected_path, picked) == 0);
    file_browser_index_close(index);

    char page_root[] = "/tmp/file-browser-natural-page-XXXXXX";
    assert(mkdtemp(page_root));
    for (unsigned i = 1; i <= 70; i++) {
        char page_name[64];
        assert(snprintf(page_name, sizeof(page_name), "page-%u.mp3", i) < (int)sizeof(page_name));
        create_entry(page_root, page_name, false, 100);
    }
    assert(file_browser_index_open(page_root, &index, &count) && count == 70);
    expect_index_name(index, 64, "page-65.mp3", false);
    assert(file_browser_index_path_at(index, 64, selected_path, sizeof(selected_path)));
    assert(strstr(selected_path, "/page-65.mp3") != NULL);
    file_browser_index_close(index);
    char cleanup[9000];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf -- '%s' '%s'", root, page_root) < (int)sizeof(cleanup));
    assert(system(cleanup) == 0);
    return 0;
}
