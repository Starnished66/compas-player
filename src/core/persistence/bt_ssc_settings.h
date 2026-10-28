#ifndef BT_SSC_SETTINGS_H
#define BT_SSC_SETTINGS_H

/* Persistence of the per-accessory Samsung SSC memory, stored beside the
 * per-accessory rate as "bt_ssc_device=MAC,worked,failures,skip". */

#include <stdio.h>
#include <stdbool.h>

#include "settings.h"

/* One settings line's value; a malformed one is dropped. */
void bt_ssc_settings_parse(player_settings_t * out, const char * value);
void bt_ssc_settings_write(FILE * f, const player_settings_t * settings);
/* Hands the saved memory to the Bluetooth policy at startup. */
void bt_ssc_settings_apply(const player_settings_t * settings);
/* Copies changed policy memory into the settings; true when they should be
 * saved. Call from the thread that owns the settings. */
bool bt_ssc_settings_sync(player_settings_t * settings);

#endif /* BT_SSC_SETTINGS_H */
