/* Filesystem detection, labels, mount options, partition watching, the slot
 * table (mount, unplug, replug) and the shared storage roots list. */
#define USB_DRIVE_SYS_BLOCK "sys_block"
#define USB_DRIVE_PROC_MOUNTS "proc_mounts"
#define USB_DRIVE_DEV_DIR "dev"
#define USB_DRIVE_MOUNT_ROOT "mnt"
#include "../usb_drive.c"
#include "../storage_roots.c"
#include <assert.h>

/* The tests never run the worker, so nothing may spawn a command. */
bool subprocess_run_timeout(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms) {
    (void) argv; (void) out_buf; (void) out_buf_size; (void) timeout_ms;
    assert(!"subprocess must not run in tests");
    return false;
}

static void le16(unsigned char * s, size_t at, unsigned v) { s[at] = (unsigned char) v; s[at + 1] = (unsigned char) (v >> 8); }
static void le32(unsigned char * s, size_t at, unsigned long v) { le16(s, at, (unsigned) (v & 0xFFFF)); le16(s, at + 2, (unsigned) (v >> 16)); }

static void fat32_sector(unsigned char * s, const char * label) {
    memset(s, 0, 512);
    memcpy(s + 3, "MSDOS5.0", 8);
    le16(s, 11, 512); s[13] = 8; le16(s, 14, 32); s[16] = 2;
    le32(s, 32, 2000000); le32(s, 36, 2000);
    s[66] = 0x29; memcpy(s + 71, label, 11); memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void fat16_sector(unsigned char * s, const char * label) {
    memset(s, 0, 512);
    memcpy(s + 3, "MSDOS5.0", 8);
    le16(s, 11, 512); s[13] = 4; le16(s, 14, 1); s[16] = 2; le16(s, 17, 512);
    le16(s, 19, 60000); le16(s, 22, 64);
    s[38] = 0x29; memcpy(s + 43, label, 11); memcpy(s + 54, "FAT16   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void exfat_sector(unsigned char * s) {
    memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x76; s[2] = 0x90;
    memcpy(s + 3, "EXFAT   ", 8);
    le32(s, 92, 1000);
    s[510] = 0x55; s[511] = 0xAA;
}

static void ntfs_sector(unsigned char * s) {
    memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x52; s[2] = 0x90;
    memcpy(s + 3, "NTFS    ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void test_detect_fs(void) {
    unsigned char s[512];
    fat32_sector(s, "MUSIC      "); assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_VFAT);
    fat16_sector(s, "MUSIC      "); assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_VFAT);
    exfat_sector(s); assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_EXFAT);
    ntfs_sector(s); assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_NTFS);
    /* A partition table sector and blank media are not filesystems. */
    memset(s, 0, sizeof(s)); s[510] = 0x55; s[511] = 0xAA; s[446] = 0x80; s[450] = 0x0C;
    assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_UNKNOWN);
    memset(s, 0, sizeof(s)); assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_UNKNOWN);
    ntfs_sector(s); s[511] = 0; assert(usb_drive_detect_fs(s, sizeof(s)) == SD_FS_KIND_UNKNOWN);
    assert(usb_drive_detect_fs(s, 100) == SD_FS_KIND_UNKNOWN);
    assert(usb_drive_detect_fs(NULL, 512) == SD_FS_KIND_UNKNOWN);
}

