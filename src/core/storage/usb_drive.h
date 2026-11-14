#ifndef USB_DRIVE_H
#define USB_DRIVE_H

#include "sd_fsck.h"

#include <stdbool.h>
#include <stddef.h>

/* USB drives plugged into the USB-C port while it is a host (OTG). A watch on
 * /sys/block finds sd[a-z][0-9]* partitions, a worker thread mounts each
 * supported one (vfat, exFAT, NTFS) under /data/mnt/usb_<n> and lazily
 * unmounts it when the device goes away. Nothing here touches the USB role,
 * the gadget or any sysfs knob: it only reads, so the Storage, DAC and ADB
 * modes are unaffected. When the port is a device, no sd* node exists and the
 * watch stays idle. */

#define USB_DRIVE_MAX 4
#define USB_DRIVE_DEVICE_BYTES 16
#define USB_DRIVE_LABEL_BYTES 48
#define USB_DRIVE_MOUNT_BYTES 48
#define USB_DRIVE_STOCK_MOUNT_DIR "/tmp/mass_storage"

/* A mounted drive partition. */
typedef struct {
    char device[USB_DRIVE_DEVICE_BYTES];       /* "sda1" */
    char mount_point[USB_DRIVE_MOUNT_BYTES];   /* "/data/mnt/usb_0" */
    char label[USB_DRIVE_LABEL_BYTES];         /* volume label, "" when it has none */
    sd_fs_kind_t kind;
} usb_drive_t;

/* ---- Pure helpers ---- */

/* Filesystem of a partition from its first 512 bytes. */
sd_fs_kind_t usb_drive_detect_fs(const unsigned char * sector, size_t length);

/* LABEL="..." of `blkid <device>` output. False when it has no label. */
bool usb_drive_label_from_blkid(const char * text, char * out, size_t out_size);

/* Label stored in a FAT boot sector, the fallback without blkid. exFAT and
 * NTFS keep theirs elsewhere and only get a label from blkid. */
bool usb_drive_label_from_boot_sector(const unsigned char * sector, size_t length, char * out, size_t out_size);

#define USB_DRIVE_ATTEMPT_MAX 2

typedef struct {
    char tool[32];
    char device[40];
    char mount_point[USB_DRIVE_MOUNT_BYTES];
    char options[160];
    char * argv[8];
} usb_drive_mount_plan_t;

/* The command that mounts a partition. attempt 0 is the full option set; the
 * next one is the fallback (bare options for FAT and exFAT, read-only for
 * NTFS, which ntfs-3g allows when Windows left the volume dirty or
 * hibernated). False once the attempts run out or the kind is unsupported.
 * ntfs3g_present picks ntfs-3g over the kernel ntfs driver. */
bool usb_drive_mount_plan(sd_fs_kind_t kind, bool ntfs3g_present, const char * device_path,
                          const char * mount_point, unsigned attempt, usb_drive_mount_plan_t * out);

/* The stock mdev script mounts partitions under /tmp/mass_storage with `sync`.
 * Finds that mount of device_path (e.g. "/dev/sda1") in /proc/mounts text. */
bool usb_drive_stock_mount_point(const char * mounts_text, const char * device_path, char * out, size_t out_size);

/* ---- Slot table, shared by the watch and the worker ---- */

typedef enum {
    USB_SLOT_FREE = 0,
    USB_SLOT_SETTLING,   /* seen once; mounts if it is still there one poll later */
    USB_SLOT_QUEUED,     /* waiting for the worker */
    USB_SLOT_MOUNTING,
    USB_SLOT_MOUNTED,
    USB_SLOT_FAILED,     /* unsupported or could not be mounted; not retried */
    USB_SLOT_UNMOUNTING, /* device gone, the worker detaches the mount */
} usb_slot_state_t;

typedef struct {
    usb_slot_state_t state;
    bool gone;       /* device vanished while the worker was mounting it */
    bool announced;  /* the outcome was already reported */
    usb_drive_t drive;
} usb_slot_t;

typedef struct {
    bool mounted;  /* a drive finished mounting */
    bool failed;   /* a drive could not be mounted */
    bool changed;  /* the list of mounted drives changed */
    size_t removed_count;
    char removed[USB_DRIVE_MAX][USB_DRIVE_MOUNT_BYTES]; /* mount points that just went away */
} usb_drive_events_t;

/* Folds the partitions the kernel lists now into the slots and reports what
 * changed. present is a list of device names such as "sda1". */
void usb_drive_reconcile(usb_slot_t slots[USB_DRIVE_MAX], const char (* present)[USB_DRIVE_DEVICE_BYTES],
                         size_t present_count, usb_drive_events_t * events);

/* ---- Runtime ---- */

/* Called from the UI poll. Cheap: at most one directory read a second, and no
 * mount work on the calling thread. Does nothing on host builds. */
void usb_drive_poll(usb_drive_events_t * events);

/* Copies the mounted drives, safe from any thread. Returns how many. */
size_t usb_drive_list(usb_drive_t * out, size_t max);

#endif
