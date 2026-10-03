#include "firmware_update.h"
#include "input_device_utils.h"
#include "firmware_image.h"
#include "gui_library.h"
#include "storage_paths.h"
#include "battery.h"
#include "i18n.h"
#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HOST_BUILD
#include <sys/reboot.h>
#endif

#ifdef HOST_BUILD
  #define FIRMWARE_UPDATE_SD_ROOT "./music"
#else
  #define FIRMWARE_UPDATE_SD_ROOT "/data/mnt/sd_0"
#endif

static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
static firmware_update_status_t update_status;
static enum { UPDATE_OWNER_NONE, UPDATE_OWNER_MANUAL, UPDATE_OWNER_OTA } update_owner;
static bool recovery_from_ota;
static char manual_path[512];
static int phase_log_fd = -1; /* owned by the single recovery worker */

static const char * board_image(void) {
#if defined(BOARD_R3PROII)
    return "r3proii.upt";
#elif defined(BOARD_R3II_2025)
    return "r3ii_2025.upt";
#else
    return "r1.upt";
#endif
}

static bool is_upt_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcasecmp(ext, ".upt") == 0;
}

void firmware_update_get_status(firmware_update_status_t * out) {
    pthread_mutex_lock(&update_mutex);
    *out = update_status;
    pthread_mutex_unlock(&update_mutex);
}

bool firmware_update_busy(void) {
    pthread_mutex_lock(&update_mutex);
    bool busy = update_owner != UPDATE_OWNER_NONE;
    pthread_mutex_unlock(&update_mutex);
    return busy;
}

bool firmware_update_claim_ota(void) {
    pthread_mutex_lock(&update_mutex);
    bool ok = update_owner == UPDATE_OWNER_NONE;
    if (ok) update_owner = UPDATE_OWNER_OTA;
    pthread_mutex_unlock(&update_mutex);
    return ok;
}

void firmware_update_release_ota(void) {
    pthread_mutex_lock(&update_mutex);
    if (update_owner == UPDATE_OWNER_OTA) update_owner = UPDATE_OWNER_NONE;
    pthread_mutex_unlock(&update_mutex);
}

static bool set_phase(firmware_update_phase_t phase, const char * detail) {
    pthread_mutex_lock(&update_mutex);
    update_status.phase = phase;
    update_status.delayed = false;
    update_status.error[0] = '\0';
    pthread_mutex_unlock(&update_mutex);
    fprintf(stderr, "firmware_update: %s\n", detail);
    if (phase_log_fd >= 0) {
        return dprintf(phase_log_fd, "%ld phase=%d %s\n", (long) time(NULL), phase, detail) >= 0 && fsync(phase_log_fd) == 0;
    }
    return false;
}

static bool finish_update(bool ok, const char * error) {
    if (!ok) set_phase(FIRMWARE_UPDATE_FAILED, error);
    if (phase_log_fd >= 0) { close(phase_log_fd); phase_log_fd = -1; }
    pthread_mutex_lock(&update_mutex);
    if (error) snprintf(update_status.error, sizeof(update_status.error), "%s", error);
    update_status.busy = false;
    /* The OTA worker must publish its result before releasing its claim;
     * otherwise a new request can reset this error or steal that claim. */
    update_owner = recovery_from_ota ? UPDATE_OWNER_OTA : UPDATE_OWNER_NONE;
    pthread_mutex_unlock(&update_mutex);
    return ok;
}

static bool reserve_update(bool from_ota) {
    pthread_mutex_lock(&update_mutex);
    if (update_owner != (from_ota ? UPDATE_OWNER_OTA : UPDATE_OWNER_NONE)) { pthread_mutex_unlock(&update_mutex); return false; }
    update_owner = UPDATE_OWNER_MANUAL;
    recovery_from_ota = from_ota;
    memset(&update_status, 0, sizeof(update_status));
    update_status.busy = true;
    update_status.phase = FIRMWARE_UPDATE_VALIDATING;
    pthread_mutex_unlock(&update_mutex);
    return true;
}

