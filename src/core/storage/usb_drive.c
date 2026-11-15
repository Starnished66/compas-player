#include "usb_drive.h"
#include "../time/monotonic.h"

#include "../process/subprocess.h"
#include "../text/utf8_util.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef USB_DRIVE_SYS_BLOCK
#define USB_DRIVE_SYS_BLOCK "/sys/block"
#endif
#ifndef USB_DRIVE_PROC_MOUNTS
#define USB_DRIVE_PROC_MOUNTS "/proc/mounts"
#endif
#ifndef USB_DRIVE_DEV_DIR
#define USB_DRIVE_DEV_DIR "/dev"
#endif
#ifndef USB_DRIVE_MOUNT_ROOT
#define USB_DRIVE_MOUNT_ROOT "/data/mnt"
#endif
#ifndef USB_DRIVE_NTFS3G
#define USB_DRIVE_NTFS3G "/usr/bin/ntfs-3g"
#endif

#define USB_DRIVE_POLL_MS 1000
#define USB_DRIVE_MOUNT_TIMEOUT_MS 30000

/* ---- Filesystem and label detection ---- */

sd_fs_kind_t usb_drive_detect_fs(const unsigned char * sector, size_t length) {
    if (!sector || length < 512 || sector[510] != 0x55 || sector[511] != 0xAA) return SD_FS_KIND_UNKNOWN;
    if (memcmp(sector + 3, "NTFS    ", 8) == 0) return SD_FS_KIND_NTFS;
    sd_fs_geometry_t geometry;
    if (sd_fsck_parse_boot_sector(sector, length, &geometry)) return geometry.kind;
    return SD_FS_KIND_UNKNOWN;
}

/* Keeps a label that is safe to show: no control bytes, trimmed. */
static bool store_label(const char * text, size_t length, char * out, size_t out_size) {
    char clean[USB_DRIVE_LABEL_BYTES];
    size_t n = 0;
    for (size_t i = 0; i < length && n + 1 < sizeof(clean); i++)
        clean[n++] = (unsigned char) text[i] < 0x20 ? ' ' : text[i];
    while (n && clean[n - 1] == ' ') n--;
    size_t start = 0;
    while (start < n && clean[start] == ' ') start++;
    if (start == n) return false;
    utf8_truncate_safe_bounded(out, out_size, clean + start, n - start);
    return out[0] != '\0';
}

bool usb_drive_label_from_blkid(const char * text, char * out, size_t out_size) {
    if (out_size) out[0] = '\0';
    if (!text || !out_size) return false;
    const char * cursor = text;
    while ((cursor = strstr(cursor, "LABEL=\"")) != NULL) {
        /* PARTLABEL= is the partition table's name, not the volume label. */
        bool own = cursor == text || cursor[-1] == ' ' || cursor[-1] == '\n' || cursor[-1] == ':';
        cursor += 7;
        if (!own) continue;
        const char * end = strchr(cursor, '"');
        if (!end) return false;
        return store_label(cursor, (size_t) (end - cursor), out, out_size);
    }
    return false;
}

bool usb_drive_label_from_boot_sector(const unsigned char * sector, size_t length, char * out, size_t out_size) {
    if (out_size) out[0] = '\0';
    sd_fs_geometry_t geometry;
    if (!out_size || !sd_fsck_parse_boot_sector(sector, length, &geometry) || geometry.kind != SD_FS_KIND_VFAT)
        return false;
    size_t offset = geometry.fat_bits == 32 ? 71 : 43;
    /* 0x29 marks the extended boot record that holds the label. */
    if (sector[offset - 5] != 0x29) return false;
    char raw[11];
    memcpy(raw, sector + offset, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++)
        if ((unsigned char) raw[i] < 0x20 || (unsigned char) raw[i] > 0x7E) return false;
    if (memcmp(raw, "NO NAME    ", sizeof(raw)) == 0) return false;
    return store_label(raw, sizeof(raw), out, out_size);
}

