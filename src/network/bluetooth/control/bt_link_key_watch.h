#ifndef BT_LINK_KEY_WATCH_H
#define BT_LINK_KEY_WATCH_H

/* Watches the kernel Bluetooth management channel for new link keys. A peer that pairs with
 * "No Bonding" gets a key the kernel reports with store_hint 0: bluetoothd then leaves the old
 * info file (and so the old bond fingerprint) untouched, so the file alone cannot show the
 * re-pairing. Such an event revokes that device's Remote Control trust (bt_rc_trust.h). A stored
 * key (store_hint 1) revokes it too unless it is the key the trust was granted for, since
 * bluetoothd can fail to write it, and it is also what completes the user's Pair tap into a
 * trust grant, so the grant is tied to the pairing that was confirmed. Trust is judged only while the watch is healthy, and a queue
 * that may have overflowed revokes everything, so it fails closed. */

#include <stdbool.h>

#ifdef HOST_BUILD
/* The host has no mgmt socket: nothing to watch, so it is always healthy. */
static inline void bt_link_key_watch_start(void) {}
static inline bool bt_link_key_watch_healthy(void) { return true; }
static inline void bt_link_key_watch_drain(void) {}
#else
/* Starts the reader thread once; later calls do nothing. */
void bt_link_key_watch_start(void);
/* True while the mgmt socket is open and has not failed. */
bool bt_link_key_watch_healthy(void);
/* Processes every queued event now, so a key reported before this call is acted on first.
 * Never call with the trust lock held. */
void bt_link_key_watch_drain(void);
#endif

#endif /* BT_LINK_KEY_WATCH_H */
