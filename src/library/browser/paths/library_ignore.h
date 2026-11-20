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
 * their rows like deleted files, and the database.ignore / database.unignore
 * rule, in one place for the walk and for tagcache. A directory holding
 * exactly one of the two markers sets whether its files are included; both or
 * neither inherit. */

/* Where stored paths are rooted: the card mount for compas-sdtool, whose
 * storage_stored_path() maps it to the real card, else the music root. */
#ifdef COMPAS_RUNTIME_SD_ROOT
#define LIBRARY_STORED_ROOT SD_DEVICE_MOUNT
#else
#define LIBRARY_STORED_ROOT MUSIC_ROOT_DIR
#endif

typedef enum { LIBRARY_INCLUDED, LIBRARY_IGNORED, LIBRARY_IGNORE_UNKNOWN } library_ignore_t;

/* Marker present. Any failure but ENOENT sets *unsure (when given): the answer
 * could not be told, which the walk ignores and tagcache must not. */
static inline bool library_marker_present(const char * dir_path, const char * name, char * probe,
                                          size_t probe_size, bool * unsure) {
    int len = snprintf(probe, probe_size, "%s/%s", dir_path, name);
    if (len > 0 && (size_t) len < probe_size && access(probe, F_OK) == 0) return true;
    if (unsure && (len <= 0 || (size_t) len >= probe_size || errno != ENOENT)) *unsure = true;
    return false;
}

/* Whether files of dir_path are included, given the parent's answer. probe is
 * caller scratch of probe_size bytes, so a recursive caller keeps one buffer. */
static inline bool library_dir_includes(const char * dir_path, char * probe, size_t probe_size, bool inherited,
                                        bool * unsure) {
    bool has_ignore = library_marker_present(dir_path, "database.ignore", probe, probe_size, unsure);
    bool has_unignore = library_marker_present(dir_path, "database.unignore", probe, probe_size, unsure);
    return has_ignore != has_unignore ? has_unignore : inherited;
}

/* Whether dir (trailing slash allowed) is a symlink: 1 yes, 0 no, -1 could not
 * tell. The walk never enters a symlinked folder, so nothing below one is
 * delivered. Windows has no symlinks here (compas-sdtool's lstat is stat). */
static inline int library_dir_is_symlink(const char * dir, char * probe, size_t probe_size) {
    int len = snprintf(probe, probe_size, "%s", dir);
    if (len <= 0 || (size_t) len >= probe_size) return -1;
    if (len > 1 && probe[len - 1] == '/') probe[len - 1] = '\0';
    struct stat st;
    if (lstat(probe, &st) != 0) return -1;
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

/* Whether a stored file path under root is excluded: below a symlinked folder
 * (the walk skips those; the root itself may be a link, as the walk starts
 * there), or, unless links_only, by the markers of its ancestor directories,
 * root first. A path outside root is included. IGNORED by markers is reported
 * only when no failed probe could have changed the answer; UNKNOWN also when a
 * folder cannot be mapped here. */
static inline library_ignore_t library_path_check(const char * root, const char * path, bool links_only) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0 || path[root_len] != '/') return LIBRARY_INCLUDED;
    size_t dir_end = (size_t) (strrchr(path, '/') - path);
    char dir[PATH_MAX], mapped[PATH_MAX], probe[PATH_MAX];
    if (dir_end + 2 >= sizeof(dir)) return LIBRARY_IGNORE_UNKNOWN;
    bool include = true, unsure = false;
    /* Each folder ends at the next slash; the one at dir_end is always found. */
    for (size_t n = root_len; ; n = (size_t) ((const char *) memchr(path + n + 1, '/', dir_end - n) - path)) {
        memcpy(dir, path, n);
        dir[n] = '/';   /* compas-sdtool maps the card mount only as "mount/" */
        dir[n + 1] = '\0';
        const char * real = library_host_path(dir, mapped, sizeof(mapped));
        if (!real) return LIBRARY_IGNORE_UNKNOWN;   /* the card path could not be built */
        if (n > root_len) {
            int link = library_dir_is_symlink(real, probe, sizeof(probe));
            if (link > 0) return LIBRARY_IGNORED;
            if (link < 0) unsure = true;
        }
        if (!links_only) include = library_dir_includes(real, probe, sizeof(probe), include, &unsure);
        if (n >= dir_end) break;
    }
    if (include) return LIBRARY_INCLUDED;
    return unsure ? LIBRARY_IGNORE_UNKNOWN : LIBRARY_IGNORED;
}

static inline library_ignore_t library_path_ignored(const char * root, const char * path) {
    return library_path_check(root, path, false);
}

/* Only the symlink rule, for a file whose own stat failed, where markers say
 * nothing: IGNORED only when a folder below root on the path is a confirmed
 * link. */
static inline library_ignore_t library_path_under_symlink(const char * root, const char * path) {
    return library_path_check(root, path, true);
}

#endif /* LIBRARY_IGNORE_H */
