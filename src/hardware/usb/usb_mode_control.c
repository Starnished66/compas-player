#include "usb_mode_control.h"
#include "../power/battery.h"
#include "../../core/system/runtime/board_config.h"
#include "../power/charge_limiter.h"
#include "../../core/persistence/storage_paths.h"
#include "../../core/storage/sd_fsck.h"
#include "../../core/process/subprocess.h"
#include "usb_dac_bridge.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define SETTLE_RETRY_LIMIT 20 /* ~1s at 50ms between checks */

/* Storage and DAC both live under this same configfs gadget name (confirmed
 * by reading usb_dev_mass_storage.sh and uac_device_config.sh directly off
 * the device) -- ADB uses a separate "adb_demo" directory. */
#ifndef USB_MODE_CONTROL_ANDROID0_DIR
#define USB_MODE_CONTROL_ANDROID0_DIR "/sys/kernel/config/usb_gadget/android0"
#endif
#ifndef USB_MODE_CONTROL_ADB_DEMO_DIR
#define USB_MODE_CONTROL_ADB_DEMO_DIR "/sys/kernel/config/usb_gadget/adb_demo"
#endif
#ifndef USB_MODE_CONTROL_UDC_STATE_DIR
#define USB_MODE_CONTROL_UDC_STATE_DIR "/sys/class/udc"
#endif
#ifndef USB_MODE_CONTROL_GADGET_ROOT
#define USB_MODE_CONTROL_GADGET_ROOT "/sys/kernel/config/usb_gadget"
#endif
#ifndef USB_MODE_CONTROL_R1_ROLE_PATH
#define USB_MODE_CONTROL_R1_ROLE_PATH "/sys/devices/platform/tcs1421/tcs1421_cfg"
#endif
#ifndef USB_MODE_CONTROL_DAC_JOURNAL_PATH
#define USB_MODE_CONTROL_DAC_JOURNAL_PATH INTERNAL_COMPAS_DIR "/usb_dac_session.txt"
#endif
#define ANDROID0_DIR USB_MODE_CONTROL_ANDROID0_DIR
#define ANDROID0_UDC_PATH ANDROID0_DIR "/UDC"
#define ADB_DEMO_DIR USB_MODE_CONTROL_ADB_DEMO_DIR
#define ADB_DEMO_UDC_PATH ADB_DEMO_DIR "/UDC"

/* Matches subprocess_run()'s own default -- not reused directly since
 * SUBPROCESS_TIMEOUT_MS is private to subprocess.c and there's no public
 * "run with the default timeout, but checked" entry point. */
#define SCRIPT_TIMEOUT_MS 15000

static pthread_mutex_t usb_mode_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned storage_write_lease_count;
static atomic_uint storage_write_active_count;
static bool storage_write_restore_needed;
static char storage_write_saved_controller[256];
#if BOARD_R1_HARDWARE
static bool dac_usb_policy_owned;
static char dac_saved_usb_role[24];
static bool dac_saved_charger_enabled;
static bool dac_last_prepare_captured_baseline;
static bool dac_saved_charger_valid;
static bool dac_charging_disabled = true;
#endif

/* The TCS1421 add-on driver accepts these four exact role words and exposes
 * them on the R1 sysfs node. Save the live value instead of assuming the
 * usual NormalDRP boot role, so leaving DAC returns to the actual prior role. */
#if BOARD_R1_HARDWARE
static bool r1_usb_role_valid(const char *role) {
    return strcmp(role, "Sink") == 0 || strcmp(role, "Source") == 0 ||
           strcmp(role, "StrongDRP") == 0 || strcmp(role, "NormalDRP") == 0;
}

