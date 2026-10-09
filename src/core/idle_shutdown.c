#include "idle_shutdown.h"
#include "firmware_update.h"
#include "settings.h"

#ifndef HOST_BUILD
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

static bool parent_is_bootloader(void) {
    char path[32];
    char target[256];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) getppid());
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) return false;
    target[length] = '\0';
    const char * base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strncmp(base, "compas_bootloader", strlen("compas_bootloader")) == 0;
}
#endif

void idle_shutdown_now(void) {
    if (firmware_update_busy()) return;
#ifndef HOST_BUILD
    settings_shutdown_flush();
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    /* Must not go through subprocess_run(): that helper waits 15s then
     * SIGKILLs the child. /sbin/poweroff (busybox, talks to init or waits
     * on other processes) routinely outlives that budget, so the 3-2-1
     * countdown reached zero, the child was killed, and the device stayed
     * on -- only a hardware power-button hold actually cut power. */
    sync();
    execl("/sbin/poweroff", "poweroff", (char *) NULL);
    reboot(RB_POWER_OFF);
    for (;;) pause();
#endif
}

void idle_shutdown_reboot_now(void) {
    if (firmware_update_busy()) return;
#ifndef HOST_BUILD
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    if (idle_shutdown_reboot_handoff()) return;
    for (;;) pause();
#endif
}

bool idle_shutdown_reboot_handoff(void) {
#ifndef HOST_BUILD
    settings_shutdown_flush();
    /* Let the supervisor release the SD card before restarting it. The
     * supervisor recognizes this status as a requested reboot. */
    if (parent_is_bootloader()) _exit(IDLE_SHUTDOWN_REBOOT_EXIT_CODE);
    sync();
    if (reboot(RB_AUTOBOOT) == 0) return true;
    /* Terminal callers keep saves latched. A caller returning to the live UI
     * must explicitly cancel shutdown after recording this syscall's errno. */
    return false;
#else
    return false;
#endif
}
