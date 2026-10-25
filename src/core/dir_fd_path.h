#ifndef DIR_FD_PATH_H
#define DIR_FD_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#endif

/* Writes a path that names the directory open on fd. On Linux this is the
 * /proc/self/fd entry, which stays bound to that directory even if the mount
 * point is later replaced; other systems have no such link and get the
 * directory's real path instead (Windows: compas-sdtool's compat layer). */
#ifdef _WIN32
bool dir_fd_path(int fd, char *out, size_t size);
#elif defined(__APPLE__)
static inline bool dir_fd_path(int fd, char *out, size_t size) {
    char path[PATH_MAX];
    if (fcntl(fd, F_GETPATH, path) != 0 || strlen(path) >= size) return false;
    memcpy(out, path, strlen(path) + 1);
    return true;
}
#else
static inline bool dir_fd_path(int fd, char *out, size_t size) {
    return snprintf(out, size, "/proc/self/fd/%d", fd) < (int) size;
}
#endif

#endif