static bool r1_usb_role_read(char *role, size_t size) {
    FILE *file = fopen(USB_MODE_CONTROL_R1_ROLE_PATH, "r");
    if (!file) return false;
    size_t len = fread(role, 1, size - 1, file);
    bool ok = !ferror(file) && memchr(role, '\0', len) == NULL;
    int extra = fgetc(file);
    if (extra != EOF || !feof(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    if (len && role[len - 1] == '\n') --len;
    if (len && role[len - 1] == '\r') --len;
    role[len] = '\0';
    return len > 0 && r1_usb_role_valid(role);
}

static bool r1_usb_role_set(const char *role) {
    if (!r1_usb_role_valid(role)) return false;
    FILE *file = fopen(USB_MODE_CONTROL_R1_ROLE_PATH, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%s\n", role) > 0;
    if (fclose(file) != 0) ok = false;
    char actual[24];
    return ok && r1_usb_role_read(actual, sizeof(actual)) && strcmp(actual, role) == 0;
}

typedef enum {
    DAC_JOURNAL_ERROR = -1,
    DAC_JOURNAL_ABSENT = 0,
    DAC_JOURNAL_VALID = 1
} dac_journal_result_t;

static bool sync_dac_journal_directory(void) {
    char parent[sizeof(USB_MODE_CONTROL_DAC_JOURNAL_PATH)];
    snprintf(parent, sizeof(parent), "%s", USB_MODE_CONTROL_DAC_JOURNAL_PATH);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool save_dac_session_journal(const char *role, bool charger_enabled) {
    if (!r1_usb_role_valid(role)) return false;
    char parent[sizeof(USB_MODE_CONTROL_DAC_JOURNAL_PATH)];
    snprintf(parent, sizeof(parent), "%s", USB_MODE_CONTROL_DAC_JOURNAL_PATH);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = '\0';
    if (mkdir(parent, 0755) != 0 && errno != EEXIST) return false;

    char temporary[sizeof(USB_MODE_CONTROL_DAC_JOURNAL_PATH) + 32];
    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
             USB_MODE_CONTROL_DAC_JOURNAL_PATH, (long) getpid());
    FILE *file = fopen(temporary, "w");
    if (!file) return false;
    bool ok = fprintf(file, "version=1\nrole=%s\ncharger_enabled=%d\n",
                      role, charger_enabled ? 1 : 0) > 0;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(temporary); return false; }
    if (rename(temporary, USB_MODE_CONTROL_DAC_JOURNAL_PATH) != 0) {
        unlink(temporary);
        return false;
    }
    return sync_dac_journal_directory();
}

static bool remove_dac_session_journal(void) {
    if (unlink(USB_MODE_CONTROL_DAC_JOURNAL_PATH) != 0 && errno != ENOENT) return false;
    return sync_dac_journal_directory();
}

static dac_journal_result_t load_dac_session_journal(char *role, size_t role_size,
                                                      bool *charger_enabled) {
    FILE *file = fopen(USB_MODE_CONTROL_DAC_JOURNAL_PATH, "r");
    if (!file) return errno == ENOENT ? DAC_JOURNAL_ABSENT : DAC_JOURNAL_ERROR;
    char content[128];
    size_t len = fread(content, 1, sizeof(content) - 1, file);
    bool ok = !ferror(file) && memchr(content, '\0', len) == NULL;
    int extra = fgetc(file);
    if (extra != EOF || !feof(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) return DAC_JOURNAL_ERROR;
    content[len] = '\0';

    static const char *const roles[] = { "Sink", "Source", "StrongDRP", "NormalDRP" };
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        for (int enabled = 0; enabled <= 1; ++enabled) {
            char expected[96];
            int expected_len = snprintf(expected, sizeof(expected),
                                        "version=1\nrole=%s\ncharger_enabled=%d\n",
                                        roles[i], enabled);
            if (expected_len > 0 && (size_t) expected_len < sizeof(expected) &&
                len == (size_t) expected_len && strcmp(content, expected) == 0) {
                if (strlen(roles[i]) >= role_size) return DAC_JOURNAL_ERROR;
                snprintf(role, role_size, "%s", roles[i]);
                *charger_enabled = enabled != 0;
                return DAC_JOURNAL_VALID;
            }
        }
    }
    return DAC_JOURNAL_ERROR;
}
#endif

static bool restore_dac_usb_policy(void) {
#if BOARD_R1_HARDWARE
    if (!dac_usb_policy_owned) return true;
    /* A previous restore may have released the in-memory charger claim but
     * failed to remove the durable record. Reclaim it from the saved snapshot
     * before retrying, so a poll cannot change the value we eventually restore. */
    if (!charge_limiter_dac_session_resume(dac_saved_charger_enabled)) {
        fprintf(stderr, "usb_mode_control: could not reclaim DAC charger inhibit for restore retry\n");
        return false;
    }
    /* Restore the Type-C role first. Charging remains inhibited if that
     * write cannot be verified, so a still-bound DAC cannot resume charging. */
    bool role_ok = r1_usb_role_set(dac_saved_usb_role);
    if (!role_ok) {
        fprintf(stderr, "usb_mode_control: DAC role restore failed; keeping charger inhibited and retaining ownership\n");
        return false;
    }
    bool charger_ok = charge_limiter_dac_session_release(dac_saved_charger_enabled);
    if (!charger_ok) {
        fprintf(stderr, "usb_mode_control: DAC charger restore failed; retaining ownership and journal\n");
        return false;
    }
    if (!remove_dac_session_journal()) {
        fprintf(stderr, "usb_mode_control: DAC session journal removal failed; retaining ownership for retry\n");
        return false;
    }
    dac_usb_policy_owned = false;
    return true;
#else
    return true;
#endif
}

static bool prepare_dac_usb_policy(void) {
#if BOARD_R1_HARDWARE
    dac_last_prepare_captured_baseline = false;
    bool already_owned = dac_usb_policy_owned;
    if (!dac_usb_policy_owned) {
        dac_saved_charger_valid = false;
        if (!r1_usb_role_read(dac_saved_usb_role, sizeof(dac_saved_usb_role))) {
            fprintf(stderr, "usb_mode_control: cannot read a supported R1 Type-C role; refusing DAC start\n");
            return false;
        }
        dac_last_prepare_captured_baseline = true;
    } else {
        dac_last_prepare_captured_baseline = true;
    }
    bool reclaimed = !already_owned || (dac_charging_disabled
        ? charge_limiter_dac_session_resume(dac_saved_charger_enabled)
        : charge_limiter_dac_session_release(dac_saved_charger_enabled));
    if (!reclaimed) {
        fprintf(stderr, "usb_mode_control: failed to reclaim R1 charger inhibit for DAC retry\n");
        return false;
    }
    bool charger_enabled = false;
    bool charger_ok = true;
    bool captured = true;
    if (already_owned) {
        charger_enabled = dac_saved_charger_enabled;
    } else if (dac_charging_disabled) {
        charger_ok = charge_limiter_dac_session_begin(&charger_enabled);
        captured = charge_limiter_dac_session_get_original_enabled(&charger_enabled);
    } else {
        charger_ok = charge_limiter_dac_session_capture_original_enabled(&charger_enabled);
        captured = charger_ok;
    }
    if (!charger_ok && !captured) {
        if (!already_owned) (void) charge_limiter_dac_session_end();
        fprintf(stderr, "usb_mode_control: failed to inhibit R1 charging for DAC\n");
        return false;
    }
    if (!already_owned) dac_saved_charger_enabled = charger_enabled;
    dac_saved_charger_valid = true;
    dac_usb_policy_owned = true;
    /* Persist on every attempt, including retries after a failed rollback.
     * Sink must never be entered without a recoverable original charger value. */
    if (!save_dac_session_journal(dac_saved_usb_role, dac_saved_charger_enabled)) {
        fprintf(stderr, "usb_mode_control: failed to persist DAC session recovery journal\n");
        if (!already_owned && charge_limiter_dac_session_end()) dac_usb_policy_owned = false;
        return false;
    }
    if (!dac_charging_disabled &&
        !charge_limiter_dac_session_release(dac_saved_charger_enabled)) {
        fprintf(stderr, "usb_mode_control: failed to allow R1 charging for DAC preference\n");
        return false;
    }
    if (!charger_ok) {
        fprintf(stderr, "usb_mode_control: failed to inhibit R1 charging for DAC\n");
        if (!already_owned) (void) restore_dac_usb_policy();
        return false;
    }
    bool role_ok = r1_usb_role_set("Sink");
    if (!role_ok) {
        fprintf(stderr, "usb_mode_control: failed to force R1 Type-C Sink role for DAC\n");
        if (!already_owned) (void) restore_dac_usb_policy();
        return false;
    }
#endif
    return true;
}

static bool path_exists(const char * path) {
    return access(path, F_OK) == 0;
}

static bool mounted_sd_partition_is_live(void) {
    struct stat mounted_root_st, partition_st;
    if (stat("/data/mnt/sd_0", &mounted_root_st) != 0 ||
        stat("/dev/mmcblk0p1", &partition_st) != 0 || !S_ISBLK(partition_st.st_mode))
        return false;

    FILE * dev_file = fopen("/sys/class/block/mmcblk0p1/dev", "r");
    if (!dev_file) return false;
    unsigned major_number, minor_number;
    char trailing;
    int parsed = fscanf(dev_file, "%u:%u %c", &major_number, &minor_number, &trailing);
    bool read_ok = parsed == 2 && !ferror(dev_file);
    if (fclose(dev_file) != 0) read_ok = false;
    if (!read_ok) return false;

    dev_t live_partition = makedev(major_number, minor_number);
    return mounted_root_st.st_dev == partition_st.st_rdev &&
           partition_st.st_rdev == live_partition;
}

/* Export the partition the player actually mounted. Reading one line at a
 * time preserves sd_fsck_parse_mounts()'s last-matching-mount semantics
 * without imposing a size limit on /proc/mounts. Anything other than the
 * known partition node (including a whole-disk/superfloppy mount, an
 * unsupported partition, or a read/parse failure) keeps the full-disk
 * backing device. */
static const char * storage_backing_device(void) {
    FILE * mounts = fopen("/proc/mounts", "r");
    if (!mounts) return "/dev/mmcblk0";

    char * line = NULL;
    size_t line_capacity = 0;
    bool found = false;
    char mounted_device[SD_FSCK_DEVICE_BYTES] = { 0 };
    ssize_t line_length;
    while ((line_length = getline(&line, &line_capacity, mounts)) >= 0) {
        (void) line_length;
        sd_mount_info_t info;
        if (sd_fsck_parse_mounts(line, &info) && info.found) {
            snprintf(mounted_device, sizeof(mounted_device), "%s", info.device);
            found = true;
        }
    }
    bool read_ok = feof(mounts) && !ferror(mounts);
    free(line);
    if (fclose(mounts) != 0) read_ok = false;
    if (read_ok && found && sd_fsck_device_allowed(mounted_device) &&
        strcmp(mounted_device, "/dev/mmcblk0p1") == 0 && mounted_sd_partition_is_live())
        return "/dev/mmcblk0p1";
    return "/dev/mmcblk0";
}

bool usb_mode_control_cable_connected(void) {
    return battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED;
}

/* A gadget's UDC sysfs file holds the bound UDC's name (e.g.
 * "13500000.otg_new") once `echo $UDC > UDC` has actually taken, empty
 * otherwise -- the one place in configfs that says "the kernel accepted
 * this gadget's descriptors and it's live", as opposed to "the script that
 * was supposed to set it up merely ran to completion". */
static bool udc_is_bound(const char * udc_path) {
    FILE * f = fopen(udc_path, "r");
    if (!f) return false;
    char buf[64] = { 0 };
    bool read_ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!read_ok) return false;

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = '\0';
    if (len == 0 || strcmp(buf, "none") == 0) return false;
    return true;
}

/* Polls udc_is_bound() for up to ~1s instead of checking exactly once --
 * `echo $UDC > UDC` returning to the shell script doesn't necessarily mean
 * the kernel has fully finished processing the bind by the time this
 * process's subprocess_run_checked() call returns and this gets checked;
 * a real device switch showed a start script exit 0 with a function
 * correctly linked but UDC reading back empty on the first immediate
 * check, reported as a spurious "can't switch" failure. */
static bool udc_becomes_bound(const char * udc_path) {
    for (int i = 0; i < SETTLE_RETRY_LIMIT; i++) {
        if (udc_is_bound(udc_path)) return true;
        usleep(50000);
    }
    return false;
}

/* True if some function link under <gadget_dir>/configs/c.1 starts with
 * `name_prefix` (e.g. "uac_sa." or "mass_storage.") -- checking for a
 * SPECIFIC expected function, not just "is anything bound", is what catches
 * the case a real device hit: android0's UDC bound but to a mass_storage.N
 * function left over from a different mode, while this app believed (and
 * told the user) DAC had succeeded. */
static bool function_linked(const char * gadget_dir, const char * name_prefix) {
    char path[256];
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return false;

    bool found = false;
    size_t prefix_len = strlen(name_prefix);
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, name_prefix, prefix_len) == 0) { found = true; break; }
    }
    closedir(dir);
    return found;
}