/* ---- Mounting ---- */

bool usb_drive_mount_plan(sd_fs_kind_t kind, bool ntfs3g_present, const char * device_path,
                          const char * mount_point, unsigned attempt, usb_drive_mount_plan_t * out) {
    if (!out || !device_path || !mount_point || attempt >= USB_DRIVE_ATTEMPT_MAX) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->device, sizeof(out->device), "%s", device_path);
    snprintf(out->mount_point, sizeof(out->mount_point), "%s", mount_point);
    const char * fstype = NULL;
    const char * safe = "nodev,nosuid,noexec";
    if (kind == SD_FS_KIND_VFAT) {
        fstype = "vfat";
        if (attempt == 0)
            snprintf(out->options, sizeof(out->options),
                     "rw,noatime,%s,fmask=0022,dmask=0022,iocharset=utf8,shortname=mixed,errors=remount-ro", safe);
        else snprintf(out->options, sizeof(out->options), "rw,noatime,%s", safe);
    } else if (kind == SD_FS_KIND_EXFAT) {
        fstype = "exfat";
        if (attempt == 0) snprintf(out->options, sizeof(out->options), "rw,noatime,%s,iocharset=utf8", safe);
        else snprintf(out->options, sizeof(out->options), "rw,noatime,%s", safe);
    } else if (kind == SD_FS_KIND_NTFS) {
        const char * mode = attempt == 0 ? "rw" : "ro";
        if (ntfs3g_present) snprintf(out->options, sizeof(out->options), "%s,noatime,big_writes,umask=0022", mode);
        else {
            fstype = "ntfs";
            snprintf(out->options, sizeof(out->options), "%s,noatime,%s,nls=utf8,umask=0022", mode, safe);
        }
    } else {
        return false;
    }
    size_t n = 0;
    if (kind == SD_FS_KIND_NTFS && ntfs3g_present) {
        snprintf(out->tool, sizeof(out->tool), "%s", USB_DRIVE_NTFS3G);
        out->argv[n++] = out->tool;
    } else {
        snprintf(out->tool, sizeof(out->tool), "mount");
        out->argv[n++] = out->tool;
        out->argv[n++] = (char *) "-t";
        out->argv[n++] = (char *) fstype;
    }
    out->argv[n++] = (char *) "-o";
    out->argv[n++] = out->options;
    out->argv[n++] = out->device;
    out->argv[n++] = out->mount_point;
    out->argv[n] = NULL;
    return true;
}

bool usb_drive_stock_mount_point(const char * mounts_text, const char * device_path, char * out, size_t out_size) {
    if (!mounts_text || !device_path || !out || !out_size) return false;
    const char * cursor = mounts_text;
    sd_mount_entry_t entry;
    const size_t dir_len = sizeof(USB_DRIVE_STOCK_MOUNT_DIR) - 1;
    while (sd_mounts_next(&cursor, &entry)) {
        if (strcmp(entry.device, device_path) != 0) continue;
        if (strncmp(entry.mount_point, USB_DRIVE_STOCK_MOUNT_DIR "/", dir_len + 1) != 0) continue;
        if (strlen(entry.mount_point) >= out_size) return false;
        strcpy(out, entry.mount_point);
        return true;
    }
    return false;
}

/* ---- Slot table ---- */

static bool listed(const char (* present)[USB_DRIVE_DEVICE_BYTES], size_t count, const char * device) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(present[i], device) == 0) return true;
    return false;
}

static bool slot_active(const usb_slot_t * slot) {
    return slot->state != USB_SLOT_FREE && slot->state != USB_SLOT_UNMOUNTING;
}

