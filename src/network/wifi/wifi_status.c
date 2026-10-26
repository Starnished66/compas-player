#include "wifi_status.h"
#include "../../core/process/subprocess.h"

#include <stdlib.h>
#include <string.h>

#define WIFI_INTERFACE "wlan0"
#define WIFI_QUERY_TIMEOUT_MS 1500

/* Runs `wpa_cli -i wlan0 <args>` directly and reads its stdout into out_buf.
 * `args` is always one of this file's own literal strings below. Returns
 * false if wpa_cli cannot be run, times out, exits unsuccessfully, or produces
 * no output -- all normal and expected on the host simulator. */
static bool run_wpa_cli(const char * args, char * out_buf, size_t out_buf_size) {
    char * argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) args, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, out_buf, out_buf_size, WIFI_QUERY_TIMEOUT_MS, &exit_code) && exit_code == 0 &&
           out_buf[0] != '\0';
}

bool wifi_get_status(int * out_signal_level) {
    char status_buf[512];
    if (!run_wpa_cli("status", status_buf, sizeof(status_buf))) return false;
    if (!strstr(status_buf, "wpa_state=COMPLETED")) return false;

    int level = 0;
    char signal_buf[512];
    if (run_wpa_cli("signal_poll", signal_buf, sizeof(signal_buf))) {
        const char * rssi_pos = strstr(signal_buf, "RSSI=");
        if (rssi_pos) level = wifi_signal_level_from_rssi(atoi(rssi_pos + 5));
    }

    *out_signal_level = level;
    return true;
}
