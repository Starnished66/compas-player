#ifndef BOOTLOADER_ROCKBOX_BRIDGE_H
#define BOOTLOADER_ROCKBOX_BRIDGE_H

#include <stdbool.h>

/* Uses the shared Compás splash and SD preparation, then closes the
 * framebuffer for Rockbox. */
void compas_boot_prepare(void);

/* Rechecks/mounts SD through the normal bounded Compás readiness logic. */
void compas_boot_mount_sd(void);

/* True only when this board has a matching, complete Rockbox installation. */
bool compas_rockbox_available(void);

/* Copies the validated Rockbox executable to persistent internal storage and
 * returns its path. Returns NULL on any validation or staging failure. */
const char * compas_rockbox_stage(void);

/* Re-execs this binary in Compás loader mode. If exec fails, runs the loader
 * in the current process as a recovery path. */
void compas_boot_start_compas(void);

/* Runs a bounded helper command in its own process group. Timeout is limited
 * to 1..60 seconds. Returns 0 only when the command exits successfully. */
int compas_boot_run_bounded(char * const argv[], unsigned timeout_seconds);

#endif