#if BOARD_R1_HARDWARE
static bool dac_gadget_may_be_bound(void) {
    FILE *udc = fopen(ANDROID0_UDC_PATH, "r");
    if (!udc) return errno != ENOENT;
    char value[64];
    size_t len = fread(value, 1, sizeof(value) - 1, udc);
    bool readable = !ferror(udc) && memchr(value, '\0', len) == NULL;
    int extra = fgetc(udc);
    if (extra != EOF || !feof(udc)) readable = false;
    if (fclose(udc) != 0) readable = false;
    if (!readable) return true;
    if (len && value[len - 1] == '\n') --len;
    if (len && value[len - 1] == '\r') --len;
    value[len] = '\0';
    if (len == 0 || strcmp(value, "none") == 0) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/configs/c.1", ANDROID0_DIR);
    DIR *dir = opendir(path);
    /* If the UDC is bound but configfs cannot be inspected, fail closed and
     * keep the charger inhibited until a later retry can prove UAC is gone. */
    if (!dir) return true;
    bool found = false;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "uac_sa.", 7) == 0) { found = true; break; }
    }
    bool read_error = errno != 0;
    if (closedir(dir) != 0) read_error = true;
    return found || read_error;
}
#endif

/* Generic teardown of a configfs gadget directory -- doesn't
 * assume any particular function name is inside, unlike the stock stop
 * scripts (uac_device_config.sh's uac_stop(), usb_dev_mass_storage.sh's
 * storage_stop()), which each hardcode exactly one function name and
 * silently fail to finish (their targeted rm/rmdir just no-op on a path
 * that doesn't exist, and the final rmdir on the gadget dir itself then
 * fails since it's non-empty) if a MISMATCHED function from a different
 * mode's incomplete previous teardown is what's actually there. Confirmed
 * on a real device: this is exactly how android0 ended up permanently
 * stuck with a stale mass_storage.0 that DAC's own stop script couldn't
 * remove, silently corrupting every subsequent switch attempt. Called
 * after the stock stop scripts as a guarantee, not a replacement for
 * them -- they still do the real process-killing side effects (killall
 * adbd/adbserver.sh) this doesn't attempt to duplicate. */