/* A device that is no longer listed. */
static void slot_device_gone(usb_slot_t * slot, usb_drive_events_t * events) {
    switch (slot->state) {
    case USB_SLOT_MOUNTING:
        slot->gone = true; /* the worker detaches it when the mount returns */
        break;
    case USB_SLOT_MOUNTED:
        snprintf(events->removed[events->removed_count++], USB_DRIVE_MOUNT_BYTES, "%s", slot->drive.mount_point);
        slot->state = USB_SLOT_UNMOUNTING;
        events->changed = true;
        break;
    default:
        memset(slot, 0, sizeof(*slot));
        break;
    }
}

void usb_drive_reconcile(usb_slot_t slots[USB_DRIVE_MAX], const char (* present)[USB_DRIVE_DEVICE_BYTES],
                         size_t present_count, usb_drive_events_t * events) {
    memset(events, 0, sizeof(*events));
    for (size_t i = 0; i < USB_DRIVE_MAX; i++) {
        usb_slot_t * slot = &slots[i];
        if (!slot_active(slot)) continue;
        if (!listed(present, present_count, slot->drive.device)) slot_device_gone(slot, events);
        else if (slot->state == USB_SLOT_SETTLING) slot->state = USB_SLOT_QUEUED;
    }
    for (size_t p = 0; p < present_count; p++) {
        bool tracked = false;
        size_t free_index = USB_DRIVE_MAX;
        for (size_t i = 0; i < USB_DRIVE_MAX; i++) {
            if (slot_active(&slots[i]) && strcmp(slots[i].drive.device, present[p]) == 0) tracked = true;
            if (free_index == USB_DRIVE_MAX && slots[i].state == USB_SLOT_FREE) free_index = i;
        }
        if (tracked || free_index == USB_DRIVE_MAX) continue;
        usb_slot_t * free_slot = &slots[free_index];
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->state = USB_SLOT_SETTLING;
        snprintf(free_slot->drive.device, sizeof(free_slot->drive.device), "%s", present[p]);
        snprintf(free_slot->drive.mount_point, sizeof(free_slot->drive.mount_point),
                 USB_DRIVE_MOUNT_ROOT "/usb_%zu", free_index);
    }
    for (size_t i = 0; i < USB_DRIVE_MAX; i++) {
        usb_slot_t * slot = &slots[i];
        if (slot->announced) continue;
        if (slot->state == USB_SLOT_MOUNTED) events->mounted = events->changed = true;
        else if (slot->state == USB_SLOT_FAILED) events->failed = true;
        else continue;
        slot->announced = true;
    }
}

/* ---- Watching sysfs ---- */

static long read_number(const char * path) {
    FILE * file = fopen(path, "r");
    long value = -1;
    if (!file) return -1;
    if (fscanf(file, "%ld", &value) != 1) value = -1;
    fclose(file);
    return value;
}

/* A drive name such as sda, and a partition name such as sda1. */
static bool is_disk_name(const char * name) {
    return name[0] == 's' && name[1] == 'd' && name[2] >= 'a' && name[2] <= 'z' && name[3] == '\0';
}

static bool is_partition_of(const char * disk, const char * name) {
    size_t length = strlen(disk);
    if (strncmp(name, disk, length) != 0 || !isdigit((unsigned char) name[length])) return false;
    for (const char * p = name + length; *p; p++)
        if (!isdigit((unsigned char) *p)) return false;
    return true;
}

/* Only a disk that hangs off a USB bus, with media in it. */
static bool disk_is_usb_media(const char * disk) {
    char path[320], target[256];
    snprintf(path, sizeof(path), "%s/%s", USB_DRIVE_SYS_BLOCK, disk);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    if (!strstr(target, "/usb")) return false;
    snprintf(path, sizeof(path), "%s/%s/size", USB_DRIVE_SYS_BLOCK, disk);
    return read_number(path) > 0;
}

static bool store_device_name(char * out, const char * name) {
    if (strlen(name) >= USB_DRIVE_DEVICE_BYTES) return false;
    strcpy(out, name);
    return true;
}

