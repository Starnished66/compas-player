/* Host checks for file_ops.c: run by scripts/test_file_ops.sh. */
#define _GNU_SOURCE
#include "file_ops.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef FILE_OPS_TEST_ROOT
#define FILE_OPS_TEST_ROOT "/tmp/compas-file-ops-XXXXXX"
#endif
static char root[] = FILE_OPS_TEST_ROOT;
static int fail_next_readdir;
static int fail_next_fdatasync;
static int fail_next_rename_exdev;

struct dirent *__real_readdir(DIR *dir);
int __real_fdatasync(int fd);
int __real_rename(const char *oldpath, const char *newpath);

struct dirent *__wrap_readdir(DIR *dir) {
    if (fail_next_readdir) {
        fail_next_readdir = 0;
        errno = EIO;
        return NULL;
    }
    return __real_readdir(dir);
}
int __wrap_fdatasync(int fd) {
    if (fail_next_fdatasync) {
        fail_next_fdatasync = 0;
        errno = EIO;
        return -1;
    }
    return __real_fdatasync(fd);
}
int __wrap_rename(const char *oldpath, const char *newpath) {
    if (fail_next_rename_exdev) {
        fail_next_rename_exdev = 0;
        errno = EXDEV;
        return -1;
    }
    return __real_rename(oldpath, newpath);
}

/* file_ops.c dependencies outside this test. */
bool file_browser_is_playable_name(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcmp(ext, ".flac") == 0;
}
bool usb_mode_control_storage_write_begin(void) { return true; }
void usb_mode_control_storage_write_end(void) {}

static void write_file(const char * rel, const char * text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE * f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void write_big_file(const char * rel, size_t size) {
    char path[512], block[4096];
    memset(block, 'x', sizeof(block));
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "w");
    assert(f);
    while (size) {
        size_t n = size < sizeof(block) ? size : sizeof(block);
        assert(fwrite(block, 1, n, f) == n);
        size -= n;
    }
    assert(fclose(f) == 0);
}

static void make_dir(const char * rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    assert(mkdir(path, 0777) == 0);
}

static bool exists(const char * rel) {
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    return lstat(path, &st) == 0;
}