bool firmware_update_scan_checked(char * out_path, size_t out_size, char * error, size_t error_size) {
    if (out_size) out_path[0] = '\0';
    if (!sd_card_root_is_mounted()) {
        snprintf(error, error_size, TR("No SD card is mounted."));
        return false;
    }
    DIR * dir = opendir(FIRMWARE_UPDATE_SD_ROOT);
    if (!dir) { snprintf(error, error_size, TR("Cannot read the SD card.")); return false; }
    int count = 0;
    bool valid = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) { if (errno) valid = false; break; }
        if (de->d_name[0] == '.' || !is_upt_file(de->d_name)) continue;
        count++;
        if (strcmp(de->d_name, board_image()) != 0) valid = false;
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", FIRMWARE_UPDATE_SD_ROOT, de->d_name);
        struct stat st;
        if (n < 0 || (size_t) n >= sizeof(path) || lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) valid = false;
        if (count == 1 && out_size) {
            n = snprintf(out_path, out_size, "%s", path);
            if (n < 0 || (size_t) n >= out_size) valid = false;
        }
    }
    closedir(dir);
    if (count == 1 && valid) return true;
    if (!count) snprintf(error, error_size, TR("No .upt firmware file found on SD card"));
    else if (count > 1) snprintf(error, error_size, TR("Keep only one .upt file in the SD card root."));
    else snprintf(error, error_size, TR("Use a regular %s file for this player. Check the SD card."), board_image());
    if (out_size) out_path[0] = '\0';
    return false;
}

bool firmware_update_scan(char * out_path, size_t out_size) {
    char error[160];
    return firmware_update_scan_checked(out_path, out_size, error, sizeof(error));
}

#ifndef HOST_BUILD
extern char ** environ;

/* Prepare descriptor closures in the parent. posix_spawn avoids libc directory
 * allocation in a forked, multithreaded child and tools inherit no player fds.
 * Never terminate NAND helpers on a UI deadline: let the operation finish and
 * keep the single-flight reservation until it has actually been reaped. */
static bool run_update_tool(char * const argv[], const char * input, size_t input_size, char * error, size_t size) {
    int input_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input) {
        int fds[2];
        if (input_fd >= 0) close(input_fd);
        if (pipe(fds) != 0) return false;
        ssize_t n = write(fds[1], input, input_size);
        close(fds[1]);
        if (n != (ssize_t) input_size) { close(fds[0]); return false; }
        input_fd = fds[0];
    }
    if (input_fd < 0) return false;
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) { close(input_fd); snprintf(error, size, TR("Could not prepare update helper: %s"), strerror(rc)); return false; }
    rc = posix_spawn_file_actions_adddup2(&actions, input_fd, STDIN_FILENO);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, phase_log_fd, STDOUT_FILENO);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, phase_log_fd, STDERR_FILENO);
    DIR * fds = opendir("/proc/self/fd");
    if (!fds) rc = errno;
    if (fds) {
        struct dirent * entry;
        while (!rc) {
            errno = 0;
            entry = readdir(fds);
            if (!entry) { if (errno) rc = errno; break; }
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            int fd = atoi(entry->d_name);
            if (fd > STDERR_FILENO && fd != dirfd(fds)) rc = posix_spawn_file_actions_addclose(&actions, fd);
        }
        closedir(fds);
    }
    pid_t pid;
    if (!rc) rc = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input_fd);
    if (rc) { snprintf(error, size, TR("Could not start update helper: %s"), strerror(rc)); return false; }
    int waited = 0;
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            dprintf(phase_log_fd, "helper=%s status=%d\n", argv[0], status);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            snprintf(error, size, TR("Update helper failed (%s, status %d). See .compas/ota/update.log."), argv[0], status);
            return false;
        }
        if (r < 0 && errno != EINTR) {
            snprintf(error, size, TR("Cannot read update helper status: %s"), strerror(errno));
            return false;
        }
        usleep(100000);
        if (++waited == 150) {
            pthread_mutex_lock(&update_mutex);
            update_status.delayed = true;
            snprintf(update_status.error, sizeof(update_status.error), "%s", TR("Still working. Do not turn off the player or retry."));
            pthread_mutex_unlock(&update_mutex);
            dprintf(phase_log_fd, "helper=%s delayed; waiting without interrupting it\n", argv[0]);
        }
    }
}

static bool recovery_flag_partition(void) {
    FILE * f = fopen("/proc/mtd", "r");
    if (!f) return false;
    char line[128];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned int size, erase;
        char name[32];
        if (sscanf(line, "mtd5: %x %x \"%31[^\"]\"", &size, &erase, name) == 3 &&
            strcmp(name, "ota") == 0 && size >= 256 && erase >= 256) ok = true;
    }
    fclose(f);
    return ok;
}

