#include "compat_internal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Folders for compat.h: reading by name or descriptor, and the *at() calls
 * that Windows does not have, resolved against the stand-in descriptors that
 * compat_open() hands out for folders. */

#undef open
#undef close
#undef dup
#undef stat
#undef _stat64
#undef lstat
#undef fstat
#undef unlink
#undef rename
#undef mkdir
#undef opendir
#undef fdopendir
#undef readdir
#undef rewinddir
#undef closedir
#undef DIR
#undef dirent

#define DIR_FD_MAX 64

static struct {
    int fd;
    char path[PATH_MAX];
} dir_fds[DIR_FD_MAX];
static CRITICAL_SECTION dir_fds_lock;
static INIT_ONCE dir_fds_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void) once; (void) parameter; (void) context;
    InitializeCriticalSection(&dir_fds_lock);
    for (int i = 0; i < DIR_FD_MAX; i++) dir_fds[i].fd = -1;
    return TRUE;
}

int compat_dir_fd_register(int fd, const char *path) {
    int result = -1;
    InitOnceExecuteOnce(&dir_fds_once, init_lock, NULL, NULL);
    EnterCriticalSection(&dir_fds_lock);
    for (int i = 0; i < DIR_FD_MAX && result != 0; i++) {
        if (dir_fds[i].fd >= 0) continue;
        if (strlen(path) >= sizeof(dir_fds[i].path)) break;
        dir_fds[i].fd = fd;
        strcpy(dir_fds[i].path, path);
        result = 0;
    }
    LeaveCriticalSection(&dir_fds_lock);
    if (result != 0) errno = EMFILE;
    return result;
}

void compat_dir_fd_forget(int fd) {
    InitOnceExecuteOnce(&dir_fds_once, init_lock, NULL, NULL);
    EnterCriticalSection(&dir_fds_lock);
    for (int i = 0; i < DIR_FD_MAX; i++)
        if (dir_fds[i].fd == fd) dir_fds[i].fd = -1;
    LeaveCriticalSection(&dir_fds_lock);
}

/* Callers copy the path before the descriptor can be closed. */
const char *compat_dir_fd_path(int fd) {
    const char *path = NULL;
    InitOnceExecuteOnce(&dir_fds_once, init_lock, NULL, NULL);
    EnterCriticalSection(&dir_fds_lock);
    for (int i = 0; i < DIR_FD_MAX && !path; i++)
        if (dir_fds[i].fd == fd) path = dir_fds[i].path;
    LeaveCriticalSection(&dir_fds_lock);
    return path;
}

bool dir_fd_path(int fd, char *out, size_t size) {
    const char *path = compat_dir_fd_path(fd);
    if (!path || strlen(path) >= size) return false;
    strcpy(out, path);
    return true;
}

bool compat_join_at(int dir_fd, const char *name, char *out, size_t size) {
    const char *dir = dir_fd == AT_FDCWD ? "." : compat_dir_fd_path(dir_fd);
    if (!dir) {
        errno = EBADF;
        return false;
    }
    if (snprintf(out, size, "%s/%s", dir, name) >= (int) size) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

int openat(int dir_fd, const char *path, int flags, ...) {
    char full[PATH_MAX];
    if (!compat_join_at(dir_fd, path, full, sizeof(full))) return -1;
    return compat_open(full, flags);
}

int mkdirat(int dir_fd, const char *path, int mode) {
    char full[PATH_MAX];
    (void) mode;
    return compat_join_at(dir_fd, path, full, sizeof(full)) ? compat_mkdir(full) : -1;
}

int unlinkat(int dir_fd, const char *path, int flags) {
    char full[PATH_MAX];
    if (!compat_join_at(dir_fd, path, full, sizeof(full))) return -1;
    if (!(flags & AT_REMOVEDIR)) return compat_unlink(full);
    wchar_t wide[PATH_MAX];
    return compat_to_wide(full, wide, PATH_MAX) ? _wrmdir(wide) : -1;
}

int renameat(int from_fd, const char *from, int to_fd, const char *to) {
    char full_from[PATH_MAX], full_to[PATH_MAX];
    if (!compat_join_at(from_fd, from, full_from, sizeof(full_from)) ||
        !compat_join_at(to_fd, to, full_to, sizeof(full_to)))
        return -1;
    return compat_rename(full_from, full_to);
}

int fstatat(int dir_fd, const char *path, struct _stat64 *st, int flags) {
    char full[PATH_MAX];
    (void) flags;
    return compat_join_at(dir_fd, path, full, sizeof(full)) ? compat_stat(full, st) : -1;
}

struct compat_dir {
    HANDLE handle;
    WIN32_FIND_DATAW found;
    wchar_t pattern[PATH_MAX];
    bool have_entry;
    int fd; /* descriptor the folder was opened from, closed with it; -1 if by name */
    struct compat_dirent entry;
};

static bool dir_start(compat_dir_t *dir) {
    dir->handle = FindFirstFileW(dir->pattern, &dir->found);
    dir->have_entry = dir->handle != INVALID_HANDLE_VALUE;
    return dir->have_entry || GetLastError() == ERROR_FILE_NOT_FOUND;
}

compat_dir_t *compat_opendir(const char *path) {
    compat_dir_t *dir = calloc(1, sizeof(*dir));
    wchar_t wide[PATH_MAX];
    if (!dir) return NULL;
    dir->fd = -1;
    if (!compat_to_wide(path, wide, PATH_MAX) ||
        swprintf(dir->pattern, PATH_MAX, L"%ls\\*", wide) < 0 || !dir_start(dir)) {
        if (!errno) errno = ENOENT;
        free(dir);
        return NULL;
    }
    return dir;
}

compat_dir_t *compat_fdopendir(int fd) {
    const char *path = compat_dir_fd_path(fd);
    compat_dir_t *dir = path ? compat_opendir(path) : NULL;
    if (!path) errno = EBADF;
    if (dir) dir->fd = fd;
    return dir;
}

struct compat_dirent *compat_readdir(compat_dir_t *dir) {
    if (!dir->have_entry) return NULL;
    bool converted = compat_from_wide(dir->found.cFileName, dir->entry.d_name, sizeof(dir->entry.d_name));
    dir->have_entry = FindNextFileW(dir->handle, &dir->found) != 0;
    return converted ? &dir->entry : NULL;
}

void compat_rewinddir(compat_dir_t *dir) {
    if (dir->handle != INVALID_HANDLE_VALUE) FindClose(dir->handle);
    dir_start(dir);
}

int compat_closedir(compat_dir_t *dir) {
    if (dir->handle != INVALID_HANDLE_VALUE) FindClose(dir->handle);
    if (dir->fd >= 0) compat_close(dir->fd);
    free(dir);
    return 0;
}
