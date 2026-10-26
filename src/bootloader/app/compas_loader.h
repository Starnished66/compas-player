#ifndef COMPAS_BOOTLOADER_LOADER_H
#define COMPAS_BOOTLOADER_LOADER_H

#define COMPAS_LOADER_ARG "--compas-loader"

#include <stdbool.h>

/* Draw the splash and mount SD before entering the Rockbox selector. Leaves
 * the framebuffer open when available and returns whether it opened. */
bool compas_loader_prepare(void);

/* Run the normal Compás update, player selection and supervision sequence.
 * startup_prepared is true after compas_loader_prepare() ran before exec. */
void compas_loader_run(bool startup_prepared);

/* Tools use the same orderly storage-release paths as player exits. */
void compas_loader_restart(void);
void compas_loader_poweroff(void);

#endif