static void write_udc(const char * gadget_dir, const char * value) {
    char path[300];
    snprintf(path, sizeof(path), "%s/UDC", gadget_dir);
    FILE * udc_f = fopen(path, "w");
    if (udc_f) {
        fputs(value, udc_f);
        fclose(udc_f);
    }
}

/* Empty lun.0/file is the gadget-side eject. A host that still has MUSIC
 * mounted can make the first write fail; unbind UDC (below) then retry.
 * forced_eject is optional on this kernel -- ignore if the attr is absent. */
static void eject_mass_storage_luns(const char * gadget_dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "mass_storage.", 13) != 0) continue;
        char attr[320];
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/forced_eject", gadget_dir, entry->d_name);
        FILE * ej = fopen(attr, "w");
        if (ej) { fputs("1\n", ej); fclose(ej); }
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
        FILE * lun_f = fopen(attr, "w");
        if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
    }
    closedir(dir);
}

static bool rmdir_mass_storage_functions(const char * gadget_dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    bool leftover = false;
    DIR * dir = opendir(path);
    if (!dir) return true;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char func_path[300];
        snprintf(func_path, sizeof(func_path), "%s/functions/%s", gadget_dir, entry->d_name);
        if (strncmp(entry->d_name, "mass_storage.", 13) == 0) {
            char lun_path[320];
            snprintf(lun_path, sizeof(lun_path), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
            FILE * lun_f = fopen(lun_path, "w");
            if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
        }
        if (rmdir(func_path) != 0 && path_exists(func_path)) {
            fprintf(stderr, "usb_mode_control: rmdir %s failed (%s)\n", func_path, strerror(errno));
            leftover = true;
        }
    }
    closedir(dir);
    return !leftover;
}

static bool force_clean_gadget_dir(const char * gadget_dir) {
    char path[300];

    /* Eject the LUN while still bound so a PC sees media-removed rather
     * than a surprise disconnect, then unbind. Stock storage_stop() does
     * the opposite and, if rmdir is EBUSY, re-binds UDC -- which is why
     * ADB/DAC only worked after the SD card was physically removed. */
    eject_mass_storage_luns(gadget_dir);
    write_udc(gadget_dir, "\n");
    write_udc(gadget_dir, "none\n");
    usleep(200000);
    eject_mass_storage_luns(gadget_dir);

    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    DIR * cfg = opendir(path);
    if (cfg) {
        struct dirent * entry;
        while ((entry = readdir(cfg)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char link_path[300];
            snprintf(link_path, sizeof(link_path), "%s/configs/c.1/%s", gadget_dir, entry->d_name);
            unlink(link_path);
        }
        closedir(cfg);
    }

    for (int i = 0; i < 10; i++) {
        if (rmdir_mass_storage_functions(gadget_dir)) break;
        usleep(200000);
        eject_mass_storage_luns(gadget_dir);
    }

    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/strings/0x409", gadget_dir);
    rmdir(path);
    rmdir(gadget_dir);
    return !path_exists(gadget_dir);
}

static bool configfs_has_gadgets(void) {
    DIR * dir = opendir("/sys/kernel/config/usb_gadget");
    if (!dir) return false;
    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') { found = true; break; }
    }
    closedir(dir);
    return found;
}

/* ADB and DAC start scripts require configfs to be unmounted before running.
 * Unmount configfs if no active gadgets remain. */
static bool unmount_empty_configfs(void) {
    if (!path_exists("/sys/kernel/config/usb_gadget")) return true;
    if (configfs_has_gadgets()) return false;
    int exit_code = -1;
    char * argv[] = { (char *) "/bin/umount", (char *) "/sys/kernel/config", NULL };
    subprocess_run_checked(argv, NULL, 0, 3000, &exit_code);
    if (path_exists("/sys/kernel/config/usb_gadget")) {
        char * lazy[] = { (char *) "/bin/umount", (char *) "-l", (char *) "/sys/kernel/config", NULL };
        subprocess_run_checked(lazy, NULL, 0, 3000, &exit_code);
    }
    return !path_exists("/sys/kernel/config/usb_gadget");
}

static bool wait_path(const char * path, int attempts) {
    for (int i = 0; i < attempts; i++) {
        if (path_exists(path)) return true;
        usleep(50000);
    }
    return false;
}

