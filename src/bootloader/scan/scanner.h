#ifndef BOOTLOADER_SCANNER_H
#define BOOTLOADER_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

/* Always-present internal player -- if this itself is somehow missing/not
 * executable, that is a fatal condition for the whole device, not
 * something scanner.c tries to recover from. */
#define INTERNAL_PLAYER_PATH "/usr/bin/compas_player"

/* The loader mounts the card before checking its automatic player update. */
#define SD_DEVICE_NODE_PARTITION "/dev/mmcblk0p1"
#define SD_DEVICE_NODE_WHOLE_DISK "/dev/mmcblk0"
#define SD_MOUNT_POINT "/usr/data/mnt/sd_0"

#define SD_ALT_DIR SD_MOUNT_POINT "/.compas"
#define SD_UPDATE_PLAYER_PATH SD_ALT_DIR "/compas_player"

typedef struct {
    bool sd_update_present;
    const char *sd_update_path;
} scan_result_t;

#define BOOT_BUILD_STAMP_LEN 16 /* "YYYY-MM-DD_HH:MM" */

/* Reads INTERNAL_PLAYER_PATH's or INSTALLED_PLAYER_PATH's embedded
 * BUILD_STAMP (see this function's own doc comment in scanner.c for exactly
 * what counts as one and why the lexically-maximum match across the whole
 * file is what's returned) into out, NUL-terminated. Returns false (out
 * left untouched) if path has no such string at all -- callers must treat
 * that as "unknown version", never as "oldest possible version". */
bool scanner_read_build_stamp(const char * path, char * out, size_t out_size);

/* Returns true if path exists, is a regular file, and has execute permission. */
bool scanner_path_is_executable(const char * path);

#endif /* BOOTLOADER_SCANNER_H */