static void test_labels(void) {
    char label[USB_DRIVE_LABEL_BYTES];
    assert(usb_drive_label_from_blkid("/dev/sda1: LABEL=\"MUSIC\" UUID=\"427E-289A\"\n", label, sizeof(label)));
    assert(strcmp(label, "MUSIC") == 0);
    assert(usb_drive_label_from_blkid("/dev/sda1: UUID=\"1\" LABEL=\"My Disk\" TYPE=\"vfat\"\n", label, sizeof(label)));
    assert(strcmp(label, "My Disk") == 0);
    assert(usb_drive_label_from_blkid("LABEL=\"Música ñ\"", label, sizeof(label)));
    assert(strcmp(label, "Música ñ") == 0);
    assert(!usb_drive_label_from_blkid("/dev/sda1: UUID=\"427E-289A\"\n", label, sizeof(label)));
    assert(!usb_drive_label_from_blkid("/dev/sda1: PARTLABEL=\"data\"\n", label, sizeof(label)));
    assert(!usb_drive_label_from_blkid("/dev/sda1: LABEL=\"\" UUID=\"1\"\n", label, sizeof(label)));
    assert(!usb_drive_label_from_blkid("/dev/sda1: LABEL=\"   \"\n", label, sizeof(label)));
    assert(!usb_drive_label_from_blkid("LABEL=\"unterminated", label, sizeof(label)));
    assert(!usb_drive_label_from_blkid("", label, sizeof(label)));
    char small[6];
    assert(usb_drive_label_from_blkid("LABEL=\"A long volume label\"", small, sizeof(small)));
    assert(strlen(small) == 5);

    unsigned char s[512];
    fat32_sector(s, "MUSIC      ");
    assert(usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)) && strcmp(label, "MUSIC") == 0);
    fat16_sector(s, "BACKUP DISK");
    assert(usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)) && strcmp(label, "BACKUP DISK") == 0);
    fat32_sector(s, "NO NAME    ");
    assert(!usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)));
    fat32_sector(s, "\xE9 caf\xE9    ");
    assert(!usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)));
    fat32_sector(s, "MUSIC      "); s[66] = 0;
    assert(!usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)));
    exfat_sector(s);
    assert(!usb_drive_label_from_boot_sector(s, sizeof(s), label, sizeof(label)));
}

static void test_mount_plan(void) {
    usb_drive_mount_plan_t plan;
    assert(usb_drive_mount_plan(SD_FS_KIND_VFAT, true, "/dev/sda1", "/data/mnt/usb_0", 0, &plan));
    assert(strcmp(plan.argv[0], "mount") == 0 && strcmp(plan.argv[1], "-t") == 0 && strcmp(plan.argv[2], "vfat") == 0);
    assert(strcmp(plan.argv[3], "-o") == 0 && strcmp(plan.argv[5], "/dev/sda1") == 0 &&
           strcmp(plan.argv[6], "/data/mnt/usb_0") == 0 && plan.argv[7] == NULL);
    assert(strstr(plan.options, "iocharset=utf8") && strstr(plan.options, "shortname=mixed") &&
           strstr(plan.options, "errors=remount-ro") && strstr(plan.options, "noatime"));
    assert(!strstr(plan.options, "sync"));
    assert(usb_drive_mount_plan(SD_FS_KIND_VFAT, true, "/dev/sda1", "/data/mnt/usb_0", 1, &plan));
    assert(!strstr(plan.options, "iocharset") && strstr(plan.options, "rw"));
    assert(!usb_drive_mount_plan(SD_FS_KIND_VFAT, true, "/dev/sda1", "/data/mnt/usb_0", 2, &plan));

    assert(usb_drive_mount_plan(SD_FS_KIND_EXFAT, true, "/dev/sdb1", "/data/mnt/usb_1", 0, &plan));
    assert(strcmp(plan.argv[2], "exfat") == 0 && strstr(plan.options, "iocharset=utf8") && !strstr(plan.options, "sync"));
    assert(usb_drive_mount_plan(SD_FS_KIND_EXFAT, true, "/dev/sdb1", "/data/mnt/usb_1", 1, &plan));
    assert(!strstr(plan.options, "iocharset"));

    /* ntfs-3g when it is installed, read-only as the fallback. */
    assert(usb_drive_mount_plan(SD_FS_KIND_NTFS, true, "/dev/sdc1", "/data/mnt/usb_2", 0, &plan));
    assert(strcmp(plan.argv[0], "/usr/bin/ntfs-3g") == 0 && strcmp(plan.argv[1], "-o") == 0 &&
           strncmp(plan.options, "rw,", 3) == 0 && strcmp(plan.argv[3], "/dev/sdc1") == 0 &&
           strcmp(plan.argv[4], "/data/mnt/usb_2") == 0 && plan.argv[5] == NULL);
    assert(usb_drive_mount_plan(SD_FS_KIND_NTFS, true, "/dev/sdc1", "/data/mnt/usb_2", 1, &plan));
    assert(strncmp(plan.options, "ro,", 3) == 0);
    assert(!usb_drive_mount_plan(SD_FS_KIND_NTFS, true, "/dev/sdc1", "/data/mnt/usb_2", 2, &plan));
    /* No ntfs-3g: the kernel driver. */
    assert(usb_drive_mount_plan(SD_FS_KIND_NTFS, false, "/dev/sdc1", "/data/mnt/usb_2", 0, &plan));
    assert(strcmp(plan.argv[0], "mount") == 0 && strcmp(plan.argv[2], "ntfs") == 0 && strstr(plan.options, "nls=utf8"));
    assert(!usb_drive_mount_plan(SD_FS_KIND_UNKNOWN, true, "/dev/sdd1", "/data/mnt/usb_3", 0, &plan));
}

