#ifndef IDLE_SHUTDOWN_H
#define IDLE_SHUTDOWN_H

#include <stdbool.h>

/* Full poweroff, the same mechanism the stock firmware's own "Idle
 * shutdown" setting uses (confirmed via `strings` on the stock binary:
 * /sbin/poweroff, driven by its power_save_shutdown_timer). Real
 * suspend-to-RAM exists at the kernel level on this hardware but its
 * display/WiFi resume path is broken (confirmed on a real device: the jzfb
 * driver spins forever on "pan display wait timeout" after resume, leaving
 * the screen blank while the backlight stays on), so a full poweroff is the
 * only reliable way to actually cut standby power draw -- see gui.c's
 * idle-shutdown timer for the inactivity gating (screen off, not playing,
 * not charging) before this gets called. Replaces this process with
 * busybox `poweroff` (then the reboot(RB_POWER_OFF) syscall if exec
 * fails) so it cannot return on the device. Must not use subprocess_run():
 * that helper SIGKILLs its child after 15s, which cancelled poweroff and
 * left the countdown looking finished while the device stayed on. No-op
 * on host. */
void idle_shutdown_now(void);

/* Exit status idle_shutdown_reboot_handoff() uses when its parent is
 * compas_bootloader. Non-zero so the supervisor reboots (after releasing
 * the SD card) instead of treating the exit as a clean poweroff. The
 * bootloader's player_exit.log marks this value as a requested restart. */
#define IDLE_SHUTDOWN_REBOOT_EXIT_CODE 75

/* Normal reboot entry point. Retains the update-busy guard and drains the
 * player queue before using the shared reboot handoff. No-op on host. */
void idle_shutdown_reboot_now(void);

/* Flush terminal settings and hand off a reboot. On device, exits with
 * IDLE_SHUTDOWN_REBOOT_EXIT_CODE when supervised by compas_bootloader;
 * otherwise syncs and invokes reboot(RB_AUTOBOOT). Returns false with errno
 * preserved if the syscall fails; settings remain latched. A caller returning
 * to the live UI must call settings_shutdown_cancel(). Host builds return
 * false harmlessly. This
 * lower-level entry point intentionally has no update-busy guard because the
 * recovery updater calls it while holding the update reservation. */
bool idle_shutdown_reboot_handoff(void);

#endif /* IDLE_SHUTDOWN_H */
