#include "idle_shutdown.h"
#include "../../updates/firmware_update.h"
#include "../../persistence/settings.h"
#include "reboot_handoff.h"
#include <stdio.h>
#ifndef HOST_BUILD
#include "../../../bootloader/selector/ui/controller/selector_request.h"
#endif

static void selector_error(char * error, size_t error_size, const char * message) {
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message);
}

#ifndef HOST_BUILD
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    return strncmp(base, "compas_loader", strlen("compas_loader")) == 0 ||
           strncmp(base, "compas_bootloader", strlen("compas_bootloader")) == 0 ||
           strncmp(base, "compas_rockbox_bootloader", strlen("compas_rockbox_bootloader")) == 0;
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
    /* Standalone player launches have no supervisor to reap them. Ask init
     * to run its orderly shutdown hooks before the bounded raw fallback. */
    (void)reboot_request_orderly_shutdown(NULL);
    if (reboot(RB_AUTOBOOT) == 0) return true;
    /* Terminal callers keep saves latched. A caller returning to the live UI
     * must explicitly cancel shutdown after recording the syscall errno. */
    return false;
#else
    return false;
#endif
}

bool idle_shutdown_reboot_to_bootloader(char * error, size_t error_size) {
#ifdef HOST_BUILD
    selector_error(error, error_size, "Boot selector restart is unavailable on this host");
    return false;
#else
    if (firmware_update_busy()) {
        selector_error(error, error_size, "An update is in progress. Please wait.");
        return false;
    }
    extern void gui_player_queue_flush(void);
    settings_shutdown_flush();
    gui_player_queue_flush();
    if (!selector_request_write(error, error_size)) {
        settings_shutdown_cancel();
        return false;
    }
    sync();
    if (idle_shutdown_reboot_handoff()) return true;
    bool cancelled = selector_request_cancel();
    settings_shutdown_cancel();
    selector_error(error, error_size, cancelled ? "Could not restart into the boot selector" :
                   "Restart failed; the next restart will open the boot selector");
    return false;
#endif
}
