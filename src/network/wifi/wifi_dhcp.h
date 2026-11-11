#ifndef WIFI_DHCP_H
#define WIFI_DHCP_H

/* The player's single DHCP client for wlan0. A worker attached to
 * wpa_supplicant's control socket starts one long-lived udhcpc on every
 * association and stops it, clearing the address, on every disconnection:
 * joining, switching or reconnecting to a network always gets a fresh
 * lease, and leases renew. The worker reattaches by itself when
 * wpa_supplicant is restarted by the radio scripts.
 *
 * start() after wifi_on.sh (idempotent, returns at once). stop() before
 * Wi-Fi goes off or the device sleeps; it joins the worker and kills the
 * client, so call it off the UI thread. */
void wifi_dhcp_start(void);
void wifi_dhcp_stop(void);

#endif /* WIFI_DHCP_H */