static bool start_adb_gadget(void) {
    int exit_code = -1;
    char * argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
    subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
    if (exit_code != 0) {
        fprintf(stderr, "usb_mode_control: S440adb start exit=%d\n", exit_code);
        return false;
    }
    /* S440adb backgrounds adbserver.sh, which mounts functionfs then starts
     * adbd. The UDC bind is a separate stock script and must wait until
     * that mount exists -- otherwise udc_becomes_bound() times out and the
     * UI reports "Failed to switch to ADB" even though setup is in flight. */
    wait_path("/dev/usb-ffs/adb", SETTLE_RETRY_LIMIT);
    char * bind_argv[] = { (char *) "/sbin/usb_adb_enable.sh", NULL };
    subprocess_run_checked(bind_argv, NULL, 0, 5000, &exit_code);
    bool ok = udc_becomes_bound(ADB_DEMO_UDC_PATH) && function_linked(ADB_DEMO_DIR, "ffs.");
    fprintf(stderr, "usb_mode_control: ADB bind exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
    return ok;
}

static bool usb_mode_control_apply_gadget_locked(usb_mode_t mode) {
    fprintf(stderr, "usb_mode_control: switching to mode=%d\n", (int) mode);
    /* Capture the source before teardown changes mounts or gadget state. */
    const char * storage_device = mode == USB_MODE_STORAGE ? storage_backing_device() : "/dev/mmcblk0";
    usb_dac_bridge_stop(); /* no-op unless we're actually leaving DAC mode */

    /* Deliberately does NOT call uac_device_config.sh's own "stop" --
     * confirmed on a real device that uac_stop()'s very first line (`echo 0
     * > /sys/class/usb_gadget/android0/soft_disconnect`, a stale reference
     * to the legacy sysfs class path rather than the configfs one this
     * script otherwise uses throughout) blocks indefinitely on this
     * device/kernel. Every call ate the full subprocess timeout and never
     * reached its actual cleanup code, which is how android0 ended up
     * permanently stuck with stale leftover functions poisoning every
     * later switch. force_clean_gadget_dir() below (which only touches the
     * safe configfs paths, confirmed working directly against this exact
     * leftover state) handles android0 teardown unconditionally instead,
     * for both DAC and Storage. */

    char * adb_stop_argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "stop", NULL };
    subprocess_run(adb_stop_argv, NULL, 0);

    /* Do not call usb_dev_mass_storage.sh stop. If rmdir of mass_storage.0
     * fails because the host still has the volume mounted, that script
     * re-binds UDC and returns 0 -- Storage stays up and every later
     * ADB/DAC start refuses. Eject + force_clean below never re-bind. */
    sync();

    /* Guarantee a clean slate regardless of whether the stock stop scripts
     * actually finished -- see force_clean_gadget_dir()'s own comment. */
    if (path_exists(ANDROID0_DIR)) {
        fprintf(stderr, "usb_mode_control: android0 survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(ANDROID0_DIR); i++) {
            if (force_clean_gadget_dir(ANDROID0_DIR)) break;
            usleep(200000);
        }
    }
    if (path_exists(ADB_DEMO_DIR)) {
        fprintf(stderr, "usb_mode_control: adb_demo survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(ADB_DEMO_DIR); i++) {
            if (force_clean_gadget_dir(ADB_DEMO_DIR)) break;
            usleep(200000);
        }
    }
    /* A leftover unbound gadget dir is not fatal: Storage reuses android0,
     * ADB lives under a different name, and the DAC start script now drops
     * an unbound android0 itself. Only a still-bound UDC blocks the switch. */
    if (udc_is_bound(ANDROID0_UDC_PATH) || udc_is_bound(ADB_DEMO_UDC_PATH)) {
        fprintf(stderr, "usb_mode_control: UDC still bound after teardown; refusing to start mode=%d\n", (int) mode);
        return false;
    }

    /* Prefer an unmounted configfs so stock start scripts are happy.
     * If umount fails on an empty leftover (common after Storage, or a
     * recovery flash with the cable still in), ADB/DAC start scripts now
     * tolerate that and mkdir their gadget on the existing mount. */
    if (mode != USB_MODE_STORAGE && !unmount_empty_configfs())
        fprintf(stderr, "usb_mode_control: configfs still mounted before mode=%d; continuing\n", (int) mode);

    int exit_code = -1;
    switch (mode) {
        case USB_MODE_STORAGE: {
            char * argv[] = { (char *) "/usr/bin/usb_dev_mass_storage.sh", (char *) "start", (char *) storage_device, NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ANDROID0_UDC_PATH) && function_linked(ANDROID0_DIR, "mass_storage.");
            fprintf(stderr, "usb_mode_control: Storage start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
        case USB_MODE_ADB:
            return start_adb_gadget();
        case USB_MODE_DAC: {
            /* No -v/-p/-m/-n overrides -- the script's own defaults
             * (VID 0x32BB/PID 0x0004, "Hiby"/"R1") already match what the
             * stock binary passes explicitly, confirmed by reading the
             * script directly off the device. */
            char * argv[] = { (char *) "/usr/bin/uac_device_config.sh", (char *) "start", NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ANDROID0_UDC_PATH) && function_linked(ANDROID0_DIR, "uac_sa.");
            fprintf(stderr, "usb_mode_control: DAC start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            if (!ok) return false;
            usb_dac_bridge_start();
            return true;
        }
    }
    return false;
}

static bool finish_dac_usb_policy_apply(usb_mode_t mode, bool applied, bool dac_still_bound) {
    if (mode == USB_MODE_DAC) {
        if (!applied && !dac_still_bound) {
            fprintf(stderr, "usb_mode_control: DAC gadget failed and is unbound; restoring pre-DAC USB policy\n");
            (void) restore_dac_usb_policy();
        } else if (!applied && dac_still_bound) {
            fprintf(stderr, "usb_mode_control: DAC apply failed while its gadget remains bound; retaining charger inhibit\n");
        }
        return applied;
    }

#if BOARD_R1_HARDWARE
    /* Keep the R1 charger inhibited until teardown has proved the UAC gadget
     * is gone. A failed target switch may have left the old DAC bound, in
     * which case restoring DRP/charging would recreate the reported failure. */
    if (dac_usb_policy_owned && !dac_still_bound) {
        if (!restore_dac_usb_policy()) return false;
    } else if (dac_usb_policy_owned && dac_still_bound) {
        fprintf(stderr, "usb_mode_control: DAC remains bound after mode=%d failure; retaining charger inhibit\n",
                (int) mode);
    }
#else
    (void) dac_still_bound;
#endif
    return applied;
}

/* Keep the R1's Type-C sink/charger policy paired with the lifetime of its
 * USB DAC gadget. The caller holds usb_mode_mutex, so DAC retries and mode
 * exits cannot interleave their saved-role bookkeeping. */
static bool usb_mode_control_apply_locked(usb_mode_t mode) {
    if (mode == USB_MODE_DAC && !prepare_dac_usb_policy()) return false;

    bool applied = usb_mode_control_apply_gadget_locked(mode);
    bool dac_still_bound = false;
#if BOARD_R1_HARDWARE
    if (dac_usb_policy_owned || (mode == USB_MODE_DAC && !applied))
        dac_still_bound = dac_gadget_may_be_bound();
#endif
    return finish_dac_usb_policy_apply(mode, applied, dac_still_bound);
}

bool usb_mode_control_reconcile_dac_session(void) {
#if BOARD_R1_HARDWARE
    pthread_mutex_lock(&usb_mode_mutex);
    if (storage_write_lease_count != 0) {
        pthread_mutex_unlock(&usb_mode_mutex);
        return false;
    }

    bool dac_may_be_bound = dac_gadget_may_be_bound();
    char saved_role[24];
    bool charger_enabled = false;
    dac_journal_result_t journal = load_dac_session_journal(
        saved_role, sizeof(saved_role), &charger_enabled);
    if (journal == DAC_JOURNAL_ABSENT) {
        bool ok = true;
        if (dac_may_be_bound && !prepare_dac_usb_policy()) {
            if (dac_usb_policy_owned || dac_last_prepare_captured_baseline) {
                /* Preparation may have captured the real legacy-session
                 * baseline before a transient role failure. A successful
                 * rollback can clear ownership, but must not erase that
                 * still-needed snapshot while the legacy gadget remains live. */
                if (!dac_saved_charger_valid) {
                    /* The register read itself failed before an original bit
                     * could be captured. Keep charging off conservatively. */
                    dac_saved_charger_enabled = false;
                    dac_saved_charger_valid = true;
                }
                dac_usb_policy_owned = true;
                bool charger_claimed = dac_charging_disabled
                    ? charge_limiter_dac_session_resume(dac_saved_charger_enabled)
                    : charge_limiter_dac_session_release(dac_saved_charger_enabled);
                bool journal_ok = save_dac_session_journal(
                    dac_saved_usb_role, dac_saved_charger_enabled);
                bool role_ok = r1_usb_role_set("Sink");
                fprintf(stderr, "usb_mode_control: legacy DAC retry retains captured baseline (charger=%d journal=%d role=%d)\n",
                        charger_claimed, journal_ok, role_ok);
                pthread_mutex_unlock(&usb_mode_mutex);
                return false;
            }
            /* A legacy live DAC may be active even when its prior role
             * cannot be captured or durable journaling is unavailable. Keep
             * it on the conservative role with charging inhibited, and
             * persist that safe fallback if possible for the next restart. */
            snprintf(dac_saved_usb_role, sizeof(dac_saved_usb_role), "Sink");
            dac_saved_charger_enabled = false;
            dac_saved_charger_valid = true;
            dac_usb_policy_owned = true;
            bool charger_ok = dac_charging_disabled
                ? charge_limiter_dac_session_resume(false)
                : charge_limiter_dac_session_release(false);
            if (access(USB_MODE_CONTROL_DAC_JOURNAL_PATH, F_OK) != 0)
                (void) save_dac_session_journal("Sink", false);
            bool role_ok = r1_usb_role_set("Sink");
            fprintf(stderr, "usb_mode_control: legacy DAC recovery retained conservatively (charger=%d role=%d)\n",
                    charger_ok, role_ok);
            ok = false;
        }
        pthread_mutex_unlock(&usb_mode_mutex);
        return ok;
    }

    if (journal == DAC_JOURNAL_ERROR) {
        fprintf(stderr, "usb_mode_control: DAC session journal is unreadable or invalid\n");
        if (!dac_may_be_bound) {
            bool cleaned = remove_dac_session_journal();
            pthread_mutex_unlock(&usb_mode_mutex);
            return cleaned;
        }

        /* Without a trustworthy saved role/state, use the safe session role
         * and keep charging inhibited. Leave the corrupt record for diagnosis
         * and retry rather than guessing the prior Type-C/charger policy. */
        snprintf(dac_saved_usb_role, sizeof(dac_saved_usb_role), "Sink");
        dac_saved_charger_enabled = false;
        dac_saved_charger_valid = true;
        dac_usb_policy_owned = true;
        bool charger_ok = dac_charging_disabled
            ? charge_limiter_dac_session_resume(false)
            : charge_limiter_dac_session_release(false);
        bool role_ok = r1_usb_role_set("Sink");
        fprintf(stderr, "usb_mode_control: retaining conservative DAC policy (charger=%d role=%d)\n",
                charger_ok, role_ok);
        pthread_mutex_unlock(&usb_mode_mutex);
        return false;
    }

    snprintf(dac_saved_usb_role, sizeof(dac_saved_usb_role), "%s", saved_role);
    dac_saved_charger_enabled = charger_enabled;
    dac_saved_charger_valid = true;
    dac_usb_policy_owned = true;
    if (dac_may_be_bound) {
        bool charger_ok = dac_charging_disabled
            ? charge_limiter_dac_session_resume(charger_enabled)
            : charge_limiter_dac_session_release(charger_enabled);
        bool role_ok = r1_usb_role_set("Sink");
        if (!charger_ok || !role_ok)
            fprintf(stderr, "usb_mode_control: DAC recovery remains owned (charger=%d role=%d)\n",
                    charger_ok, role_ok);
        pthread_mutex_unlock(&usb_mode_mutex);
        return charger_ok && role_ok;
    }

    bool charger_ok = charge_limiter_dac_session_resume(charger_enabled);
    if (!charger_ok) {
        fprintf(stderr, "usb_mode_control: charger could not be inhibited while recovering stale DAC lease\n");
        pthread_mutex_unlock(&usb_mode_mutex);
        return false;
    }
    bool restored = restore_dac_usb_policy();
    pthread_mutex_unlock(&usb_mode_mutex);
    return restored;
#else
    return true;
#endif
}

bool usb_mode_control_set_dac_charging_disabled(bool disabled) {
#if BOARD_R1_HARDWARE
    pthread_mutex_lock(&usb_mode_mutex);
    if (disabled == dac_charging_disabled && !dac_usb_policy_owned) {
        pthread_mutex_unlock(&usb_mode_mutex);
        return true;
    }
    if (!dac_usb_policy_owned) {
        dac_charging_disabled = disabled;
        pthread_mutex_unlock(&usb_mode_mutex);
        return true;
    }

    bool ok = disabled
        ? charge_limiter_dac_session_resume(dac_saved_charger_enabled)
        : charge_limiter_dac_session_release(dac_saved_charger_enabled);
    if (!ok && dac_charging_disabled)
        (void) charge_limiter_dac_session_resume(dac_saved_charger_enabled);
    else if (!ok)
        (void) charge_limiter_dac_session_release(dac_saved_charger_enabled);
    if (ok) dac_charging_disabled = disabled;
    else fprintf(stderr, "usb_mode_control: DAC charging preference transition failed; retaining previous preference\n");
    pthread_mutex_unlock(&usb_mode_mutex);
    return ok;
#else
    (void) disabled;
    return true;
#endif
}

bool usb_mode_control_apply_with_status(usb_mode_t mode, bool * storage_busy) {
    if (storage_busy) *storage_busy = false;
    pthread_mutex_lock(&usb_mode_mutex);
    if (storage_write_lease_count != 0) {
        if (storage_busy) *storage_busy = true;
        pthread_mutex_unlock(&usb_mode_mutex);
        fprintf(stderr, "usb_mode_control: mode switch rejected while SD write lease is active\n");
        return false;
    }
    bool applied = usb_mode_control_apply_locked(mode);
    pthread_mutex_unlock(&usb_mode_mutex);
    return applied;
}

bool usb_mode_control_apply(usb_mode_t mode) {
    return usb_mode_control_apply_with_status(mode, NULL);
}

bool usb_mode_control_detect_current(usb_mode_t * out_mode) {
    if (udc_is_bound(ADB_DEMO_UDC_PATH) && function_linked(ADB_DEMO_DIR, "ffs.")) {
        *out_mode = USB_MODE_ADB;
        return true;
    }
    if (udc_is_bound(ANDROID0_UDC_PATH)) {
        if (function_linked(ANDROID0_DIR, "uac_sa.")) {
            *out_mode = USB_MODE_DAC;
            return true;
        }
        if (function_linked(ANDROID0_DIR, "mass_storage.")) {
            *out_mode = USB_MODE_STORAGE;
            return true;
        }
    }
    return false;
}

/* Read a complete bounded sysfs value, accepting its optional final newline.
 * Reject truncation, embedded NULs and read errors rather than matching a
 * valid-looking prefix of unavailable or malformed state. */
static bool read_storage_host_value(const char * path, char * out, size_t size) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    size_t len = fread(out, 1, size - 1, f);
    bool ok = !ferror(f) && feof(f) && memchr(out, '\0', len) == NULL;
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;
    if (len && out[len - 1] == '\n') --len;
    out[len] = '\0';
    return len != 0;
}

typedef enum {
    UDC_VALUE_ERROR,
    UDC_VALUE_UNBOUND,
    UDC_VALUE_BOUND
} udc_value_kind_t;

static bool read_udc_value(const char * path, char * out, size_t size, udc_value_kind_t * kind) {
    if (!path || !out || size < 2 || !kind) return false;
    out[0] = '\0';
    FILE * file = fopen(path, "r");
    if (!file) return false;
    size_t n = fread(out, 1, size - 1, file);
    bool ok = !ferror(file) && feof(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    if (!n || strcmp(out, "none") == 0) {
        *kind = UDC_VALUE_UNBOUND;
        return true;
    }
    if (n > 255) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char) out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    *kind = UDC_VALUE_BOUND;
    return true;
}

static bool write_udc_checked(const char * path, const char * value) {
    FILE * file = fopen(path, "w");
    if (!file) return false;
    bool ok = fputs(value, file) >= 0 && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool wait_udc_value(const char * path, udc_value_kind_t wanted_kind,
                           const char * wanted_value) {
    for (int i = 0; i < SETTLE_RETRY_LIMIT; ++i) {
        char actual[256];
        udc_value_kind_t kind;
        if (read_udc_value(path, actual, sizeof(actual), &kind) && kind == wanted_kind &&
            (wanted_kind != UDC_VALUE_BOUND || (wanted_value && strcmp(actual, wanted_value) == 0)))
            return true;
        usleep(50000);
    }
    return false;
}

static bool storage_udc_state_idle(const char * controller) {
    char path[320], state[32];
    int n = snprintf(path, sizeof(path), "%s/%s/state", USB_MODE_CONTROL_UDC_STATE_DIR, controller);
    if (n <= 0 || n >= (int) sizeof(path) || !read_storage_host_value(path, state, sizeof(state))) return false;
    static const char * const valid_states[] = {
        "not attached", "attached", "powered", "default", "address"
    };
    for (size_t i = 0; i < sizeof(valid_states) / sizeof(valid_states[0]); ++i)
        if (strcmp(state, valid_states[i]) == 0) return true;
    /* configured, suspended, and unknown states may still represent a host
     * that owns the medium; do not revoke its gadget connection. */
    return false;
}

/* Refuse to restore Storage if any other configured gadget is bound. */
static bool other_gadget_bound(void) {
    DIR * dir = opendir(USB_MODE_CONTROL_GADGET_ROOT);
    if (!dir) return true; /* Cannot prove the controller is free. */
    bool busy = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strcmp(entry->d_name, "android0") == 0) continue;
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s/UDC", USB_MODE_CONTROL_GADGET_ROOT, entry->d_name);
        if (n <= 0 || n >= (int) sizeof(path)) { busy = true; break; }
        char value[256];
        udc_value_kind_t kind;
        if (!read_udc_value(path, value, sizeof(value), &kind) || kind == UDC_VALUE_BOUND) {
            busy = true;
            break;
        }
    }
    closedir(dir);
    return busy;
}

bool usb_mode_control_storage_write_blocked(void) {
    if (!path_exists(ANDROID0_DIR)) return false;
    char controller[256];
    udc_value_kind_t kind;
    if (!read_udc_value(ANDROID0_UDC_PATH, controller, sizeof(controller), &kind)) return true;
    if (kind == UDC_VALUE_UNBOUND) return false;
    bool storage = function_linked(ANDROID0_DIR, "mass_storage.");
    if (!storage && function_linked(ANDROID0_DIR, "uac_sa.")) return false;
    return !storage || !storage_udc_state_idle(controller);
}

bool usb_mode_control_storage_write_begin(void) {
    pthread_mutex_lock(&usb_mode_mutex);
    if (storage_write_lease_count) {
        storage_write_lease_count++;
        atomic_store_explicit(&storage_write_active_count, storage_write_lease_count, memory_order_release);
        pthread_mutex_unlock(&usb_mode_mutex);
        return true;
    }
#ifdef HOST_BUILD
    storage_write_lease_count = 1;
    atomic_store_explicit(&storage_write_active_count, 1, memory_order_release);
    pthread_mutex_unlock(&usb_mode_mutex);
    return true; /* Simulate exclusion without touching host gadget/configfs paths. */
#endif
    storage_write_restore_needed = false;
    storage_write_saved_controller[0] = '\0';

    bool android0_exists = path_exists(ANDROID0_DIR);
    bool storage_link = function_linked(ANDROID0_DIR, "mass_storage.");
    bool dac_link = function_linked(ANDROID0_DIR, "uac_sa.");
    char controller[256];
    udc_value_kind_t kind = UDC_VALUE_ERROR;
    bool udc_readable = !android0_exists || read_udc_value(ANDROID0_UDC_PATH, controller, sizeof(controller), &kind);
    if (!udc_readable || (kind == UDC_VALUE_BOUND && !storage_link && !dac_link)) {
        pthread_mutex_unlock(&usb_mode_mutex);
        return false;
    }
    if (storage_link && kind == UDC_VALUE_BOUND) {
        if (!storage_udc_state_idle(controller)) {
            pthread_mutex_unlock(&usb_mode_mutex);
            return false;
        }
        if (!write_udc_checked(ANDROID0_UDC_PATH, "\n") ||
            !wait_udc_value(ANDROID0_UDC_PATH, UDC_VALUE_UNBOUND, NULL)) {
            /* If the write took but verification failed transiently, put the
             * same UDC back only after confirming no gadget was rebound. */
            char now[256];
            udc_value_kind_t now_kind;
            if (read_udc_value(ANDROID0_UDC_PATH, now, sizeof(now), &now_kind) &&
                now_kind == UDC_VALUE_UNBOUND && !udc_is_bound(ADB_DEMO_UDC_PATH)) {
                (void)write_udc_checked(ANDROID0_UDC_PATH, "\n");
                char bind_value[sizeof(controller) + 2];
                snprintf(bind_value, sizeof(bind_value), "%s\n", controller);
                (void)write_udc_checked(ANDROID0_UDC_PATH, bind_value);
            }
            pthread_mutex_unlock(&usb_mode_mutex);
            return false;
        }
        snprintf(storage_write_saved_controller, sizeof(storage_write_saved_controller), "%s", controller);
        storage_write_restore_needed = true;
    }
    storage_write_lease_count = 1;
    atomic_store_explicit(&storage_write_active_count, 1, memory_order_release);
    pthread_mutex_unlock(&usb_mode_mutex);
    return true;
}

void usb_mode_control_storage_write_end(void) {
    pthread_mutex_lock(&usb_mode_mutex);
    if (!storage_write_lease_count) {
        pthread_mutex_unlock(&usb_mode_mutex);
        return;
    }
    if (--storage_write_lease_count) {
        atomic_store_explicit(&storage_write_active_count, storage_write_lease_count, memory_order_release);
        pthread_mutex_unlock(&usb_mode_mutex);
        return;
    }
    bool restore = storage_write_restore_needed;
    char controller[sizeof(storage_write_saved_controller)];
    snprintf(controller, sizeof(controller), "%s", storage_write_saved_controller);
    storage_write_restore_needed = false;
    storage_write_saved_controller[0] = '\0';
    if (restore) {
        sync(); /* Flush all plugin/scan writes before exposing the volume again. */
        if (udc_is_bound(ANDROID0_UDC_PATH) || other_gadget_bound()) {
            fprintf(stderr, "usb_mode_control: cannot restore Storage UDC; another gadget is bound\n");
        } else {
            char bind_value[sizeof(controller) + 2];
            snprintf(bind_value, sizeof(bind_value), "%s\n", controller);
            if (!write_udc_checked(ANDROID0_UDC_PATH, bind_value) ||
                   !wait_udc_value(ANDROID0_UDC_PATH, UDC_VALUE_BOUND, controller)) {
                fprintf(stderr, "usb_mode_control: failed to restore Storage UDC %s\n", controller);
            }
        }
    }
    atomic_store_explicit(&storage_write_active_count, 0, memory_order_release);
    pthread_mutex_unlock(&usb_mode_mutex);
}

bool usb_mode_control_storage_write_active(void) {
    bool active = atomic_load_explicit(&storage_write_active_count, memory_order_acquire) != 0;
    return active;
}

bool usb_mode_control_storage_host_configured(void) {
    usb_mode_t mode;
    if (!usb_mode_control_detect_current(&mode) || mode != USB_MODE_STORAGE) return false;

    char controller[258]; /* NAME_MAX controller plus optional newline and NUL. */
    if (!read_storage_host_value(ANDROID0_UDC_PATH, controller, sizeof(controller))) return false;
    size_t len = strlen(controller);
    if (len > 255 || strcmp(controller, "none") == 0 ||
        strcmp(controller, ".") == 0 || strcmp(controller, "..") == 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)controller[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s/state", USB_MODE_CONTROL_UDC_STATE_DIR, controller);
    char state[32];
    return read_storage_host_value(path, state, sizeof(state)) &&
           strcmp(state, "configured") == 0;
}
