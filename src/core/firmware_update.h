#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>

/* Firmware update via the R1 Pro's own stock recovery mechanism -- confirmed
 * by reading the closed-source hiby_player binary's strings (references a
 * "%s/sd_0/%s.upt" path pattern and calls "/usr/bin/bootmode.sh Recovery")
 * and by inspecting /proc/mtd: kernel2/rootfs2 are a separate, smaller
 * partition pair (rootfs2 is roughly half rootfs's size), not a live A/B
 * twin of the running system -- they're a dedicated recovery image whose
 * whole job is to find a *.upt file (an ISO9660 image containing a chunked
 * kernel+rootfs payload, the exact same directory shape /etc/ota_bin's
 * network OTA scripts expect, just packaged as a mountable ISO instead of a
 * plain directory) on the SD card and flash it into the main kernel/rootfs
 * slot, then switch back and reboot again. Because that recovery partition
 * is untouched by this project's own repack, no custom flashing logic is
 * needed here at all -- only the same trigger the stock player uses. */

/* Only one regular board-named image may exist at the SD root. The checked
 * form describes missing, ambiguous, wrong-board and unreadable files. */
bool firmware_update_scan(char * out_path, size_t out_size);
bool firmware_update_scan_checked(char * out_path, size_t out_size, char * error, size_t error_size);

typedef enum {
    FIRMWARE_UPDATE_IDLE,
    FIRMWARE_UPDATE_VALIDATING,
    FIRMWARE_UPDATE_BOOTFLAG,
    FIRMWARE_UPDATE_SYNC,
    FIRMWARE_UPDATE_REBOOT,
    FIRMWARE_UPDATE_FAILED
} firmware_update_phase_t;

typedef struct {
    firmware_update_phase_t phase;
    bool busy;
    bool delayed; /* helper exceeded 15 seconds; no retry while it still runs */
    char error[160];
} firmware_update_status_t;

void firmware_update_get_status(firmware_update_status_t * out);
bool firmware_update_busy(void);
/* Shared atomic gate for online check/download/install and manual recovery.
 * OTA release only drops an OTA-owned claim, never an active NAND operation. */
bool firmware_update_claim_ota(void);
void firmware_update_release_ota(void);
bool firmware_update_enter_ota_recovery_for_path(const char * path);
/* Starts manual validation/recovery on a single-flight worker. No LVGL work.
 * All diagnostics are persisted at SD/.compas/ota/update.log. */
bool firmware_update_start(const char * path, char * error, size_t error_size);
/* Synchronous worker/early-boot entry. Validates the selected package, checks
 * erase/write and verifies the recovery flag before flushing and rebooting.
 * Host builds only validate. Returns false on failure; success on device does
 * not return. NAND helpers are never killed by a UI deadline; delayed status
 * remains busy until they finish, preventing unsafe retries. */
bool firmware_update_enter_recovery_for_path(const char * path);
void firmware_update_enter_recovery(void);

/* Checks whether Power + Volume Up (Power + Play/Pause on the R3II 2025, the
 * stock combo there) are BOTH currently held down (via
 * EVIOCGKEY, which reads the device's live key-state bitmap rather than
 * waiting for a fresh press event -- by the time this runs, several seconds
 * into boot, the user is expected to already be holding both, so a normal
 * evdev press event would never arrive) and a *.upt file is present. If so,
 * calls firmware_update_enter_recovery() immediately with no confirmation
 * prompt -- this is the deliberate hold-at-boot recovery gesture, the same
 * kind of unprompted combo other devices use for the same purpose -- and
 * does not return on success. Meant to run once, very early in main(), before
 * any display/GUI setup, so a held combo never even flashes the normal UI
 * on screen first. No-op (returns normally) if the combo isn't held or no
 * update file is present, or entering recovery fails. */
void firmware_update_check_boot_combo(void);

#endif /* FIRMWARE_UPDATE_H */
