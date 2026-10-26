#ifndef BOOTLOADER_INSTALLER_H
#define BOOTLOADER_INSTALLER_H

#include "../scan/scanner.h"

/* Persistent player override written by the player's update screen. */
#define INSTALLED_PLAYER_PATH "/usr/data/compas_player"

/* Installs the canonical SD update before startup; failures preserve the
 * installed player and allow normal startup. */
void installer_run(const scan_result_t * scan, bool fb_ready);

/* Uses embedded build stamps to retire an override older than the firmware's
 * packaged player. An unknown build stamp preserves the installed override. */
const char * installer_select_internal_player(const char * packaged, const char * installed);
const char * installer_internal_player_path(void);

#endif
