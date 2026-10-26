#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "selector_request.h"
#include "../../../install/internal_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char payload[] = "COMPAS_SELECTOR_1\n";
static const char name[] = "boot-selector.once";
static struct stat last_request;
static bool last_request_valid;
static const char temporary[] = ".boot-selector.once.tmp";

static void request_error(char *out, size_t size, const char *message) {
    if (out && size) snprintf(out, size, "%s", message);
}

static int request_directory(bool create) {
    char reason[192];
    if (!internal_storage_ready(reason, sizeof(reason))) return -1;
    int root = open("/usr/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -1;
    if (create && mkdirat(root, ".compas", 0700) && errno != EEXIST) {
        close(root);
        return -1;
    }
    int dir = openat(root, ".compas", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (dir >= 0 && (fstat(dir, &st) || st.st_uid != 0 || (st.st_mode & 0022))) {
        close(dir);
        dir = -1;
    }
    /* Persist a newly-created directory as well as its contents. */
    if (create && dir >= 0 && fsync(root)) { close(dir); dir = -1; }
    close(root);
    return dir;
}

static bool valid_request(int fd) {
    struct stat st;
    char data[sizeof(payload)];
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
        st.st_size != (off_t)(sizeof(payload) - 1)) return false;
    ssize_t count;
    do { count = pread(fd, data, sizeof(data), 0); } while (count < 0 && errno == EINTR);
    return count == (ssize_t)(sizeof(payload) - 1) && !memcmp(data, payload, sizeof(payload) - 1);
}

static bool remove_same(int dir, const char *entry, int fd) {
    struct stat ours, current;
    return !fstat(fd, &ours) && !fstatat(dir, entry, &current, AT_SYMLINK_NOFOLLOW) &&
           ours.st_dev == current.st_dev && ours.st_ino == current.st_ino &&
           !unlinkat(dir, entry, 0);
}

bool selector_request_write(char *error, size_t size) {
    last_request_valid = false;
    int dir = request_directory(true);
    if (dir < 0) {
        request_error(error, size, "Internal storage is unavailable or unsafe");
        return false;
    }
    int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        bool ok = valid_request(fd) && fsync(fd) == 0 && fsync(dir) == 0;
        if (ok) last_request_valid = fstat(fd, &last_request) == 0;
        close(fd); close(dir);
        if (!ok) request_error(error, size, "An unsafe boot selector request already exists");
        return ok;
    }
    if (errno != ENOENT) goto failed;
    /* Only a private, regular interrupted temporary file may be removed. */
    fd = openat(dir, temporary, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        bool safe = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                    (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && remove_same(dir, temporary, fd);
        close(fd);
        if (!safe) goto failed;
    } else if (errno != ENOENT) goto failed;
    fd = openat(dir, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) goto failed;
    bool ok = fchmod(fd, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < sizeof(payload) - 1) {
        ssize_t count = write(fd, payload + offset, sizeof(payload) - 1 - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += (size_t)count;
    }
    ok = ok && fsync(fd) == 0;
    bool renamed = false;
    if (ok) {
        /* Directory is trusted/root-owned; consumers never overwrite this
         * request while the player is running. */
        renamed = renameat(dir, temporary, dir, name) == 0;
        ok = renamed && fsync(dir) == 0;
    }
    if (!ok) {
        (void)remove_same(dir, renamed ? name : temporary, fd);
        (void)fsync(dir);
    }
    if (ok) last_request_valid = fstat(fd, &last_request) == 0;
    close(fd); close(dir);
    if (!ok) request_error(error, size, "Could not save the boot selector request");
    return ok;
failed:
    close(dir);
    request_error(error, size, "Could not safely create the boot selector request");
    return false;
}

bool selector_request_consume(void) {
    int dir = request_directory(false);
    if (dir < 0) return false;
    int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    bool ok = fd >= 0 && valid_request(fd) && remove_same(dir, name, fd) && fsync(dir) == 0;
    if (fd >= 0) close(fd);
    close(dir);
    return ok;
}

bool selector_request_cancel(void) {
    if (!last_request_valid) return true;
    int dir = request_directory(false);
    if (dir < 0) return false;
    struct stat current;
    bool ok = fstatat(dir, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
              current.st_dev == last_request.st_dev && current.st_ino == last_request.st_ino &&
              S_ISREG(current.st_mode) && unlinkat(dir, name, 0) == 0 && fsync(dir) == 0;
    close(dir);
    if (ok) last_request_valid = false;
    return ok;
}