static void test_mount_table(void) {
    const char * mounts =
        "/dev/root / squashfs ro,relatime 0 0\n"
        "/dev/mmcblk0p1 /usr/data/mnt/sd_0 vfat rw,relatime 0 0\n"
        "/dev/sda1 /tmp/mass_storage/sda1 vfat rw,sync 0 0\n"
        "/dev/sdb1 /usr/data/mnt/usb_1 fuseblk rw,nosuid 0 0\n"
        "/dev/sdc1 /tmp/mass_storage/sdc1 exfat rw 0 0\n"
        "broken line\n"
        "/dev/sdd1 /tmp/mass_storage/with\\040space vfat rw 0 0";
    char found[96];
    assert(usb_drive_stock_mount_point(mounts, "/dev/sda1", found, sizeof(found)) &&
           strcmp(found, "/tmp/mass_storage/sda1") == 0);
    assert(usb_drive_stock_mount_point(mounts, "/dev/sdd1", found, sizeof(found)) &&
           strcmp(found, "/tmp/mass_storage/with space") == 0);
    /* The player's own mount is not the stock one. */
    assert(!usb_drive_stock_mount_point(mounts, "/dev/sdb1", found, sizeof(found)));
    assert(!usb_drive_stock_mount_point(mounts, "/dev/sde1", found, sizeof(found)));
    assert(!usb_drive_stock_mount_point(mounts, "/dev/sda1", found, 8));
    assert(!usb_drive_stock_mount_point(NULL, "/dev/sda1", found, sizeof(found)));

    const char * cursor = mounts;
    sd_mount_entry_t entry;
    size_t lines = 0;
    while (sd_mounts_next(&cursor, &entry)) lines++;
    assert(lines == 6); /* the broken line is skipped */
    sd_mount_info_t sd;
    assert(sd_fsck_parse_mounts(mounts, &sd) && strcmp(sd.device, "/dev/mmcblk0p1") == 0 && !sd.readonly);
}

static void put(const char * path, const char * text) {
    FILE * f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static void make_disk(const char * bus_dir, const char * disk, const char * size) {
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", bus_dir, disk);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/%s/size", bus_dir, disk);
    put(path, size);
    snprintf(path, sizeof(path), "../%s/%s", bus_dir, disk);
    char link_path[200];
    snprintf(link_path, sizeof(link_path), "%s/%s", USB_DRIVE_SYS_BLOCK, disk);
    assert(symlink(path, link_path) == 0);
}

static void make_part(const char * bus_dir, const char * disk, const char * part, const char * size) {
    char path[200];
    snprintf(path, sizeof(path), "%s/%s/%s", bus_dir, disk, part);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/%s/%s/size", bus_dir, disk, part);
    put(path, size);
}

static bool has_device(char (* names)[USB_DRIVE_DEVICE_BYTES], size_t count, const char * name) {
    return listed((const char (*)[USB_DRIVE_DEVICE_BYTES]) names, count, name);
}

static void test_scan_partitions(void) {
    assert(mkdir(USB_DRIVE_SYS_BLOCK, 0700) == 0);
    assert(mkdir("devices", 0700) == 0);
    assert(mkdir("devices/usb1", 0700) == 0);
    assert(mkdir("devices/pci", 0700) == 0);
    /* The sysfs links point at ../devices/<bus>/<disk>. */
    make_disk("devices/usb1", "sda", "1000\n");
    make_part("devices/usb1", "sda", "sda1", "400\n");
    make_part("devices/usb1", "sda", "sda2", "500\n");
    make_part("devices/usb1", "sda", "sda3", "0\n");   /* no media */
    make_disk("devices/usb1", "sdb", "900\n");          /* no partition table */
    make_disk("devices/usb1", "sdd", "0\n");            /* empty card reader slot */
    make_disk("devices/pci", "sdc", "900\n");           /* not on a USB bus */
    make_part("devices/pci", "sdc", "sdc1", "400\n");
    assert(mkdir("sys_block/mmcblk0", 0700) == 0);
    assert(mkdir("sys_block/sdaa", 0700) == 0);

    char names[USB_DRIVE_MAX][USB_DRIVE_DEVICE_BYTES];
    size_t count = scan_partitions(names, USB_DRIVE_MAX);
    assert(count == 3);
    assert(has_device(names, count, "sda1") && has_device(names, count, "sda2") && has_device(names, count, "sdb"));
    assert(!has_device(names, count, "sda") && !has_device(names, count, "sdc1") && !has_device(names, count, "sdd"));
    assert(scan_partitions(names, 2) == 2);
}