/* Partitions of every USB disk, or the disk itself when it has none. */
static size_t scan_partitions(char (* out)[USB_DRIVE_DEVICE_BYTES], size_t max) {
    size_t count = 0;
    DIR * block = opendir(USB_DRIVE_SYS_BLOCK);
    if (!block) return 0;
    struct dirent * disk;
    while (count < max && (disk = readdir(block)) != NULL) {
        if (!is_disk_name(disk->d_name) || !disk_is_usb_media(disk->d_name)) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", USB_DRIVE_SYS_BLOCK, disk->d_name);
        DIR * parts = opendir(path);
        size_t before = count;
        struct dirent * part;
        while (parts && count < max && (part = readdir(parts)) != NULL) {
            if (!is_partition_of(disk->d_name, part->d_name)) continue;
            char size_path[640];
            snprintf(size_path, sizeof(size_path), "%s/%s/size", path, part->d_name);
            if (read_number(size_path) > 0 && store_device_name(out[count], part->d_name)) count++;
        }
        if (parts) closedir(parts);
        if (count == before && count < max && store_device_name(out[count], disk->d_name)) count++;
    }
    closedir(block);
    return count;
}

/* ---- Worker ---- */

static pthread_mutex_t drive_mutex = PTHREAD_MUTEX_INITIALIZER;
static usb_slot_t drive_slots[USB_DRIVE_MAX];
static bool worker_running;

static size_t read_text(const char * path, char * out, size_t out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t total = 0;
    ssize_t n;
    while (total + 1 < out_size && (n = read(fd, out + total, out_size - 1 - total)) > 0) total += (size_t) n;
    close(fd);
    out[total] = '\0';
    return total;
}

static void run_quiet(char * const argv[]) {
    subprocess_run_timeout(argv, NULL, 0, USB_DRIVE_MOUNT_TIMEOUT_MS);
}

static void detach_mount(const char * mount_point) {
    char * argv[] = { (char *) "umount", (char *) "-l", (char *) mount_point, NULL };
    run_quiet(argv);
}

static bool mount_point_attached(const char * mount_point) {
    struct stat point, parent;
    return stat(mount_point, &point) == 0 && stat(USB_DRIVE_MOUNT_ROOT, &parent) == 0 &&
           point.st_dev != parent.st_dev;
}

/* The stock mdev script mounts every partition with `sync` and no utf8 for
 * FAT. Dropping it first leaves the player as the only mount. The device is
 * shared, so a lazy unmount is safe even if something still holds it. */
static void drop_stock_mount(const char * device_path) {
    char mounts[8192], stock[USB_DRIVE_MOUNT_BYTES + 64];
    read_text(USB_DRIVE_PROC_MOUNTS, mounts, sizeof(mounts));
    if (usb_drive_stock_mount_point(mounts, device_path, stock, sizeof(stock))) detach_mount(stock);
}

static void read_label(const char * device_path, const unsigned char * sector, usb_drive_t * drive) {
    char text[256] = {0};
    char * argv[] = { (char *) "blkid", (char *) device_path, NULL };
    if (subprocess_run_timeout(argv, text, sizeof(text), 5000) &&
        usb_drive_label_from_blkid(text, drive->label, sizeof(drive->label)))
        return;
    usb_drive_label_from_boot_sector(sector, 512, drive->label, sizeof(drive->label));
}

