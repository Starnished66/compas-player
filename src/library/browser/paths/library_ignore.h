#ifndef LIBRARY_IGNORE_H
#define LIBRARY_IGNORE_H

#include "library_paths.h"
#include "../../../core/persistence/storage_paths.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Which stored paths the library walk would not deliver, for tagcache to drop
 * their rows like deleted files. */

/* Where stored paths are rooted: the card mount for compas-sdtool, whose
 * storage_stored_path() maps it to the real card, else the music root. */
#ifdef COMPAS_RUNTIME_SD_ROOT
#define LIBRARY_STORED_ROOT SD_DEVICE_MOUNT
#else
#define LIBRARY_STORED_ROOT MUSIC_ROOT_DIR
#endif

typedef enum { LIBRARY_INCLUDED, LIBRARY_IGNORED, LIBRARY_IGNORE_UNKNOWN } library_ignore_t;

/* Whether dir is a symlink: 1 yes, 0 no, -1 could not tell. The walk never
 * enters a symlinked folder, so nothing below one is delivered. Windows has no
 * symlinks here (compas-sdtool's lstat is stat). */
static inline int library_dir_is_symlink(const char * dir) {
    struct stat st;
    if (lstat(dir, &st) != 0) return -1;
    return S_ISLNK(st.st_mode) ? 1 : 0;
}

/* The file a stored path names on this machine, or NULL when it cannot be
 * told: when the path is too long, compas-sdtool gets back the unmapped player
 * path or an empty one, and neither may be probed. The player maps nothing. */
static inline const char * library_host_path(const char * stored, char * out, size_t size) {
    const char * real = storage_stored_path(stored, out, size);
#ifdef COMPAS_RUNTIME_SD_ROOT
    if (real == stored || !real[0]) return NULL;
#endif
    return real;
}

/* Whether a stored file path under root is below a symlinked folder, which the
 * walk skips: IGNORED only when a folder strictly below root on the path is a
 * confirmed link, UNKNOWN when a folder cannot be mapped here. The root itself
 * may be a link, as the walk starts there. A path outside root is included. */
static inline library_ignore_t library_path_under_symlink(const char * root, const char * path) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0 || path[root_len] != '/') return LIBRARY_INCLUDED;
    size_t dir_end = (size_t) (strrchr(path, '/') - path);
    char dir[PATH_MAX], mapped[PATH_MAX];
    if (dir_end >= sizeof(dir)) return LIBRARY_IGNORE_UNKNOWN;
    /* Each folder ends at the next slash; the one at dir_end is always found. */
    for (size_t n = root_len; n < dir_end; ) {
        n = (size_t) ((const char *) memchr(path + n + 1, '/', dir_end - n) - path);
        memcpy(dir, path, n);
        dir[n] = '\0';
        const char * real = library_host_path(dir, mapped, sizeof(mapped));
        if (!real) return LIBRARY_IGNORE_UNKNOWN;   /* the card path could not be built */
        if (library_dir_is_symlink(real) > 0) return LIBRARY_IGNORED;
    }
    return LIBRARY_INCLUDED;
}

#endif /* LIBRARY_IGNORE_H */
