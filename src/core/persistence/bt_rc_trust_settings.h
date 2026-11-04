#ifndef BT_RC_TRUST_SETTINGS_H
#define BT_RC_TRUST_SETTINGS_H

/* Persistence of the Bluetooth devices trusted for Remote Control
 * (bt_rc_trust.h), one "bt_rc_trusted=MAC FINGERPRINT" line each. */

#include <stdio.h>
#include <stdbool.h>

#include "settings.h"

/* One settings line's value; a malformed or repeated one is dropped (including one without a valid fingerprint). */
void bt_rc_trust_settings_parse(player_settings_t * out, const char * value);
void bt_rc_trust_settings_write(FILE * f, const player_settings_t * settings);
/* Hands the saved list to the trust module at startup. */
void bt_rc_trust_settings_apply(const player_settings_t * settings);
/* Copies a changed list into the settings; true when they should be saved.
 * Call from the thread that owns the settings. */
bool bt_rc_trust_settings_sync(player_settings_t * settings);

#endif /* BT_RC_TRUST_SETTINGS_H */
