#ifndef WIRELESS_ADB_H
#define WIRELESS_ADB_H

#include <stdbool.h>

/* Wireless ADB: a private copy of the stock adbd, run with no USB function
 * endpoint so it listens on TCP 5555 (all interfaces, so Wi-Fi coming up
 * later needs no restart). Only one adbd can hold port 5037, so it runs only
 * while the setting is on and USB ADB is not in use. It is its own process
 * and survives a player restart. */

/* Records the user's setting; a background thread starts or stops the process. */
void wireless_adb_sync(bool wanted);

/* Initialize both desired flags without waiting on the UI thread. */
void wireless_adb_boot_sync(bool wanted, bool claimed);

/* Tells the module whether USB ADB owns (or is about to own) the port. Call
 * with true before starting the ADB gadget (it returns once the TCP adbd is gone), and with false once a switch away
 * from ADB, or a failed ADB switch, has finished. False forbids USB startup. */
bool wireless_adb_usb_adb_claim(bool claimed);

#endif /* WIRELESS_ADB_H */
