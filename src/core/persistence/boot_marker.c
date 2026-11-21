#include "boot_marker.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

/* fsyncs the directory that holds path so the entry itself survives a crash. */
static void sync_parent_dir(const char * path) {
    char dir[PATH_MAX];
    const char * slash = strrchr(path, '/');
    if (!slash || (size_t) (slash - path) >= sizeof(dir)) return;
    memcpy(dir, path, (size_t) (slash - path));
    dir[slash - path] = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void) fsync(fd);
    close(fd);
}

void boot_marker_set_text(const char * path, const char * text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t len = strlen(text);
        if (len > 0) (void) !write(fd, text, len);
        (void) fsync(fd);
        close(fd);
    }
    sync_parent_dir(path);
}

void boot_marker_set(const char * path) {
    boot_marker_set_text(path, "");
}

bool boot_marker_read(const char * path, char * buf, size_t size) {
    if (size == 0) return false;
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return true;
}

bool boot_marker_exists(const char * path) {
    return access(path, F_OK) == 0;
}

void boot_marker_clear(const char * path) {
    (void) unlink(path);
    sync_parent_dir(path);
}
