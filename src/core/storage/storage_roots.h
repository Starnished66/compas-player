#ifndef STORAGE_ROOTS_H
#define STORAGE_ROOTS_H

#include "usb_drive.h"

#include <stdbool.h>
#include <stddef.h>

/* The storages the player browses: the SD card first, then each mounted USB
 * drive partition. One list serves the File Manager, Music > Files and the
 * Remote Control folders API. */

#define STORAGE_ROOTS_MAX (1 + USB_DRIVE_MAX)

typedef struct {
    char id[8];                            /* "sd", "usb0", "usb1", ... */
    char path[USB_DRIVE_MOUNT_BYTES];
    char label[USB_DRIVE_LABEL_BYTES];     /* volume label, "" when it has none */
    bool removable;                        /* a USB drive */
} storage_root_t;

/* Builds the list from the SD mount point and the mounted drives. */
size_t storage_roots_build(const char * sd_path, const usb_drive_t * drives, size_t drive_count,
                           storage_root_t * out, size_t max);

/* The live list. */
size_t storage_roots_list(storage_root_t * out, size_t max);

/* How many USB drive partitions are mounted. Zero means the screens skip the
 * storage list and open straight into the SD card. */
size_t storage_roots_drive_count(void);

/* The root that holds path, or false. */
bool storage_roots_find_path(const storage_root_t * roots, size_t count, const char * path, size_t * index);

/* The root with this id, or false. */
bool storage_roots_find_id(const storage_root_t * roots, size_t count, const char * id, size_t * index);

/* Label when the volume has one, otherwise the given default. The caller
 * passes translated defaults ("SD card", "USB storage"). */
const char * storage_root_name(const storage_root_t * root, const char * sd_default, const char * usb_default);

#endif
