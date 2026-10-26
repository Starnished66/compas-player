/* Applies the canonical SD player update and selects the installed or
 * packaged executable. The player also provides explicit update actions. */
#define _POSIX_C_SOURCE 200809L
#include "installer.h"
#include "../scan/scanner.h"
#include "../../core/updates/player_install.h"
#include "../display/fb_draw.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef COMPAS_LOADER_BUILD
/* Keep a rejected SD update for inspection without retrying it every boot. */
static void quarantine_rejected_update(const char *path, const struct stat *snapshot) {
    char parent[512], leaf[64];
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1] || (size_t)(slash - path) >= sizeof(parent) ||
        strlen(slash + 1) >= sizeof(leaf)) return;
    size_t length = (size_t)(slash - path);
    memcpy(parent, path, length);
    parent[length] = '\0';
    snprintf(leaf, sizeof(leaf), "%s", slash + 1);
    int dir = open(length ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return;
    struct stat current;
    if (fstatat(dir, leaf, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(current.st_mode) || current.st_dev != snapshot->st_dev ||
        current.st_ino != snapshot->st_ino || current.st_size != snapshot->st_size ||
        current.st_mtim.tv_sec != snapshot->st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != snapshot->st_mtim.tv_nsec ||
        current.st_ctim.tv_sec != snapshot->st_ctim.tv_sec ||
        current.st_ctim.tv_nsec != snapshot->st_ctim.tv_nsec) {
        close(dir);
        return;
    }
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        char rejected[96];
        if (attempt) snprintf(rejected, sizeof(rejected), "%s.rejected.%u", leaf, attempt);
        else snprintf(rejected, sizeof(rejected), "%s.rejected", leaf);
        struct stat existing;
        if (fstatat(dir, rejected, &existing, AT_SYMLINK_NOFOLLOW) == 0) continue;
        if (errno != ENOENT) break;
        if (renameat(dir, leaf, dir, rejected) == 0) {
            (void)fsync(dir);
            fprintf(stderr, "compas_loader: rejected update saved as %s\n", rejected);
        }
        break;
    }
    close(dir);
}
void installer_run(const scan_result_t *scan, bool fb_ready) {
    if (!scan || !scan->sd_update_present) return;
    const char *path = scan->sd_update_path ? scan->sd_update_path : SD_UPDATE_PLAYER_PATH;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat snapshot;
    char error[160] = {0};
    bool ok = fstat(fd, &snapshot) == 0 && S_ISREG(snapshot.st_mode) &&
              snapshot.st_size > 0 && snapshot.st_size <= PLAYER_INSTALL_MAX_BYTES;
    if (ok && !player_install_preflight_fd(fd, false, error, sizeof(error))) {
        quarantine_rejected_update(path, &snapshot);
        fprintf(stderr, "compas_loader: rejected SD player update: %s\n", error);
        close(fd);
        return;
    }
    if (ok && fb_ready) {
        fb_draw_installer_warning();
        fb_flush();
    }
    if (ok) ok = player_install_fd(fd, (uint64_t)snapshot.st_size, NULL, false, true,
                                   path, &snapshot, error, sizeof(error));
    close(fd);
    if (!ok) fprintf(stderr, "compas_loader: SD player update failed: %s\n", error[0] ? error : "Could not read update");
    else fprintf(stderr, "compas_loader: installed SD player update\n");
}
#endif

