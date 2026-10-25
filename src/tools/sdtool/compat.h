#ifndef SDTOOL_COMPAT_H
#define SDTOOL_COMPAT_H

/* The player's library code is written for Linux. This header, force-included
 * (-include) into compas-sdtool's own build on other systems, supplies the few
 * POSIX calls it uses that those systems lack, so the player sources carry no
 * per-platform conditionals. Linux builds do not include it.
 *
 * macOS: only two names are missing.
 * Windows (MinGW-w64): paths are UTF-8 in the program and converted for the
 * wide Windows API; file times are 64-bit; a folder opened with O_DIRECTORY
 * gets a stand-in descriptor that the *at() calls resolve (compat_dirs.c). */

#if defined(__APPLE__)

#include <unistd.h>
#define fdatasync fsync
/* Free memory is unknown here; callers then choose their smallest layouts. */
#define _SC_AVPHYS_PAGES (-1)

#elif defined(_WIN32)

#include <dirent.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#undef PATH_MAX
#define PATH_MAX 4096
#define NAME_MAX 255

/* Flags with no Windows meaning are accepted and ignored. */
#define O_DIRECTORY 0x10000000
#define O_NOFOLLOW 0
#define O_CLOEXEC 0
#define O_NONBLOCK 0
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define S_ISLNK(mode) 0

int compat_open(const char *path, int flags, ...);
FILE *compat_fopen(const char *path, const char *mode);
int compat_close(int fd);
int compat_dup(int fd);
int compat_unlink(const char *path);
int compat_rename(const char *from, const char *to);
int compat_access(const char *path, int mode);
int compat_mkdir(const char *path);
int compat_stat(const char *path, struct _stat64 *st);
int compat_fstat(int fd, struct _stat64 *st);
int compat_fsync(int fd);
long compat_sysconf(int name);
int compat_mkstemp(char *pattern);
ssize_t pread(int fd, void *buffer, size_t length, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t length, off_t offset);

/* 64-bit sizes and times: `struct stat` becomes `struct _stat64`, and the call
 * `_stat64(path, st)` is routed through the UTF-8 wrapper. */
#define stat _stat64
#define _stat64(path, st) compat_stat((path), (st))
#define lstat(path, st) compat_stat((path), (st))
#undef fstat
#define fstat(fd, st) compat_fstat((fd), (st))
#define open compat_open
#define fopen compat_fopen
#define close compat_close
#define dup compat_dup
#define unlink compat_unlink
#define rename compat_rename
#define access compat_access
#define mkdir(path, mode) compat_mkdir(path)
#define fsync compat_fsync
#define fdatasync compat_fsync
#define sysconf compat_sysconf
#define mkstemp compat_mkstemp
#define _SC_PAGESIZE 1
#define _SC_AVPHYS_PAGES (-1)
#define _SC_NPROCESSORS_ONLN 3

/* Folders, opened by name or by descriptor. */
typedef struct compat_dir compat_dir_t;
struct compat_dirent {
    char d_name[PATH_MAX];
};
compat_dir_t *compat_opendir(const char *path);
compat_dir_t *compat_fdopendir(int fd);
struct compat_dirent *compat_readdir(compat_dir_t *dir);
void compat_rewinddir(compat_dir_t *dir);
int compat_closedir(compat_dir_t *dir);
#define DIR compat_dir_t
#define dirent compat_dirent
#define opendir compat_opendir
#define fdopendir compat_fdopendir
#define readdir compat_readdir
#define rewinddir compat_rewinddir
#define closedir compat_closedir

int openat(int dir_fd, const char *path, int flags, ...);
int mkdirat(int dir_fd, const char *path, int mode);
int unlinkat(int dir_fd, const char *path, int flags);
int renameat(int from_fd, const char *from, int to_fd, const char *to);
int fstatat(int dir_fd, const char *path, struct _stat64 *st, int flags);

/* The program's own entry point is main(); a UTF-8 wrapper calls it. */
#define main sdtool_main

#endif /* platform */
#endif /* SDTOOL_COMPAT_H */
