#ifndef IDLE_SHUTDOWN_H
#define IDLE_SHUTDOWN_H

#include <stdbool.h>
#include <stddef.h>

/* Request poweroff and return to the UI. Queue checkpoints are polled; rating,
 * numeric and settings persistence runs on a worker. Failure or a five-second
 * persistence timeout cancels the action and reports an error. A stalled worker
 * is retained until it finishes, without blocking the UI or powering off late.
 * No-op on host. */
void idle_shutdown_now(void);
/* True while a requested shutdown is still being prepared or committed. After
 * idle_shutdown_now() it turns false again only if the request was refused or
 * cancelled. Always false on host. */
bool idle_shutdown_pending(void);

/* Exit status idle_shutdown_reboot_handoff() uses when its parent is
 * compas_loader. Non-zero so the supervisor reboots (after releasing
 * the SD card) instead of treating the exit as a clean poweroff. The
 * bootloader's player_exit.log marks this value as a requested restart. */
#define IDLE_SHUTDOWN_REBOOT_EXIT_CODE 75

/* Normal reboot entry point. Retains the update-busy guard and drains the
 * player queue before using the shared reboot handoff. No-op on host. */
void idle_shutdown_reboot_now(void);
/* Queue a factory reset behind the same asynchronous persistence gate. */
void idle_shutdown_factory_reset(void);

/* Hand off a reboot after the caller has drained terminal persistence. On device, exits with
 * IDLE_SHUTDOWN_REBOOT_EXIT_CODE when supervised by compas_loader (or the previous compas_bootloader);
 * otherwise syncs, asks BusyBox init to run its shutdown scripts when present,
 * waits up to 20 seconds, then invokes the kernel reboot syscall. Returns
 * false with errno preserved if the syscall fails; settings remain latched.
 * A caller returning to the live UI must call settings_shutdown_cancel().
 * Host builds return false harmlessly. This
 * lower-level entry point intentionally has no update-busy guard because the
 * recovery updater calls it while holding the update reservation. */
bool idle_shutdown_reboot_handoff(void);

/* Worker request and UI-thread poll for recovery persistence preparation. */
void idle_shutdown_recovery_reboot(void);
void idle_shutdown_poll_recovery(void);

/* Reboot into the boot selector using a durable one-shot marker on internal
 * storage. Returns whether preparation was scheduled; later persistence or
 * restart failure is reported on the UI. Host builds never write or reboot. */
bool idle_shutdown_reboot_to_bootloader(char * error, size_t error_size);

#endif /* IDLE_SHUTDOWN_H */
