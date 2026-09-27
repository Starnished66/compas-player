#ifndef SD_FSCK_H
#define SD_FSCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decides whether a mounted SD card should be repaired, and which
 * non-interactive checker to run. The device runner in sd_fsck_run.c
 * performs the unmount, the check, and the remount. */

#define SD_FSCK_DEVICE_BYTES 96
#define SD_FSCK_MOUNT_BYTES 160
#define SD_FSCK_FSTYPE_BYTES 32
#define SD_FSCK_TOOL_BYTES 128
#define SD_FSCK_ATTEMPT_BYTES 80

typedef enum {
    SD_FS_KIND_UNKNOWN = 0,
    SD_FS_KIND_VFAT,
    SD_FS_KIND_EXFAT,
    SD_FS_KIND_NTFS,
} sd_fs_kind_t;

/* Posted for the UI thread. show_error_toast() is not safe before the
 * notification widgets exist, and it is not safe from the repair thread. */
typedef enum {
    SD_REPAIR_NOTE_NONE = 0,
    SD_REPAIR_NOTE_STARTED,
    SD_REPAIR_NOTE_REPAIRED,
    SD_REPAIR_NOTE_CHECKED,
    SD_REPAIR_NOTE_STILL_READONLY,
    SD_REPAIR_NOTE_FAILED,
    SD_REPAIR_NOTE_NEEDS_COMPUTER,
    SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER,
} sd_repair_note_t;

typedef struct {
    sd_fs_kind_t kind;
    unsigned fat_bits;   /* 12, 16 or 32 for FAT; 0 for exFAT */
    uint64_t clusters;
} sd_fs_geometry_t;

typedef struct {
    bool found;
    bool readonly;
    sd_fs_kind_t kind;
    char device[SD_FSCK_DEVICE_BYTES];
    char mount_point[SD_FSCK_MOUNT_BYTES];
    char fstype[SD_FSCK_FSTYPE_BYTES];
} sd_mount_info_t;

typedef struct {
    char tool[SD_FSCK_TOOL_BYTES];
    char device[SD_FSCK_DEVICE_BYTES];
    char arg1[8];
    char arg2[8];
    char * argv[6];
} sd_fsck_plan_t;

/* Last matching SD mount line wins. Recognizes both /data/mnt/sd_0 and
 * /usr/data/mnt/sd_0. A line is read-only only when its option list has
 * an exact "ro" token and no "rw" token. */
bool sd_fsck_parse_mounts(const char * mounts_text, sd_mount_info_t * out);

sd_fs_kind_t sd_fsck_kind_from_type(const char * fstype);

/* Repair is only offered for the internal card's own block nodes. */
bool sd_fsck_device_allowed(const char * device);

/* Candidate absolute paths, NULL-terminated. The image overlay installs
 * the first entry of each list. */
const char * const * sd_fsck_tool_candidates(sd_fs_kind_t kind);

/* exists() reports whether a candidate path can be executed. */
const char * sd_fsck_select_tool(sd_fs_kind_t kind, bool (* exists)(const char * path, void * ctx), void * ctx);

bool sd_fsck_plan(sd_fs_kind_t kind, const char * tool, const char * device, sd_fsck_plan_t * out);

/* Standard fsck status bits: 0 clean, 1 corrected, 2 reboot requested.
 * Any other bit means the filesystem was not left consistent. */
bool sd_fsck_exit_usable(int exit_code);

/* Whether the checker finished its work. fsck.fat exits 1 both after a
 * repair and when it dies (including out of memory), so its status alone
 * is not enough: only a finished run prints the "<device>: N files, a/b
 * clusters" summary. output is the tail of what this run printed. */
bool sd_fsck_tool_succeeded(sd_fs_kind_t kind, int exit_code, const char * output, const char * device);

/* Note for the finished check. dirty_trigger is true when the check ran
 * because the card was not safely removed, false when it was read-only. */
sd_repair_note_t sd_fsck_outcome(bool tool_ok, bool dirty_trigger, bool mounted, bool readonly);

/* Reads the cluster count from a FAT12/16/32 or exFAT boot sector. */
bool sd_fsck_parse_boot_sector(const unsigned char * sector, size_t length, sd_fs_geometry_t * out);

/* Upper estimate of the checker's peak heap in bytes. fsck.fat keeps both
 * FAT copies while reading, then one copy plus two per-cluster arrays. */
uint64_t sd_fsck_memory_estimate(const sd_fs_geometry_t * geometry);

/* True when the kernel log holds a "Volume was not properly unmounted"
 * warning for device_base (for example "mmcblk0p1") after the latest SD
 * card insertion line. SDIO insertions (the Wi-Fi chip) are ignored.
 * stamp receives the insertion line's timestamp, or "none", so a card
 * inserted again gets a new attempt key. */
bool sd_fsck_klog_dirty(const char * log, const char * device_base, char * stamp, size_t stamp_size);

/* One automatic attempt per card. A readable CID identifies the card;
 * without one, the block node is the identity for this boot. */
void sd_repair_attempt_key(const char * device, const char * cid, char * out, size_t out_size);

#endif
