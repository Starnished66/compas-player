#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include <stdbool.h>

/* Blocking worker-only API: returns true if connected to an access point,
 * checking `wpa_cli -i wlan0
 * status` for "wpa_state=COMPLETED". On success, populates
 * *out_signal_level with a 0 (weakest) - 3 (strongest) bucket derived from
 * `wpa_cli -i wlan0 signal_poll`'s RSSI reading; these are conventional
 * RSSI buckets, not values extracted from the stock firmware. Returns false
 * (leaving *out_signal_level untouched) if not connected, or if
 * wpa_cli/wpa_supplicant aren't available at all (wifi off, or no wlan0
 * supplicant instance -- always the case on the host simulator); callers
 * should show the disconnected icon in every such case. Call from a worker
 * thread, never directly from the UI thread. */
bool wifi_get_status(int * out_signal_level);

/* Maps RSSI (dBm) to 0 (weakest) - 3 (strongest) signal level. */
static inline int wifi_signal_level_from_rssi(int rssi_dbm) {
    if (rssi_dbm >= -50) return 3;
    if (rssi_dbm >= -60) return 2;
    if (rssi_dbm >= -70) return 1;
    return 0;
}

#endif /* WIFI_STATUS_H */
