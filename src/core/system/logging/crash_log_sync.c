#include "crash_log_sync.h"
#include "db_log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRASH_LOG_SYNC_MAX_BYTES (256 * 1024)

static const char * const crash_log_names[] = {
    "player_exit.log", "player_exit.log.1", "player_crash.log", "player_crash.log.1",
};

static bool crash_log_up_to_date(const struct stat * src, const char * dst_path) {
    struct stat dst;
    /* FAT stores mtimes at 2 s granularity. */
    return stat(dst_path, &dst) == 0 && dst.st_size == src->st_size &&
           llabs((long long) dst.st_mtime - (long long) src->st_mtime) <= 2;
}

static void crash_log_copy(const char * src_path, const char * dst_path, const struct stat * src) {
    int in = open(src_path, O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    int out = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(in, buf, sizeof(buf))) > 0 && write(out, buf, (size_t) n) == n) {}
        close(out);
        struct timespec times[2] = {src->st_mtim, src->st_mtim};
        (void) utimensat(AT_FDCWD, dst_path, times, 0);
    }
    close(in);
}

void crash_log_sync_to_sd(void) {
    bool dir_ready = false;
    for (size_t i = 0; i < sizeof(crash_log_names) / sizeof(crash_log_names[0]); i++) {
        char src_path[96], dst_path[96];
        snprintf(src_path, sizeof(src_path), "/usr/data/%s", crash_log_names[i]);
        snprintf(dst_path, sizeof(dst_path), DB_LOG_DIR "/%s", crash_log_names[i]);
        struct stat src;
        if (stat(src_path, &src) != 0 || !S_ISREG(src.st_mode) || src.st_size > CRASH_LOG_SYNC_MAX_BYTES) continue;
        if (!dir_ready) {
            (void) mkdir(DB_LOG_DIR, 0755);
            dir_ready = true;
        }
        if (!crash_log_up_to_date(&src, dst_path)) crash_log_copy(src_path, dst_path, &src);
    }
}
