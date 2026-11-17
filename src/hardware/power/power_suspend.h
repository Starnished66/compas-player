#ifndef POWER_SUSPEND_H
#define POWER_SUSPEND_H

#include <stdbool.h>

/* Initiates suspend-to-RAM via /sys/power/state. Wraps suspension with
 * framebuffer blanking (FB_BLANK_POWERDOWN / FB_BLANK_UNBLANK) to prevent
 * display driver timeouts on resume.
 *
 * Tears down active Wi-Fi and Bluetooth radios before suspend. Bluetooth is
 * restored in a background thread upon waking so the GUI can unblank the
 * display immediately; Wi-Fi is handed to the shell's toggle worker through
 * power_suspend_take_wifi_restore(). */
void power_suspend_now(void);

/* True when the last power_suspend_now() reached the mem write and the
 * wake is a charger. That requires axp2101_wake loaded and armed
 * (/sys/module/axp2101_wake/parameters/armed is 1). Then the R1 reads
 * AXP2101 REG00h bit 5, and a failed read falls back to the sysfs online
 * sample. Only CONNECTED counts; UNKNOWN is a button wake. No armed
 * module is a button wake, including every board that does not load it.
 * False when suspend was skipped or the wake was a button. */
bool power_suspend_woke_on_external_power(void);

/* True from the moment a Bluetooth restore is queued after a suspend until it has finished. */
bool power_suspend_bt_restore_pending(void);

/* True once, when a resumed Wi-Fi radio is waiting to be turned back on. */
bool power_suspend_take_wifi_restore(void);

#endif /* POWER_SUSPEND_H */
