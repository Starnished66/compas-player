#ifndef REMOTE_CONTROL_MDNS_H
#define REMOTE_CONTROL_MDNS_H

#include <stdbool.h>
#include <stddef.h>

/* Best-effort DNS-SD advertisement for the Wi-Fi Remote Control endpoint. */
void remote_control_mdns_start(void);
void remote_control_mdns_stop(void);
/* Copies the probed DNS name (e.g. "r1.compas.local") once it is announced on
 * Wi-Fi; false while it is not published. */
bool remote_control_mdns_hostname(char *out, size_t size);

#endif