typedef char names_t[USB_DRIVE_DEVICE_BYTES];

static size_t slots_in_state(const usb_slot_t * slots, usb_slot_state_t state) {
    size_t n = 0;
    for (size_t i = 0; i < USB_DRIVE_MAX; i++) n += slots[i].state == state;
    return n;
}

static void test_reconcile(void) {
    usb_slot_t slots[USB_DRIVE_MAX];
    memset(slots, 0, sizeof(slots));
    usb_drive_events_t ev;
    names_t two[] = { "sda1", "sda2" };

    /* First sighting waits one poll so a partition scan can finish. */
    usb_drive_reconcile(slots, two, 2, &ev);
    assert(slots_in_state(slots, USB_SLOT_SETTLING) == 2 && !ev.changed && !ev.mounted);
    assert(strcmp(slots[0].drive.mount_point, "mnt/usb_0") == 0 && strcmp(slots[1].drive.mount_point, "mnt/usb_1") == 0);
    usb_drive_reconcile(slots, two, 2, &ev);
    assert(slots_in_state(slots, USB_SLOT_QUEUED) == 2);

    /* The worker mounted the first and could not mount the second. */
    slots[0].state = USB_SLOT_MOUNTED; strcpy(slots[0].drive.label, "MUSIC");
    slots[1].state = USB_SLOT_FAILED;
    usb_drive_reconcile(slots, two, 2, &ev);
    assert(ev.mounted && ev.failed && ev.changed && ev.removed_count == 0);
    usb_drive_reconcile(slots, two, 2, &ev);
    assert(!ev.mounted && !ev.failed && !ev.changed);

    /* The list of mounted drives, as the screens see it. */
    usb_drive_t listed_drives[USB_DRIVE_MAX];
    size_t n = 0;
    for (size_t i = 0; i < USB_DRIVE_MAX; i++) if (slots[i].state == USB_SLOT_MOUNTED) listed_drives[n++] = slots[i].drive;
    assert(n == 1 && strcmp(listed_drives[0].label, "MUSIC") == 0);

    /* Unplug: the mount point is reported, the slot is detached by the worker. */
    names_t only_failed[] = { "sda2" };
    usb_drive_reconcile(slots, only_failed, 1, &ev);
    assert(ev.removed_count == 1 && strcmp(ev.removed[0], "mnt/usb_0") == 0 && ev.changed);
    assert(slots[0].state == USB_SLOT_UNMOUNTING && slots[1].state == USB_SLOT_FAILED);
    usb_drive_reconcile(slots, only_failed, 1, &ev);
    assert(ev.removed_count == 0 && !ev.changed && slots[0].state == USB_SLOT_UNMOUNTING);

    /* Replugged while the old mount is still being detached: another slot. */
    names_t back[] = { "sda1", "sda2" };
    usb_drive_reconcile(slots, back, 2, &ev);
    assert(slots[0].state == USB_SLOT_UNMOUNTING && slots[2].state == USB_SLOT_SETTLING &&
           strcmp(slots[2].drive.device, "sda1") == 0 && strcmp(slots[2].drive.mount_point, "mnt/usb_2") == 0);
    memset(&slots[0], 0, sizeof(slots[0])); /* the worker finished */

    /* Everything unplugged: unmounted drives report, failed ones just vanish. */
    slots[2].state = USB_SLOT_MOUNTED; slots[2].announced = true;
    usb_drive_reconcile(slots, NULL, 0, &ev);
    assert(ev.removed_count == 1 && strcmp(ev.removed[0], "mnt/usb_2") == 0);
    assert(slots[1].state == USB_SLOT_FREE && slots[2].state == USB_SLOT_UNMOUNTING);
}

