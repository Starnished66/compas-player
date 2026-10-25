#ifndef SDTOOL_COMPAT_INTERNAL_H
#define SDTOOL_COMPAT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Shared by compat.c and compat_dirs.c (Windows only). */

/* UTF-8 to UTF-16; sets errno and returns false if it does not fit. */
bool compat_to_wide(const char *utf8, wchar_t *out, size_t count);
bool compat_from_wide(const wchar_t *wide, char *out, size_t size);

/* The folder a stand-in descriptor stands for, or NULL for any other fd. */
const char *compat_dir_fd_path(int fd);
int compat_dir_fd_register(int fd, const char *path);
void compat_dir_fd_forget(int fd);

/* path = folder + "/" + name; false if it does not fit. */
bool compat_join_at(int dir_fd, const char *name, char *out, size_t size);

#endif
