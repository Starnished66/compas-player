#include "plugin_reserved_dirs.h"
#include "../../core/device/app_approval.h"
#include "../../library/browser/paths/library_paths.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char * const reserved_names[] = { ".apps", ".apps-data", ".emulators" };
#define RESERVED_COUNT ((int) (sizeof(reserved_names) / sizeof(reserved_names[0])))
#define PROTECTED_MAX (RESERVED_COUNT + 2)

/* An absolute path with "." and ".." collapsed and no duplicate slashes,
 * without touching the filesystem. */
static bool normalize(const char * in, char * out, size_t size) {
    char joined[PATH_MAX];
    if (!in || !in[0]) return false;
    if (in[0] == '/') {
        if (snprintf(joined, sizeof(joined), "%s", in) >= (int) sizeof(joined)) return false;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)) || snprintf(joined, sizeof(joined), "%s/%s", cwd, in) >= (int) sizeof(joined))
            return false;
    }
    char * parts[PATH_MAX / 2];
    int depth = 0;
    for (char * p = joined; *p;) {
        while (*p == '/') p++;
        char * start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - start);
        if (*p) *p++ = '\0';
        if (len == 0 || (len == 1 && start[0] == '.')) continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) depth--;
            continue;
        }
        if (depth >= (int) (sizeof(parts) / sizeof(parts[0]))) return false;
        parts[depth++] = start;
    }
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        int n = snprintf(out + used, size - used, "/%s", parts[i]);
        if (n < 0 || (size_t) n >= size - used) return false;
        used += (size_t) n;
    }
    if (used == 0) snprintf(out, size, "/");
    return true;
}

/* True when any component of path names a reserved folder. FAT and exFAT
 * ignore case and drop trailing dots and spaces, so ".APPS." and ".apps "
 * are the folder too; the check applies at any depth, so nested creation
 * and a file operation's destination/<source name> are covered. */
static bool has_reserved_component(const char * path) {
    for (const char * p = path; *p;) {
        while (*p == '/') p++;
        const char * start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - start);
        while (len > 0 && (start[len - 1] == '.' || start[len - 1] == ' ')) len--;
        for (int i = 0; i < RESERVED_COUNT; i++) {
            if (len == strlen(reserved_names[i]) && strncasecmp(start, reserved_names[i], len) == 0) return true;
        }
    }
    return false;
}

/* The approval file and its temporary (internal storage, but plugins write
 * there as root too). */
static bool is_approval_file(const char * path) {
    char approval[PATH_MAX], tmp[PATH_MAX];
    if (!normalize(APP_APPROVAL_FILE, approval, sizeof(approval))) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", approval);
    return strcmp(path, approval) == 0 || strcmp(path, tmp) == 0;
}

static bool is_reserved_identity(const struct stat * st, const struct stat * reserved, int count) {
    for (int i = 0; i < count; i++) {
        if (st->st_dev == reserved[i].st_dev && st->st_ino == reserved[i].st_ino) return true;
    }
    return false;
}

/* path with its existing part resolved through links and other mount points
 * of the card (the player reaches the card as /data/mnt/sd_0 and
 * /usr/data/mnt/sd_0). */
static bool resolve_existing(const char * norm, char * out, size_t size) {
    char prefix[PATH_MAX], real[PATH_MAX];
    size_t len = strlen(norm);
    for (size_t cut = len + 1; cut-- > 0;) {
        if (norm[cut] != '/' && norm[cut] != '\0') continue;
        memcpy(prefix, norm, cut);
        prefix[cut] = '\0';
        if (cut == 0) snprintf(prefix, sizeof(prefix), "/");
        if (realpath(prefix, real)) {
            int n = snprintf(out, size, "%s%s", strcmp(real, "/") == 0 ? "" : real, norm + cut);
            return n >= 0 && (size_t) n < size;
        }
    }
    return false;
}

bool plugin_path_in_program_dirs_of(const char * sd_root, const char * path) {
    char norm[PATH_MAX], resolved[PATH_MAX];
    if (!normalize(path, norm, sizeof(norm))) return true; /* unusable names are refused, not trusted */
    bool have_resolved = resolve_existing(norm, resolved, sizeof(resolved));
    if (has_reserved_component(norm) || is_approval_file(norm) ||
        (have_resolved && (has_reserved_component(resolved) || is_approval_file(resolved)))) {
        return true;
    }
    /* Identity: the existing folders and files themselves, reached by any
     * other name (vfat short names such as APPS~1, links, another mount of
     * the card). A name that does not exist yet cannot be an alias of one
     * that does. */
    char root[PATH_MAX], protected_path[PROTECTED_MAX][PATH_MAX + 16];
    struct stat guarded[PROTECTED_MAX];
    int existing = 0, names = 0;
    if (!normalize(sd_root, root, sizeof(root))) return false;
    for (int i = 0; i < RESERVED_COUNT; i++)
        snprintf(protected_path[names++], sizeof(protected_path[0]), "%s/%s", strcmp(root, "/") == 0 ? "" : root,
                 reserved_names[i]);
    snprintf(protected_path[names++], sizeof(protected_path[0]), "%s", APP_APPROVAL_FILE);
    snprintf(protected_path[names++], sizeof(protected_path[0]), "%s.tmp", APP_APPROVAL_FILE);
    for (int i = 0; i < names; i++) {
        if (stat(protected_path[i], &guarded[existing]) == 0) existing++;
    }
    if (existing == 0) return false;
    char prefix[PATH_MAX];
    size_t len = strlen(norm);
    for (size_t i = 1; i <= len; i++) {
        if (norm[i] != '/' && norm[i] != '\0') continue;
        memcpy(prefix, norm, i);
        prefix[i] = '\0';
        struct stat st;
        if (stat(prefix, &st) != 0) break;
        if (is_reserved_identity(&st, guarded, existing)) return true;
    }
    return false;
}

bool plugin_path_in_program_dirs(const char * path) {
    return plugin_path_in_program_dirs_of(MUSIC_ROOT_DIR, path);
}