static bool write_recovery_flag(char * error, size_t size) {
    if (!recovery_flag_partition()) {
        snprintf(error, size, TR("Recovery flag partition does not match this firmware."));
        return false;
    }
    int fd = open("/dev/mtd5", O_RDONLY | O_CLOEXEC);
    struct stat st;
    int64_t offset = 0;
    /* MEMGETBADBLOCK = _IOW('M', 11, __kernel_loff_t), from Linux mtd ABI.
     * Refuse a bad first block rather than read/write different offsets. */
    if (fd < 0 || fstat(fd, &st) || !S_ISCHR(st.st_mode) ||
        ioctl(fd, _IOW('M', 11, int64_t), &offset) != 0) {
        if (fd >= 0) close(fd);
        snprintf(error, size, TR("Cannot safely access the recovery flag block."));
        return false;
    }
    close(fd);
    char * erase_argv[] = { "/usr/sbin/flash_erase", "/dev/mtd5", "0", "1", NULL };
    if (!run_update_tool(erase_argv, NULL, 0, error, size)) {
        snprintf(error, size, TR("Recovery flag erase failed. Do not reboot; check .compas/ota/update.log."));
        return false;
    }
    char flag[256];
    memset(flag, ' ', sizeof(flag));
    memcpy(flag, "ota:kernel2", 11);
    char * write_argv[] = { "/usr/sbin/nandwrite", "-s", "0", "-p", "/dev/mtd5", "-", NULL };
    if (!run_update_tool(write_argv, flag, sizeof(flag), error, size)) {
        snprintf(error, size, TR("Recovery flag write failed. Do not reboot; check .compas/ota/update.log."));
        return false;
    }
    char actual[256];
    fd = open("/dev/mtd5", O_RDONLY | O_CLOEXEC);
    bool ok = fd >= 0 && read(fd, actual, sizeof(actual)) == sizeof(actual) && !memcmp(actual, flag, sizeof(flag));
    if (fd >= 0) close(fd);
    if (!ok) snprintf(error, size, TR("Recovery flag verification failed. Do not reboot; check the update log."));
    return ok;
}
#endif

