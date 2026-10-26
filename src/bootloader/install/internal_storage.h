#ifndef BOOTLOADER_INTERNAL_STORAGE_H
#define BOOTLOADER_INTERNAL_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifndef INTERNAL_DATA_ROOT
#define INTERNAL_DATA_ROOT "/usr/data"
#endif

/* Checks the newest mountinfo entry whose mount point is exactly mount_point:
 * its device must be dev_major:dev_minor, its filesystem ubifs, its source a
 * UBI volume ("/dev/ubi..." or "ubi..."), and both its per-mount and
 * superblock options read-write. On failure writes a short reason. */
bool internal_storage_mountinfo_ok(FILE * mountinfo, const char * mount_point,
                                   unsigned dev_major, unsigned dev_minor,
                                   char * reason, size_t reason_size);

/* True only when INTERNAL_DATA_ROOT is its own writable UBIFS mount. Never
 * creates or writes files. On failure writes a short reason. */
bool internal_storage_ready(char * reason, size_t reason_size);

#endif /* BOOTLOADER_INTERNAL_STORAGE_H */
