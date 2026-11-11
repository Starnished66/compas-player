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

/* True once, when a resumed Wi-Fi radio is waiting to be turned back on. */
bool power_suspend_take_wifi_restore(void);

#endif /* POWER_SUSPEND_H */
