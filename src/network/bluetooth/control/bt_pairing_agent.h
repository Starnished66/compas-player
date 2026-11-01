#ifndef BT_PAIRING_AGENT_H
#define BT_PAIRING_AGENT_H

#include <stdbool.h>

#ifdef HOST_BUILD
static inline bool bt_pairing_agent_activate(void) { return false; }
static inline bool bt_pairing_agent_yield(void) { return false; }
static inline void bt_pairing_agent_allow_pairing_for(unsigned int ms) { (void) ms; }
#else
/* In-process BlueZ pairing agent (org.bluez.Agent1) with DisplayYesNo capability, so a peer
 * that demands MITM protection (some car head units) gets an authenticated key: numeric
 * comparison is confirmed automatically, legacy PIN requests are answered with "0000".
 * Registers it and makes it the default agent; blocking with short timeouts, safe to call
 * repeatedly (one round trip once registered; re-registers after a bluetoothd restart). False
 * when the bus or bluetoothd is unavailable, so the caller can fall back to bt-agent. */
bool bt_pairing_agent_activate(void);
/* For handing over to another agent: stays registered and queued for the default role (registering
 * again if needed), so the next agent to request the default role takes over with no gap and this
 * one answers again if that agent goes away. Stops re-claiming the role after bluetoothd restarts.
 * False when ours could not be queued, so the caller must not remove the agent now answering. */
bool bt_pairing_agent_yield(void);
/* Opens (or extends, never shortens) the window in which pairing requests are accepted; outside
 * it RequestConfirmation, RequestPinCode, RequestPasskey and RequestAuthorization are rejected, so
 * a device in range cannot pair unless the user is pairing. Thread-safe and non-blocking. */
void bt_pairing_agent_allow_pairing_for(unsigned int ms);
#endif

#endif
