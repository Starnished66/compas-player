#ifndef AIC_RADIO_H
#define AIC_RADIO_H

#include <stdbool.h>

/* AIC8800D40 support for the newer R1 units (X1600EN/HN). HiBy v2.0 firmware
 * writes /run/r1_wireless_type = "bcm" or "aic" at boot. Everything here is
 * inert unless that file says "aic"; Broadcom units, and any unit where the
 * file is missing, never reach these functions' work. */

typedef enum {
    AIC_RADIO_NOTICE_NONE,
    AIC_RADIO_NOTICE_WIFI_BLOCKED_BY_BT,
    AIC_RADIO_NOTICE_BT_BLOCKED_BY_WIFI
} aic_radio_notice_t;

/* True on an AIC unit. The type file is read until it exists, then cached
 * for the life of the process. */
bool aic_radio_active(void);

/* True once the boot-time driver load has finished (successfully or not), so
 * the UI can stop waiting for /tmp/bt_init_ok, which AIC units only create
 * while Bluetooth is on. Always false off AIC. */
bool aic_radio_boot_settled(void);

/* Wi-Fi on/off through /usr/bin/wifi_on.sh and wifi_off.sh, which carry the
 * power-gate logic on v2.0. Enable returns false (and records a notice) when
 * refused or when the script fails. Callers run these off the UI thread. */
bool aic_radio_wifi_enable(void);
void aic_radio_wifi_disable(void);

/* Bluetooth bring-up and tear-down: takes /run/aic_bt_power_gate, runs
 * /usr/bin/bt_enable_aic.sh, and on the way out runs /usr/bin/bt_disable and
 * releases the gate. */
bool aic_radio_bt_power_on(void);
void aic_radio_bt_power_off(void);

/* Before sleeping: make sure neither radio is left powered. Returns false if
 * hci0 or aic8800_fdrv survived the shutdown, in which case the caller must
 * not suspend (same rule as the stock player). */
bool aic_radio_suspend_prepare(void);

/* The most recent refusal since the last call, cleared by reading. */
aic_radio_notice_t aic_radio_take_notice(void);

#endif /* AIC_RADIO_H */