static bool mount_drive(usb_drive_t * drive) {
    char device_path[40];
    snprintf(device_path, sizeof(device_path), "%s/%s", USB_DRIVE_DEV_DIR, drive->device);
    unsigned char sector[512];
    int fd = open(device_path, O_RDONLY);
    ssize_t got = fd >= 0 ? pread(fd, sector, sizeof(sector), 0) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t) sizeof(sector)) return false;
    drive->kind = usb_drive_detect_fs(sector, sizeof(sector));
    if (drive->kind == SD_FS_KIND_UNKNOWN) return false;
    read_label(device_path, sector, drive);

    drop_stock_mount(device_path);
    mkdir(USB_DRIVE_MOUNT_ROOT, 0755);
    mkdir(drive->mount_point, 0755);
    /* A mount left by an earlier run belongs to a drive that may be gone. */
    if (mount_point_attached(drive->mount_point)) detach_mount(drive->mount_point);
    bool ntfs3g = access(USB_DRIVE_NTFS3G, X_OK) == 0;
    usb_drive_mount_plan_t plan;
    for (unsigned attempt = 0; usb_drive_mount_plan(drive->kind, ntfs3g, device_path, drive->mount_point,
                                                    attempt, &plan); attempt++) {
        run_quiet(plan.argv);
        if (mount_point_attached(drive->mount_point)) return true;
    }
    return false;
}

/* Takes the next job: a slot to mount or to unmount. */
static usb_slot_t * next_job(void) {
    for (size_t i = 0; i < USB_DRIVE_MAX; i++)
        if (drive_slots[i].state == USB_SLOT_QUEUED || drive_slots[i].state == USB_SLOT_UNMOUNTING)
            return &drive_slots[i];
    return NULL;
}

static void * worker_main(void * arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&drive_mutex);
        usb_slot_t * slot = next_job();
        if (!slot) {
            worker_running = false;
            pthread_mutex_unlock(&drive_mutex);
            return NULL;
        }
        bool unmount = slot->state == USB_SLOT_UNMOUNTING;
        usb_drive_t drive = slot->drive;
        if (!unmount) slot->state = USB_SLOT_MOUNTING;
        pthread_mutex_unlock(&drive_mutex);

        bool ok = false;
        if (unmount) detach_mount(drive.mount_point);
        else ok = mount_drive(&drive);

        pthread_mutex_lock(&drive_mutex);
        if (unmount) {
            memset(slot, 0, sizeof(*slot));
        } else if (slot->gone) {
            if (ok) slot->state = USB_SLOT_UNMOUNTING;
            else memset(slot, 0, sizeof(*slot));
        } else {
            slot->drive = drive;
            slot->state = ok ? USB_SLOT_MOUNTED : USB_SLOT_FAILED;
        }
        pthread_mutex_unlock(&drive_mutex);
    }
}

/* Called with drive_mutex held. */
static void start_worker_if_needed(void) {
    if (worker_running || !next_job()) return;
    pthread_t thread;
    worker_running = true;
    if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
        worker_running = false;
        return;
    }
    pthread_detach(thread);
}

void usb_drive_poll(usb_drive_events_t * events) {
    memset(events, 0, sizeof(*events));
#ifdef HOST_BUILD
    const bool watching = false; /* a development PC's own drives are not ours to mount */
#else
    const bool watching = true;
#endif
    static uint64_t last_poll_ms;
    uint64_t now = monotonic_ms();
    if (!watching || (last_poll_ms && now - last_poll_ms < USB_DRIVE_POLL_MS)) return;
    last_poll_ms = now;

    char present[USB_DRIVE_MAX][USB_DRIVE_DEVICE_BYTES];
    size_t count = scan_partitions(present, USB_DRIVE_MAX);
    pthread_mutex_lock(&drive_mutex);
    usb_drive_reconcile(drive_slots, (const char (*)[USB_DRIVE_DEVICE_BYTES]) present, count, events);
    start_worker_if_needed();
    pthread_mutex_unlock(&drive_mutex);
}

size_t usb_drive_list(usb_drive_t * out, size_t max) {
    size_t count = 0;
    pthread_mutex_lock(&drive_mutex);
    for (size_t i = 0; i < USB_DRIVE_MAX && count < max; i++)
        if (drive_slots[i].state == USB_SLOT_MOUNTED) out[count++] = drive_slots[i].drive;
    pthread_mutex_unlock(&drive_mutex);
    return count;
}
