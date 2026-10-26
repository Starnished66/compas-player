#ifndef SDTOOL_STAT_SHIM_H
#define SDTOOL_STAT_SHIM_H

#include <sys/stat.h>

/* Force-included into the tool's build on Linux and macOS (Windows routes
 * stat() through compat.h). Every library stat() of a card file then reports
 * the modification time the player would read; see mtime.c. */
int sdtool_stat(const char *path, struct stat *st);
#define stat(path, st) sdtool_stat((path), (st))

#endif
