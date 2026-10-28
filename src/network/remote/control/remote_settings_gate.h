#ifndef REMOTE_SETTINGS_GATE_H
#define REMOTE_SETTINGS_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* The "Allow remote settings changes" switch. Session-only: it is never
 * saved, so it is off after every boot, and it switches itself off after
 * REMOTE_SETTINGS_ALLOW_MS. The caller passes the clock so tests need none. */
#define REMOTE_SETTINGS_ALLOW_MS (30u * 60u * 1000u)

void remote_settings_gate_set(bool on, uint64_t now_ms);
bool remote_settings_gate_open(uint64_t now_ms);
/* True once after the switch ran out on its own, so the UI can flip it back. */
bool remote_settings_gate_consume_expired(uint64_t now_ms);
/* Milliseconds from CLOCK_BOOTTIME (runs during suspend). */
uint64_t remote_settings_now_ms(void);

#endif /* REMOTE_SETTINGS_GATE_H */