static bool prepare_recovery(const char * path) {
    char selected[512], error[160] = "";
    struct stat before, after;
    if (!firmware_update_scan_checked(selected, sizeof(selected), error, sizeof(error)) || strcmp(path, selected))
        return finish_update(false, error[0] ? error : TR("The selected update file changed. Select it again."));
    if (lstat(path, &before) != 0) return finish_update(false, TR("Cannot read the selected update file."));
    struct stat dir_st;
    /* Logs are on the verified mounted card; never fall back to RAM. */
    if (mkdir(SD_COMPAS_ROOT, 0755) != 0 && errno != EEXIST)
        return finish_update(false, TR("Cannot create the update log directory."));
    if (lstat(SD_COMPAS_ROOT, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) || dir_st.st_dev != before.st_dev)
        return finish_update(false, TR("Update diagnostics must be stored on the SD card."));
    if (mkdir(SD_COMPAS_ROOT "/ota", 0755) != 0 && errno != EEXIST)
        return finish_update(false, TR("Cannot create the update log directory."));
    if (lstat(SD_COMPAS_ROOT "/ota", &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) || dir_st.st_dev != before.st_dev)
        return finish_update(false, TR("Update diagnostics must be stored on the SD card."));
    phase_log_fd = open(SD_COMPAS_ROOT "/ota/update.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (phase_log_fd < 0) return finish_update(false, TR("Cannot save update diagnostics on the SD card."));
    struct stat log_st;
    if (fstat(phase_log_fd, &log_st) != 0 || log_st.st_dev != before.st_dev ||
        lstat(SD_COMPAS_ROOT, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) ||
        lstat(SD_COMPAS_ROOT "/ota", &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
        return finish_update(false, TR("Update diagnostics must be stored on the SD card."));
    if (!set_phase(FIRMWARE_UPDATE_VALIDATING, "validating selected package"))
        return finish_update(false, TR("Cannot save update diagnostics on the SD card."));
    if (!firmware_image_validate(path, error, sizeof(error))) return finish_update(false, error);
    /* Re-check the directory after the long read, before any NAND change. */
    if (!firmware_update_scan_checked(selected, sizeof(selected), error, sizeof(error)) || strcmp(path, selected))
        return finish_update(false, error[0] ? error : TR("The selected update file changed. Select it again."));
    if (lstat(path, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return finish_update(false, TR("The update file changed during verification. Select it again."));
#ifndef HOST_BUILD
    if (access("/usr/sbin/flash_erase", X_OK) || access("/usr/sbin/nandwrite", X_OK) || access("/bin/sync", X_OK))
        return finish_update(false, TR("A required recovery helper is missing. Update preparation stopped."));
    if (!set_phase(FIRMWARE_UPDATE_BOOTFLAG, "writing and verifying recovery boot flag"))
        return finish_update(false, TR("Cannot save update diagnostics. Update preparation stopped."));
    if (!write_recovery_flag(error, sizeof(error))) return finish_update(false, error[0] ? error : TR("Recovery boot flag failed."));
    if (!set_phase(FIRMWARE_UPDATE_SYNC, "flushing storage before reboot"))
        return finish_update(false, TR("Recovery remains selected. Keep the update file; saving diagnostics failed."));
    char * sync_argv[] = { "/bin/sync", NULL };
    if (!run_update_tool(sync_argv, NULL, 0, error, sizeof(error)))
        return finish_update(false, TR("Saving data failed. Recovery remains selected; keep the update file and check .compas/ota/update.log."));
    if (!set_phase(FIRMWARE_UPDATE_REBOOT, "requesting recovery reboot"))
        return finish_update(false, TR("Recovery remains selected. Keep the update file; saving diagnostics failed."));
    reboot(RB_AUTOBOOT);
    snprintf(error, sizeof(error), TR("Recovery remains selected but reboot failed: %s. Keep the update file."), strerror(errno));
    return finish_update(false, error);
#else
    set_phase(FIRMWARE_UPDATE_REBOOT, "host validation complete; no flash or reboot");
    return finish_update(true, NULL);
#endif
}

bool firmware_update_enter_recovery_for_path(const char * path) {
    if (!path || !reserve_update(false)) return false;
    return prepare_recovery(path);
}

bool firmware_update_enter_ota_recovery_for_path(const char * path) {
    if (!path || !reserve_update(true)) return false;
    return prepare_recovery(path);
}

void firmware_update_enter_recovery(void) {
    char path[512];
    if (firmware_update_scan(path, sizeof(path))) firmware_update_enter_recovery_for_path(path);
}

static void * manual_worker(void * unused) {
    (void) unused;
    prepare_recovery(manual_path);
    return NULL;
}

bool firmware_update_start(const char * path, char * error, size_t error_size) {
    if (!reserve_update(false)) {
        snprintf(error, error_size, TR("An update is already in progress."));
        return false;
    }
    int battery = battery_get_percent();
#ifdef HOST_BUILD
    if (battery < 0) battery = 100;
#endif
    if (battery < 30 && !battery_is_charging()) {
        snprintf(error, error_size, TR("Charge to at least 30%% or connect power before updating."));
        finish_update(false, error);
        return false;
    }
    if (!path || strlen(path) >= sizeof(manual_path)) {
        snprintf(error, error_size, TR("Invalid update path."));
        finish_update(false, error);
        return false;
    }
    snprintf(manual_path, sizeof(manual_path), "%s", path);
    pthread_t thread;
    if (pthread_create(&thread, NULL, manual_worker, NULL) != 0) {
        snprintf(error, error_size, TR("Could not start the update."));
        finish_update(false, error);
        return false;
    }
    pthread_detach(thread);
    return true;
}

#define KEY_BITS_PER_LONG (sizeof(unsigned long) * 8)
#define KEY_BITS_ARRAY_LEN ((KEY_MAX / KEY_BITS_PER_LONG) + 1)

static bool device_reports_key_down(const char * path, int keycode) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    unsigned long keys[KEY_BITS_ARRAY_LEN];
    memset(keys, 0, sizeof(keys));
    bool down = false;
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
        down = (keys[keycode / KEY_BITS_PER_LONG] >> (keycode % KEY_BITS_PER_LONG)) & 1UL;
    }
    close(fd);
    return down;
}

void firmware_update_check_boot_combo(void) {
    char gpio_keys_path[64];
    char adc_keyboard_path[64];
    /* Power lives on "md-gpio-keys", Volume Up lives on "jz adc keyboard" --
     * same device split hw_buttons.c found and documented (see its own
     * comment on why the two live on separate evdev nodes on this board). */
    if (!find_input_device_by_name("md-gpio-keys", gpio_keys_path, sizeof(gpio_keys_path))) return;
    if (!find_input_device_by_name("jz adc keyboard", adc_keyboard_path, sizeof(adc_keyboard_path))) return;

#if defined(BOARD_R3II_2025)
    /* No volume key here (the knob only pulses); the stock combo is Play/Pause
     * + Power, and Play/Pause lives on "jz adc keyboard". */
    #define FIRMWARE_UPDATE_COMBO_KEY KEY_PLAYPAUSE
#else
    #define FIRMWARE_UPDATE_COMBO_KEY KEY_VOLUMEUP
#endif
    if (!device_reports_key_down(gpio_keys_path, KEY_POWER)) return;
    if (!device_reports_key_down(adc_keyboard_path, FIRMWARE_UPDATE_COMBO_KEY)) return;

    char upt_path[512];
    if (!firmware_update_scan(upt_path, sizeof(upt_path))) return;

    fprintf(stderr, "firmware_update: boot combo held and %s found, entering recovery\n", upt_path);
    firmware_update_enter_recovery();
}