static bool fsync_path(const char * path, bool is_dir) {
    int fd = open(path, O_RDONLY | (is_dir ? O_DIRECTORY : 0));
    if (fd < 0) {
        fprintf(stderr, "installer: open(%s) for fsync failed: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fsync(fd) == 0;
    if (!ok) fprintf(stderr, "installer: fsync(%s) failed: %s\n", path, strerror(errno));
    if (close(fd) != 0) ok = false;
    return ok;
}

static void fsync_parent_dir_best_effort(const char * path) {
    char parent[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(parent)) return;
    memcpy(parent, path, len + 1);
    char * slash = strrchr(parent, '/');
    if (!slash) {
        memcpy(parent, ".", 2);
    } else if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    (void) fsync_path(parent, true);
}

static bool executable_regular_snapshot(const char * path, struct stat * out) {
    if (!path || !path[0] || lstat(path, out) != 0 || !S_ISREG(out->st_mode)) return false;
    return access(path, X_OK) == 0;
}

static bool same_file_snapshot(const struct stat * before, const struct stat * after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_mode == after->st_mode && before->st_size == after->st_size &&
           before->st_uid == after->st_uid && before->st_gid == after->st_gid &&
           before->st_nlink == after->st_nlink &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

/* This is a bounded hint cache, never authority to remove an installed player.
 * Metadata equality is a freshness heuristic, not a content-integrity proof. */
#define PLAYER_VERSION_CACHE_SUFFIX ".version-cache"
#define PLAYER_VERSION_CACHE_PATH_MAX 512
#define PLAYER_VERSION_CACHE_MAGIC UINT32_C(0x43505643)
#define PLAYER_VERSION_CACHE_VERSION 1

typedef struct {
    uint64_t dev, ino, mode, size;
    uint64_t mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
    uint64_t uid, gid, nlink;
} cache_snapshot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char packaged_path[PLAYER_VERSION_CACHE_PATH_MAX];
    char installed_path[PLAYER_VERSION_CACHE_PATH_MAX];
    char packaged_stamp[BOOT_BUILD_STAMP_LEN + 1];
    char installed_stamp[BOOT_BUILD_STAMP_LEN + 1];
    cache_snapshot_t packaged;
    cache_snapshot_t installed;
    uint64_t checksum;
} player_version_cache_t;

static uint64_t cache_checksum(const player_version_cache_t * cache) {
    const unsigned char * bytes = (const unsigned char *)cache;
    const size_t length = offsetof(player_version_cache_t, checksum);
    uint64_t value = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i) {
        value ^= bytes[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static void snapshot_to_cache(const struct stat * st, cache_snapshot_t * out) {
    out->dev = (uint64_t)st->st_dev;
    out->ino = (uint64_t)st->st_ino;
    out->mode = (uint64_t)st->st_mode;
    out->size = (uint64_t)st->st_size;
    out->mtime_sec = (uint64_t)st->st_mtim.tv_sec;
    out->mtime_nsec = (uint64_t)st->st_mtim.tv_nsec;
    out->ctime_sec = (uint64_t)st->st_ctim.tv_sec;
    out->ctime_nsec = (uint64_t)st->st_ctim.tv_nsec;
    out->uid = (uint64_t)st->st_uid;
    out->gid = (uint64_t)st->st_gid;
    out->nlink = (uint64_t)st->st_nlink;
}

static bool cache_snapshot_matches(const cache_snapshot_t * cached, const struct stat * st) {
    cache_snapshot_t current;
    snapshot_to_cache(st, &current);
    return memcmp(cached, &current, sizeof(current)) == 0;
}

static bool valid_calendar_stamp(const char * stamp) {
    if (!stamp || stamp[BOOT_BUILD_STAMP_LEN] != '\0') return false;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13) continue;
        if (stamp[i] < '0' || stamp[i] > '9') return false;
    }
    if (stamp[4] != '-' || stamp[7] != '-' || stamp[10] != '_' || stamp[13] != ':') return false;
    int year = (stamp[0]-'0')*1000 + (stamp[1]-'0')*100 + (stamp[2]-'0')*10 + stamp[3]-'0';
    int month = (stamp[5]-'0')*10 + stamp[6]-'0';
    int day = (stamp[8]-'0')*10 + stamp[9]-'0';
    int hour = (stamp[11]-'0')*10 + stamp[12]-'0';
    int minute = (stamp[14]-'0')*10 + stamp[15]-'0';
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!year || month < 1 || month > 12 || hour > 23 || minute > 59) return false;
    int max_day = month_days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++max_day;
    return day >= 1 && day <= max_day;
}

static bool make_cache_path(const char * installed, char out[PLAYER_VERSION_CACHE_PATH_MAX]) {
    size_t length = strlen(installed);
    size_t suffix = sizeof(PLAYER_VERSION_CACHE_SUFFIX) - 1;
    if (!length || length + suffix >= PLAYER_VERSION_CACHE_PATH_MAX) return false;
    memcpy(out, installed, length);
    memcpy(out + length, PLAYER_VERSION_CACHE_SUFFIX, suffix + 1);
    return true;
}

/* 1 = valid record, 0 = miss, -1 = unsafe path that must not be replaced. */
static int read_player_version_cache(const char * cache_path, const char * packaged,
                                     const char * installed, const struct stat * packaged_now,
                                     const struct stat * installed_now,
                                     player_version_cache_t * out) {
    int fd = open(cache_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat opened, named;
    bool safe = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) && opened.st_nlink == 1 &&
                opened.st_uid == geteuid() && (opened.st_mode & 077) == 0 &&
                lstat(cache_path, &named) == 0 && same_file_snapshot(&opened, &named);
    if (!safe) { close(fd); return -1; }

    player_version_cache_t cache;
    size_t used = 0;
    while (used < sizeof(cache)) {
        ssize_t n = read(fd, (unsigned char *)&cache + used, sizeof(cache) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    unsigned char extra;
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    bool exact = used == sizeof(cache) && trailing == 0;
    struct stat after_fd, after_path;
    bool stable = fstat(fd, &after_fd) == 0 && lstat(cache_path, &after_path) == 0 &&
                  same_file_snapshot(&opened, &after_fd) && same_file_snapshot(&opened, &after_path);
    close(fd);
    if (!exact || !stable) return stable ? 0 : -1;

    if (cache.magic != PLAYER_VERSION_CACHE_MAGIC || cache.version != PLAYER_VERSION_CACHE_VERSION ||
        cache.checksum != cache_checksum(&cache) ||
        !memchr(cache.packaged_path, '\0', sizeof(cache.packaged_path)) ||
        !memchr(cache.installed_path, '\0', sizeof(cache.installed_path)) ||
        strcmp(cache.packaged_path, packaged) != 0 || strcmp(cache.installed_path, installed) != 0 ||
        !valid_calendar_stamp(cache.packaged_stamp) || !valid_calendar_stamp(cache.installed_stamp) ||
        !cache_snapshot_matches(&cache.packaged, packaged_now) ||
        !cache_snapshot_matches(&cache.installed, installed_now)) return 0;
    *out = cache;
    return 1;
}

static bool write_all(int fd, const void * data, size_t length) {
    const unsigned char * p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        length -= (size_t)n;
    }
    return true;
}

static void write_player_version_cache(const char * cache_path, const char * packaged,
                                      const char * installed, const char * packaged_stamp,
                                      const char * installed_stamp, const struct stat * packaged_st,
                                      const struct stat * installed_st, bool may_replace) {
    if (strlen(packaged) >= PLAYER_VERSION_CACHE_PATH_MAX ||
        strlen(installed) >= PLAYER_VERSION_CACHE_PATH_MAX) return;
    struct stat existing;
    bool existed = false;
    if (lstat(cache_path, &existing) == 0) {
        if (!may_replace || !S_ISREG(existing.st_mode) || existing.st_nlink != 1 ||
            existing.st_uid != geteuid() || (existing.st_mode & 077) != 0) return;
        existed = true;
    } else if (errno != ENOENT) return;

    player_version_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.magic = PLAYER_VERSION_CACHE_MAGIC;
    cache.version = PLAYER_VERSION_CACHE_VERSION;
    snprintf(cache.packaged_path, sizeof(cache.packaged_path), "%s", packaged);
    snprintf(cache.installed_path, sizeof(cache.installed_path), "%s", installed);
    snprintf(cache.packaged_stamp, sizeof(cache.packaged_stamp), "%s", packaged_stamp);
    snprintf(cache.installed_stamp, sizeof(cache.installed_stamp), "%s", installed_stamp);
    snapshot_to_cache(packaged_st, &cache.packaged);
    snapshot_to_cache(installed_st, &cache.installed);
    cache.checksum = cache_checksum(&cache);

    static unsigned counter;
    char temp[PLAYER_VERSION_CACHE_PATH_MAX];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        unsigned serial = ++counter;
        int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld.%u", cache_path, (long)getpid(), serial);
        if (n < 0 || (size_t)n >= sizeof(temp)) return;
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return;
    bool ok = write_all(fd, &cache, sizeof(cache));
    if (ok) (void)fsync(fd);
    if (close(fd) != 0) ok = false;
    struct stat current;
    if (ok) {
        if (existed) {
            ok = lstat(cache_path, &current) == 0 && same_file_snapshot(&existing, &current);
        } else {
            ok = lstat(cache_path, &current) != 0 && errno == ENOENT;
        }
    }
    if (ok && rename(temp, cache_path) == 0) fsync_parent_dir_best_effort(cache_path);
    else (void)unlink(temp);
}

const char * installer_select_internal_player(const char * packaged, const char * installed) {
    struct stat packaged_before, installed_before;
    /* Preserve the legacy selection policy exactly. Strict lstat-based
     * validation below only decides whether an installed override is safe
     * to delete; an executable symlink remains selected but is never
     * deletion-eligible. */
    if (!scanner_path_is_executable(installed)) return packaged;
    if (!executable_regular_snapshot(installed, &installed_before)) return installed;

    if (!executable_regular_snapshot(packaged, &packaged_before)) return installed;

    char cache_path[PLAYER_VERSION_CACHE_PATH_MAX] = {0};
    player_version_cache_t cached;
    int cache_state = -1;
    if (make_cache_path(installed, cache_path)) {
        cache_state = read_player_version_cache(cache_path, packaged, installed,
                                                &packaged_before, &installed_before, &cached);
        /* A cache hit can only preserve the installed override. A cached newer
         * packaged player always goes through the complete scan and checks below. */
        if (cache_state == 1 && strcmp(cached.packaged_stamp, cached.installed_stamp) <= 0) {
            struct stat packaged_cached_after, installed_cached_after;
            if (executable_regular_snapshot(packaged, &packaged_cached_after) &&
                executable_regular_snapshot(installed, &installed_cached_after) &&
                same_file_snapshot(&packaged_before, &packaged_cached_after) &&
                same_file_snapshot(&installed_before, &installed_cached_after)) return installed;
            cache_state = 0;
        }
    }

    char packaged_stamp[BOOT_BUILD_STAMP_LEN + 1];
    char installed_stamp[BOOT_BUILD_STAMP_LEN + 1];
    if (!scanner_read_build_stamp(packaged, packaged_stamp, sizeof(packaged_stamp)) ||
        !scanner_read_build_stamp(installed, installed_stamp, sizeof(installed_stamp))) {
        return installed;
    }

    struct stat packaged_after, installed_after;
    if (!executable_regular_snapshot(packaged, &packaged_after) ||
        !executable_regular_snapshot(installed, &installed_after) ||
        !same_file_snapshot(&packaged_before, &packaged_after) ||
        !same_file_snapshot(&installed_before, &installed_after)) {
        fprintf(stderr, "installer: player changed during version check; preserving installed copy\n");
        return installed;
    }

    fprintf(stderr, "installer: packaged player %s, installed player %s\n", packaged_stamp, installed_stamp);
    if (strcmp(packaged_stamp, installed_stamp) <= 0) {
        if (cache_path[0] && valid_calendar_stamp(packaged_stamp) && valid_calendar_stamp(installed_stamp)) {
            write_player_version_cache(cache_path, packaged, installed, packaged_stamp, installed_stamp,
                                       &packaged_after, &installed_after, cache_state >= 0);
        }
        return installed;
    }

    fprintf(stderr, "installer: packaged player is newer; removing stale installed override %s\n", installed);
    if (unlink(installed) != 0) {
        fprintf(stderr, "installer: unlink(%s) failed: %s; booting packaged player anyway\n",
                installed, strerror(errno));
    } else {
        fsync_parent_dir_best_effort(installed);
    }
    return packaged;
}

const char * installer_internal_player_path(void) {
    return scanner_path_is_executable(INSTALLED_PLAYER_PATH)
        ? installer_select_internal_player(INTERNAL_PLAYER_PATH, INSTALLED_PLAYER_PATH)
        : INTERNAL_PLAYER_PATH;
}
