#ifndef SDTOOL_COMPAT_STATVFS_H
#define SDTOOL_COMPAT_STATVFS_H

/* Windows has no statvfs(); compat.c fills the fields the library reads. */
#include <stdint.h>

struct statvfs {
    unsigned long f_bsize;
    uint64_t f_bavail;
    uint64_t f_favail; /* FAT-style volumes have no inode limit; reported as 0 */
};

int statvfs(const char *path, struct statvfs *out);

#endif
