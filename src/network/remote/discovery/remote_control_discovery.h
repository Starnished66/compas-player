#ifndef REMOTE_CONTROL_DISCOVERY_H
#define REMOTE_CONTROL_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed, unauthenticated LAN discovery. The response contains only a protocol
 * marker and fixed port, never a hostname, URL, PIN, or device identifier.
 * Clients derive the host exclusively from the datagram source address. */
#define REMOTE_CONTROL_DISCOVERY_PORT 8899
#define REMOTE_CONTROL_DISCOVERY_REQUEST_SIZE 32

bool remote_control_discovery_request_is_valid(const uint8_t * packet, size_t packet_size);
size_t remote_control_discovery_make_reply(uint8_t * out, size_t out_size);

/* Starts/stops the UDP discovery responder. Both calls are idempotent. */
void remote_control_discovery_start(void);
void remote_control_discovery_stop(void);

#endif