static void test_reconcile_edges(void) {
    usb_slot_t slots[USB_DRIVE_MAX];
    memset(slots, 0, sizeof(slots));
    usb_drive_events_t ev;

    /* sda showed up before its partitions: it never gets mounted as a whole. */
    names_t disk[] = { "sda" };
    usb_drive_reconcile(slots, disk, 1, &ev);
    names_t parts[] = { "sda1", "sda2" };
    usb_drive_reconcile(slots, parts, 2, &ev);
    assert(slots_in_state(slots, USB_SLOT_QUEUED) == 0 && slots_in_state(slots, USB_SLOT_SETTLING) == 2);
    assert(strcmp(slots[0].drive.device, "sda1") == 0 && strcmp(slots[1].drive.device, "sda2") == 0);

    /* Pulled while the worker is mounting: flagged, detached when it returns. */
    usb_drive_reconcile(slots, parts, 2, &ev);
    slots[1].state = USB_SLOT_MOUNTING;
    usb_drive_reconcile(slots, NULL, 0, &ev);
    assert(slots[1].gone && slots[1].state == USB_SLOT_MOUNTING && ev.removed_count == 0 && !ev.changed);
    assert(slots_in_state(slots, USB_SLOT_FREE) == USB_DRIVE_MAX - 1);

    /* More partitions than slots: the extra ones wait. */
    memset(slots, 0, sizeof(slots));
    names_t many[] = { "sda1", "sda2", "sda3", "sda4", "sda5" };
    usb_drive_reconcile(slots, many, 5, &ev);
    assert(slots_in_state(slots, USB_SLOT_SETTLING) == USB_DRIVE_MAX);
}

static void test_storage_roots(void) {
    usb_drive_t drives[2];
    memset(drives, 0, sizeof(drives));
    strcpy(drives[0].mount_point, "/data/mnt/usb_0"); strcpy(drives[0].label, "MUSIC");
    strcpy(drives[1].mount_point, "/data/mnt/usb_2");
    storage_root_t roots[STORAGE_ROOTS_MAX];

    /* No drive: just the SD card. */
    assert(storage_roots_build("/data/mnt/sd_0", drives, 0, roots, STORAGE_ROOTS_MAX) == 1);
    assert(strcmp(roots[0].id, "sd") == 0 && !roots[0].removable);
    assert(strcmp(storage_root_name(&roots[0], "SD card", "USB storage"), "SD card") == 0);

    size_t count = storage_roots_build("/data/mnt/sd_0", drives, 2, roots, STORAGE_ROOTS_MAX);
    assert(count == 3);
    assert(strcmp(roots[1].id, "usb0") == 0 && strcmp(roots[1].path, "/data/mnt/usb_0") == 0 && roots[1].removable);
    assert(strcmp(roots[2].id, "usb2") == 0);
    assert(strcmp(storage_root_name(&roots[1], "SD card", "USB storage"), "MUSIC") == 0);
    assert(strcmp(storage_root_name(&roots[2], "SD card", "USB storage"), "USB storage") == 0);
    assert(storage_roots_build("/data/mnt/sd_0", drives, 2, roots, 2) == 2);

    count = storage_roots_build("/data/mnt/sd_0", drives, 2, roots, STORAGE_ROOTS_MAX);
    size_t at = 99;
    assert(storage_roots_find_path(roots, count, "/data/mnt/usb_0/Albums/a.flac", &at) && at == 1);
    assert(storage_roots_find_path(roots, count, "/data/mnt/usb_2", &at) && at == 2);
    assert(storage_roots_find_path(roots, count, "/data/mnt/sd_0/x", &at) && at == 0);
    assert(!storage_roots_find_path(roots, count, "/data/mnt/usb_00/x", NULL));
    assert(!storage_roots_find_path(roots, count, "/data/mnt/usb_1/x", NULL));
    assert(storage_roots_find_id(roots, count, "usb2", &at) && at == 2);
    assert(!storage_roots_find_id(roots, count, "usb1", NULL));
    assert(!storage_roots_find_id(roots, count, NULL, NULL));
}

int main(void) {
    test_detect_fs();
    test_labels();
    test_mount_plan();
    test_mount_table();
    test_scan_partitions();
    test_reconcile();
    test_reconcile_edges();
    test_storage_roots();
    puts("USB drive tests passed");
    return 0;
}
