#ifndef BT_PAIRING_AGENT_H
#define BT_PAIRING_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HOST_BUILD
static inline void bt_pairing_agent_cancel_prompts(void) {}
static inline bool bt_pairing_agent_activate(void) { return false; }
static inline void bt_pairing_agent_allow_pairing_for(unsigned int ms) { (void) ms; }
static inline bool bt_pairing_agent_pending_confirmation(char * name, size_t name_size, uint32_t * passkey,
                                                         uint32_t * request_id) {
    (void) name; (void) name_size; (void) passkey; (void) request_id;
    return false;
}
static inline void bt_pairing_agent_answer_confirmation(uint32_t request_id, bool accept) {
    (void) request_id; (void) accept;
}
static inline bool bt_pairing_agent_display_code(char * name, size_t name_size, char * code, size_t code_size,
                                                 uint32_t * request_id) {
    (void) name; (void) name_size; (void) code; (void) code_size; (void) request_id;
    return false;
}
#else
/* In-process BlueZ pairing agent (org.bluez.Agent1) with DisplayYesNo capability, so a peer
 * that demands MITM protection (some car head units) gets an authenticated key: numeric
 * comparison is confirmed by the user on the player (see below), legacy PIN requests are answered with "0000".
 * Registers it and makes it the default agent; blocking with short timeouts, safe to call
 * repeatedly (one round trip once registered; re-registers after a bluetoothd restart). False
 * when the bus or bluetoothd is unavailable, so the caller can fall back to bt-agent. */
bool bt_pairing_agent_activate(void);
/* Opens (or extends, never shortens) the window in which pairing requests are accepted; outside
 * it RequestConfirmation, RequestPinCode, RequestPasskey and RequestAuthorization are rejected, so
 * a device in range cannot pair unless the user is pairing. Thread-safe and non-blocking. */
void bt_pairing_agent_allow_pairing_for(unsigned int ms);
/* Numeric comparison inside the pairing window waits for the user: true while a request is
 * pending (device name, 6-digit code, id; never 0). The D-Bus reply is always sent from the
 * agent's own thread: answer only records the choice (ignored if the id is stale) and the reply
 * follows within a poll tick. Unanswered for 30 s it is rejected; Cancel, Release and a
 * bluetoothd restart drop it without a reply, so the UI sees it vanish. Thread-safe. */
bool bt_pairing_agent_pending_confirmation(char * name, size_t name_size, uint32_t * passkey, uint32_t * request_id);
void bt_pairing_agent_answer_confirmation(uint32_t request_id, bool accept);
/* The latest code the player shows for the user to type on the other device (DisplayPasskey as
 * "%06u", DisplayPinCode as sent): true while shown, with a fresh id per code (never 0).
 * Cleared on Cancel, once the device is paired, or after 60 s. Thread-safe. */
bool bt_pairing_agent_display_code(char * name, size_t name_size, char * code, size_t code_size, uint32_t * request_id);
/* Rejects a pending numeric comparison, drops a shown code and closes the pairing window, so a
 * pairing that waits for the user ends at once (the UI is going away and nobody can answer).
 * Thread-safe. */
void bt_pairing_agent_cancel_prompts(void);
#endif

#endif
