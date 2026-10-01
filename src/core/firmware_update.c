#include "firmware_update.h"
#include "input_device_utils.h"
#include "subprocess.h"

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

static bool is_upt_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcasecmp(ext, ".upt") == 0;
}

bool firmware_update_scan(char * out_path, size_t out_size) {
    DIR * dir = opendir(FIRMWARE_UPDATE_SD_ROOT);
    if (!dir) return false;

    bool found = false;
    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_upt_file(de->d_name)) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", FIRMWARE_UPDATE_SD_ROOT, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        snprintf(out_path, out_size, "%s", full_path);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

void firmware_update_enter_recovery(void) {
    char * bootmode_argv[] = { "/usr/bin/bootmode.sh", "Recovery", NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(bootmode_argv, NULL, 0, 15000, &exit_code) || exit_code != 0) {
        fprintf(stderr, "firmware_update: recovery boot flag failed (exit %d)\n", exit_code);
        return;
    }

#ifndef HOST_BUILD
    sync();
    /* A successful reboot does not return. On failure keep the player alive:
     * replacing it with the reboot utility can trigger the supervisor's
     * clean-exit poweroff, and waiting forever hides the failure from users. */
    reboot(RB_AUTOBOOT);
    fprintf(stderr, "firmware_update: reboot failed: %s\n", strerror(errno));
#endif
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