static bool same_directory(const char * a, const char * b) {
    char pa[512], pb[512];
    struct stat sa, sb;
    snprintf(pa, sizeof(pa), "%s/%s", root, a);
    snprintf(pb, sizeof(pb), "%s/%s", root, b);
    return stat(pa, &sa) == 0 && stat(pb, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void expect_text(const char * rel, const char * text) {
    char path[512], buf[256] = "";
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE * f = fopen(path, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    assert(strcmp(buf, text) == 0);
}

/* Runs one operation to completion and returns how many items failed. */
static unsigned run(file_op_kind_t kind, const char * a, const char * b, const char * dest) {
    char pa[512], pb[512], pd[512];
    char * paths[2] = { pa, pb };
    snprintf(pa, sizeof(pa), "%s/%s", root, a);
    if (b) snprintf(pb, sizeof(pb), "%s/%s", root, b);
    snprintf(pd, sizeof(pd), "%s/%s", root, dest ? dest : "");
    assert(file_op_start(kind, paths, b ? 2 : 1, pd));
    file_op_status_t st;
    for (;;) {
        file_op_get_status(&st);
        if (!st.running) break;
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }
    assert(st.done == (b ? 2u : 1u));
    return st.failed;
}

int main(void) {
    assert(mkdtemp(root));
    make_dir("src");
    make_dir("src/sub");
    make_dir("dst");
    write_file("src/a.txt", "alpha");
    write_file("src/.hidden", "dot");
    write_file("src/sub/b.flac", "beta");
    char a_path[512];
    snprintf(a_path, sizeof(a_path), "%s/src/a.txt", root);
    const struct timespec old_times[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    assert(utimensat(AT_FDCWD, a_path, old_times, 0) == 0);

    /* Copy, then a clash gets a numbered name; times are kept. */
    assert(run(FILE_OP_COPY, "src/a.txt", "src/.hidden", "dst") == 0);
    expect_text("dst/a.txt", "alpha");
    expect_text("dst/.hidden", "dot");
    struct stat st;
    char copied[512];
    snprintf(copied, sizeof(copied), "%s/dst/a.txt", root);
    assert(stat(copied, &st) == 0 && st.st_mtime == 1000000000);
    assert(run(FILE_OP_COPY, "src/a.txt", "src/.hidden", "dst") == 0);
    expect_text("dst/a (2).txt", "alpha");
    expect_text("dst/.hidden (2)", "dot");

    /* Folders copy recursively, never into themselves. */
    assert(run(FILE_OP_COPY, "src/sub", NULL, "dst") == 0);
    expect_text("dst/sub/b.flac", "beta");
    assert(run(FILE_OP_COPY, "src", NULL, "src/sub") == 1);
    assert(!exists("src/sub/src"));
    /* Model the distinct case-sensitive host paths that alias on FAT. This
     * makes the descendant case exercise the guard on Linux too. */
    bool src_alias = same_directory("src", "SRC");
    if (!src_alias) {
        make_dir("SRC");
        make_dir("SRC/sub");
    }
    make_dir("SRC2");
    assert(run(FILE_OP_COPY, "src", NULL, "SRC/sub") == 1);
    assert(!exists("SRC/sub/src"));
    /* Equal path aliases are rejected, while a mere prefix is allowed. */
    assert(run(FILE_OP_COPY, "src", NULL, "SRC") == 1);
    assert(!exists("SRC/src") && !exists("SRC/src (2)"));
    assert(run(FILE_OP_COPY, "src", NULL, "SRC2") == 0);
    assert(exists("SRC2/src/sub/b.flac"));
    assert(run(FILE_OP_DELETE, "SRC2/src", NULL, NULL) == 0);
    if (!src_alias) assert(run(FILE_OP_DELETE, "SRC", NULL, NULL) == 0);
    snprintf(copied, sizeof(copied), "%s/SRC2", root);
    assert(rmdir(copied) == 0);

    /* Moving into the same folder changes nothing; elsewhere it moves. */
    assert(run(FILE_OP_MOVE, "src/a.txt", NULL, "src") == 0);
    expect_text("src/a.txt", "alpha");
    assert(run(FILE_OP_MOVE, "src/a.txt", NULL, "dst") == 0);
    assert(!exists("src/a.txt"));
    expect_text("dst/a (3).txt", "alpha");
    assert(run(FILE_OP_MOVE, "src", NULL, "src/sub") == 1);

    /* Delete removes whole trees; a missing path is already gone. */
    assert(run(FILE_OP_DELETE, "dst/sub", "dst/missing", NULL) == 0);
    assert(!exists("dst/sub"));
    assert(run(FILE_OP_DELETE, "src", "dst", NULL) == 0);
    assert(!exists("src") && !exists("dst"));

    /* Directory read errors fail both copy and delete immediately. */
    make_dir("err-src");
    write_file("err-src/keep.txt", "keep");
    fail_next_readdir = 1;
    assert(run(FILE_OP_COPY, "err-src", NULL, "") == 1);
    assert(exists("err-src/keep.txt") && exists("err-src (2)"));
    fail_next_readdir = 1;
    assert(run(FILE_OP_DELETE, "err-src", NULL, NULL) == 1);
    assert(exists("err-src/keep.txt"));

    /* A sync error removes the incomplete file. During EXDEV move, it also
     * keeps the original because the copy did not complete. */
    write_big_file("sync.txt", 5 * 1024 * 1024);
    make_dir("dst");
    fail_next_fdatasync = 1;
    assert(run(FILE_OP_COPY, "sync.txt", NULL, "") == 1);
    assert(exists("sync.txt") && !exists("sync (2).txt"));
    fail_next_fdatasync = 1;
    fail_next_rename_exdev = 1;
    assert(run(FILE_OP_MOVE, "sync.txt", NULL, "dst") == 1);
    assert(exists("sync.txt") && !exists("dst/sync.txt"));

    /* Deep trees copy and delete within the worker's stack (the build sets
     * it to musl's 128 KiB default). */
    char deep[512] = "deep";
    make_dir(deep);
    for (int i = 0; i < 60; i++) {
        strcat(deep, "/d");
        make_dir(deep);
    }
    strcat(deep, "/leaf.txt");
    write_file(deep, "leaf");
    assert(run(FILE_OP_COPY, "deep", NULL, "") == 0);
    char copied_leaf[600];
    snprintf(copied_leaf, sizeof(copied_leaf), "deep (2)%s", deep + strlen("deep"));
    expect_text(copied_leaf, "leaf");
    assert(run(FILE_OP_DELETE, "deep", "deep (2)", NULL) == 0);
    assert(run(FILE_OP_DELETE, "err-src", "sync.txt", NULL) == 0);
    assert(run(FILE_OP_DELETE, "err-src (2)", NULL, NULL) == 0);
    snprintf(copied, sizeof(copied), "%s/dst", root);
    assert(rmdir(copied) == 0);

    assert(file_op_is_player_folder("/sd/.compas", "/sd"));
    assert(file_op_is_player_folder("/sd/.plugins", "/sd"));
    assert(file_op_is_player_folder("/sd/.Compas", "/sd"));
    assert(!file_op_is_player_folder("/sd/.compas/db", "/sd"));
    assert(!file_op_is_player_folder("/sd/Music/.compas", "/sd"));
    assert(!file_op_is_player_folder("/sd/.compassion", "/sd"));
    assert(!file_op_is_player_folder("/sdcard/.compas", "/sd"));

    assert(file_op_path_in_root("/sd", "/sd") && file_op_path_in_root("/sd/..hidden/a.txt", "/sd"));
    assert(file_op_path_in_root("./music/Album", "./music"));
    const char * aliases[] = { "sd", "/sd/", "/sd//", "/sd/.", "/sd/./a", "/sd/..", "/sd/a/../b", "/sdx", "//sd" };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) assert(!file_op_path_in_root(aliases[i], "/sd"));
    assert(!file_op_path_in_root("./music/", "./music") && !file_op_path_in_root("./music/../etc", "./music"));

    assert(rmdir(root) == 0);
    puts("file ops tests passed");
    return 0;
}
