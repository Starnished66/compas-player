#include "sd_fsck.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char * what) {
    if (condition) return;
    fprintf(stderr, "sd fsck: %s\n", what);
    failures++;
}

static bool only_overlay_tools(const char * path, void * ctx) {
    (void) ctx;
    return strcmp(path, "/usr/sbin/fsck.vfat") == 0 || strcmp(path, "/usr/sbin/fsck.exfat") == 0 ||
           strcmp(path, "/usr/sbin/ntfsfix") == 0;
}

int main(void) {
    sd_mount_info_t info;
    const char * writable =
        "/dev/mmcblk0p1 /data/mnt/sd_0 vfat rw,relatime,fmask=0022,errors=remount-ro 0 0\n";
    expect(sd_fsck_parse_mounts(writable, &info), "writable line found");
    expect(info.found && !info.readonly && info.kind == SD_FS_KIND_VFAT, "errors=remount-ro is not a read-only mount");
    expect(strcmp(info.device, "/dev/mmcblk0p1") == 0, "partition device");

    const char * readonly =
        "proc /proc proc rw,nosuid 0 0\n"
        "/dev/mmcblk0p1 /usr/data/mnt/sd_0 vfat ro,relatime,errors=remount-ro 0 0\n";
    expect(sd_fsck_parse_mounts(readonly, &info) && info.readonly, "alias mount is read-only");
    expect(strcmp(info.mount_point, "/usr/data/mnt/sd_0") == 0, "alias path kept");

    const char * replaced =
        "/dev/mmcblk0p1 /data/mnt/sd_0 vfat ro,relatime 0 0\n"
        "/dev/mmcblk0 /data/mnt/sd_0 exfat rw,relatime 0 0\n";
    expect(sd_fsck_parse_mounts(replaced, &info) && !info.readonly && info.kind == SD_FS_KIND_EXFAT, "last mount wins");
    expect(strcmp(info.device, "/dev/mmcblk0") == 0, "whole-disk device");

    const char * ntfs =
        "/dev/mmcblk0p1 /data/mnt/sd_0 fuseblk ro,relatime,user_id=0 0 0\n";
    expect(sd_fsck_parse_mounts(ntfs, &info) && info.readonly && info.kind == SD_FS_KIND_NTFS, "ntfs-3g fuse mount");

    const char * escaped =
        "/dev/mmcblk0p1 /data/mnt/sd\\0400 exfat ro 0 0\n"
        "/dev/mmcblk0p1 /data/mnt/sd_0 exfat ro,relatime 0 0\n";
    expect(sd_fsck_parse_mounts(escaped, &info) && info.found && info.kind == SD_FS_KIND_EXFAT, "escaped unrelated path ignored");

    expect(!sd_fsck_parse_mounts("tmpfs /tmp tmpfs rw 0 0\n", &info) && !info.found, "other mounts are ignored");
    expect(!sd_fsck_device_allowed("/dev/sda1") && sd_fsck_device_allowed("/dev/mmcblk0"), "device allow list");

    expect(strcmp(sd_fsck_select_tool(SD_FS_KIND_VFAT, only_overlay_tools, NULL), "/usr/sbin/fsck.vfat") == 0, "vfat tool");
    expect(sd_fsck_select_tool(SD_FS_KIND_UNKNOWN, only_overlay_tools, NULL) == NULL, "unknown tool");

    sd_fsck_plan_t plan;
    expect(sd_fsck_plan(SD_FS_KIND_VFAT, "/usr/sbin/fsck.vfat", "/dev/mmcblk0p1", &plan), "vfat plan");
    expect(strcmp(plan.argv[1], "-a") == 0 && strcmp(plan.argv[2], "-w") == 0 &&
               strcmp(plan.argv[3], "/dev/mmcblk0p1") == 0 && plan.argv[4] == NULL,
           "vfat repairs without questions");
    expect(sd_fsck_plan(SD_FS_KIND_EXFAT, "/usr/sbin/fsck.exfat", "/dev/mmcblk0", &plan), "exfat plan");
    expect(strcmp(plan.argv[1], "-y") == 0 && strcmp(plan.argv[2], "/dev/mmcblk0") == 0 && plan.argv[3] == NULL,
           "exfat answers yes");
    expect(sd_fsck_plan(SD_FS_KIND_NTFS, "/usr/sbin/ntfsfix", "/dev/mmcblk0p1", &plan), "ntfs plan");
    expect(strcmp(plan.argv[1], "/dev/mmcblk0p1") == 0 && plan.argv[2] == NULL, "ntfsfix has no interactive flag");
    expect(!sd_fsck_plan(SD_FS_KIND_VFAT, "/usr/sbin/fsck.vfat", "/dev/sda1", &plan), "foreign device rejected");

    expect(sd_fsck_exit_usable(0) && sd_fsck_exit_usable(1) && sd_fsck_exit_usable(2) && sd_fsck_exit_usable(3),
           "corrected filesystems are usable");
    expect(!sd_fsck_exit_usable(4) && !sd_fsck_exit_usable(8) && !sd_fsck_exit_usable(-1) && !sd_fsck_exit_usable(127),
           "uncorrected and operational failures are not usable");

    /* fsck.fat exits 1 after a repair and after die(); only a finished run
     * prints the summary line. */
    const char * finished = "fsck.fat 4.2 (2021-01-31)\nDirty bit is set.\n"
                            "/dev/mmcblk0p1: 120 files, 3000/3900000 clusters\n";
    const char * died = "fsck.fat 4.2 (2021-01-31)\nAllocation of 31207440 bytes failed\n";
    expect(sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 1, finished, "/dev/mmcblk0p1"), "vfat repaired run finished");
    expect(sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 0, finished, "/dev/mmcblk0p1"), "vfat clean run finished");
    expect(!sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 1, died, "/dev/mmcblk0p1"), "vfat die is not a repair");
    expect(!sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 1, NULL, "/dev/mmcblk0p1"), "vfat without output");
    expect(!sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 1, finished, "/dev/mmcblk0"), "summary for another node");
    expect(!sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, 2, finished, "/dev/mmcblk0p1"), "vfat usage error");
    expect(!sd_fsck_tool_succeeded(SD_FS_KIND_VFAT, -1, finished, "/dev/mmcblk0p1"), "vfat killed");
    expect(sd_fsck_tool_succeeded(SD_FS_KIND_EXFAT, 1, NULL, "/dev/mmcblk0") &&
               !sd_fsck_tool_succeeded(SD_FS_KIND_EXFAT, 4, NULL, "/dev/mmcblk0") &&
               !sd_fsck_tool_succeeded(SD_FS_KIND_EXFAT, 8, NULL, "/dev/mmcblk0"),
           "exfat uses the fsck status bits");
    expect(sd_fsck_tool_succeeded(SD_FS_KIND_NTFS, 0, NULL, "/dev/mmcblk0p1") &&
               !sd_fsck_tool_succeeded(SD_FS_KIND_NTFS, 1, NULL, "/dev/mmcblk0p1"),
           "ntfsfix fails with 1");

    expect(sd_fsck_outcome(true, false, false, false) == SD_REPAIR_NOTE_FAILED, "unmounted card failed");
    expect(sd_fsck_outcome(true, false, true, false) == SD_REPAIR_NOTE_REPAIRED, "read-only card repaired");
    expect(sd_fsck_outcome(true, true, true, false) == SD_REPAIR_NOTE_CHECKED, "dirty card checked");
    expect(sd_fsck_outcome(false, true, true, false) == SD_REPAIR_NOTE_NEEDS_COMPUTER, "unfinished check");
    expect(sd_fsck_outcome(false, false, true, false) == SD_REPAIR_NOTE_NEEDS_COMPUTER, "unfinished read-only check");
    expect(sd_fsck_outcome(true, false, true, true) == SD_REPAIR_NOTE_STILL_READONLY, "still read-only");
    expect(sd_fsck_outcome(false, false, true, true) == SD_REPAIR_NOTE_FAILED, "checker failed, still read-only");

    /* Missing checker and low memory share this pre-device-action policy. */
    expect(sd_fsck_skipped_note(false, true) == SD_REPAIR_NOTE_NONE,
           "writable dirty-only skip is diagnostic only");
    expect(sd_fsck_skipped_note(true, false) == SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER,
           "read-only skip still warns");
    expect(sd_fsck_skipped_note(true, true) == SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER,
           "read-only status takes priority if dirty is also reported");
    expect(sd_fsck_skipped_note(false, false) == SD_REPAIR_NOTE_NEEDS_COMPUTER,
           "non-dirty writable skip retains conservative warning");

    /* Boot sector of the 119 GB FAT32 card on the R1 test device. */
    unsigned char sector[512];
    memset(sector, 0, sizeof(sector));
    memcpy(sector + 3, "MSDOS5.0", 8);
    sector[11] = 0x00; sector[12] = 0x02;              /* 512 bytes per sector */
    sector[13] = 0x40;                                 /* 64 sectors per cluster */
    sector[14] = 0x20; sector[15] = 0x00;              /* 32 reserved sectors */
    sector[16] = 2;
    sector[32] = 0xcf; sector[33] = 0x9f; sector[34] = 0xe2; sector[35] = 0x0e; /* total sectors */
    sector[36] = 0x40; sector[37] = 0x77;              /* FAT size in sectors */
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55; sector[511] = 0xAA;
    sd_fs_geometry_t geometry;
    expect(sd_fsck_parse_boot_sector(sector, sizeof(sector), &geometry), "fat32 boot sector");
    expect(geometry.kind == SD_FS_KIND_VFAT && geometry.fat_bits == 32, "fat32 kind");
    expect(geometry.clusters == (0x0ee29fcfULL - 32 - 2ULL * 0x7740) / 64, "fat32 cluster count");
    uint64_t estimate = sd_fsck_memory_estimate(&geometry);
    expect(estimate > 48ULL * 1024 * 1024 && estimate < 64ULL * 1024 * 1024, "large card needs tens of MiB");

    /* A 1 GB FAT16 card fits comfortably. */
    memset(sector + 11, 0, 30);
    sector[11] = 0x00; sector[12] = 0x02;
    sector[13] = 0x40;
    sector[14] = 0x04;
    sector[16] = 2;
    sector[17] = 0x00; sector[18] = 0x02;              /* 512 root entries */
    sector[22] = 0x80; sector[23] = 0x00;              /* 128 sectors per FAT */
    sector[32] = 0x00; sector[33] = 0x00; sector[34] = 0x1e; sector[35] = 0x00;
    sector[36] = 0; sector[37] = 0;
    expect(sd_fsck_parse_boot_sector(sector, sizeof(sector), &geometry) && geometry.fat_bits == 16,
           "fat16 boot sector");
    expect(sd_fsck_memory_estimate(&geometry) < 3ULL * 1024 * 1024, "small card estimate");

    unsigned char exfat[512];
    memset(exfat, 0, sizeof(exfat));
    memcpy(exfat + 3, "EXFAT   ", 8);
    exfat[92] = 0x00; exfat[93] = 0x00; exfat[94] = 0x10; exfat[95] = 0x00; /* 1M clusters */
    exfat[510] = 0x55; exfat[511] = 0xAA;
    expect(sd_fsck_parse_boot_sector(exfat, sizeof(exfat), &geometry) && geometry.kind == SD_FS_KIND_EXFAT &&
               geometry.clusters == 0x100000,
           "exfat boot sector");
    exfat[510] = 0;
    expect(!sd_fsck_parse_boot_sector(exfat, sizeof(exfat), &geometry), "missing boot signature");

    char stamp[32];
    const char * boot_log =
        "<6>[    1.901000] mmc0: new high speed SDIO card at address 0001\n"
        "<6>[    1.957907] mmc1: new high speed SDXC card at address aaaa\n"
        "<4>[    4.173249] FAT-fs (mmcblk0p1): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.\n";
    expect(sd_fsck_klog_dirty(boot_log, "mmcblk0p1", stamp, sizeof(stamp)), "dirty after insertion");
    expect(strcmp(stamp, "1.957907") == 0, "insertion stamp");
    expect(!sd_fsck_klog_dirty(boot_log, "mmcblk0", stamp, sizeof(stamp)), "other node is not dirty");
    const char * swapped =
        "<6>[    1.957907] mmc1: new high speed SDXC card at address aaaa\n"
        "<4>[    4.173249] FAT-fs (mmcblk0p1): Volume was not properly unmounted. Some data may be corrupt.\n"
        "<6>[  300.000001] mmc1: new high speed SDHC card at address 1234\n"
        "<6>[  300.100000] mmc0: new high speed SDIO card at address 0001\n";
    expect(!sd_fsck_klog_dirty(swapped, "mmcblk0p1", stamp, sizeof(stamp)), "warning for the previous card");
    expect(strcmp(stamp, "300.000001") == 0, "SDIO insertion does not replace the card stamp");
    expect(sd_fsck_klog_dirty("FAT-fs (mmcblk0p1): Volume was not properly unmounted.\n", "mmcblk0p1", stamp,
                              sizeof(stamp)) &&
               strcmp(stamp, "none") == 0,
           "insertion line rotated out of the log");

    char key[SD_FSCK_ATTEMPT_BYTES];
    sd_repair_attempt_key("/dev/mmcblk0p1", "abcd", key, sizeof(key));
    expect(strcmp(key, "cid:abcd") == 0, "card id identifies an attempt");
    sd_repair_attempt_key("/dev/mmcblk0p1", "", key, sizeof(key));
    expect(strcmp(key, "dev:/dev/mmcblk0p1") == 0, "block node identifies an attempt without a card id");
    sd_repair_fat_attempt_key("1.957907", false, key, sizeof(key));
    expect(strcmp(key, "fat@1.957907:dirty") == 0, "writable dirty trigger has its own FAT attempt");
    char dirty_key[SD_FSCK_ATTEMPT_BYTES];
    snprintf(dirty_key, sizeof(dirty_key), "%s", key);
    sd_repair_fat_attempt_key("1.957907", true, key, sizeof(key));
    expect(strcmp(key, "fat@1.957907:ro") == 0 && strcmp(key, dirty_key) != 0,
           "read-only trigger remains available after a dirty attempt");
    sd_repair_fat_attempt_key("pending", false, key, sizeof(key));
    expect(strcmp(key, "fat@pending:dirty") == 0, "pending FAT attempt preserves trigger identity");
    char pending_dirty_key[SD_FSCK_ATTEMPT_BYTES];
    snprintf(pending_dirty_key, sizeof(pending_dirty_key), "%s", key);
    sd_repair_fat_attempt_key("pending", true, key, sizeof(key));
    expect(strcmp(key, "fat@pending:ro") == 0 && strcmp(key, pending_dirty_key) != 0,
           "pending read-only attempt transfers independently from dirty attempt");

    if (failures) {
        fprintf(stderr, "sd fsck: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("sd fsck: PASS");
    return 0;
}
