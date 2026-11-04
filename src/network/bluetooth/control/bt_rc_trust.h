#ifndef BT_RC_TRUST_H
#define BT_RC_TRUST_H

/* Bluetooth devices trusted to use Remote Control without the PIN. BlueZ
 * pairing alone proves nothing here (a Just Works pairing is auto-accepted),
 * so a device is trusted only after the user tapped Pair on the player's own
 * numeric-comparison popup or after it sent the valid PIN. Each entry is bound
 * to a fingerprint (SHA-256) of the bond's link key as bluetoothd stored it:
 * a bond made again, by anyone and through any agent, has a new key and so
 * loses the trust, and a device with no readable bond is never trusted. The
 * list is bounded (oldest evicted), keyed by upper-case "AA:BB:CC:DD:EE:FF", and
 * thread-safe: the RFCOMM workers, the UI thread, the pairing agent thread
 * and the link key watcher all use it. Persistence lives in bt_rc_trust_settings.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_RC_TRUST_MAX 8
#define BT_RC_TRUST_MAC_SIZE 18
#define BT_RC_TRUST_FP_SIZE 65 /* 64 hex chars and the NUL */

typedef struct {
    char mac[BT_RC_TRUST_MAC_SIZE];
    char fingerprint[BT_RC_TRUST_FP_SIZE];
} bt_rc_trust_entry_t;

/* Writes the upper-case form of a "AA:BB:CC:DD:EE:FF" MAC (any case); false when malformed. */
bool bt_rc_trust_normalize(const char * mac, char out[BT_RC_TRUST_MAC_SIZE]);
/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" to the normalized MAC; false when it is not a device path. */
bool bt_rc_trust_mac_from_path(const char * path, char out[BT_RC_TRUST_MAC_SIZE]);

bool bt_rc_trust_fingerprint_valid(const char * fp);

/* True when the MAC is listed and its bond still has the fingerprint it was trusted with;
 * otherwise the entry is removed. */
bool bt_rc_trust_is_trusted(const char * mac);
/* Counts revocations (every remove and clear). A grant decided under one epoch is dropped if it changed. */
unsigned int bt_rc_trust_epoch(void);
/* Trusts the device's current bond (does nothing when it has none) unless the epoch changed since
 * it was read. This is the path of a valid Remote Control PIN. */
void bt_rc_trust_add_at_epoch(const char * mac, unsigned int epoch);
/* Also advances the epoch (even when the device is not listed), so a grant armed earlier is dropped. */
void bt_rc_trust_remove(const char * mac);
#define BT_RC_TRUST_LINK_KEY_SIZE 16

/* The user's Pair tap on the numeric comparison of the player, for the device pairing now. It
 * completes only from the link key event of that pairing: a stored, MITM-protected key reported
 * for the device after this call, with no revocation in between (any remove or clear drops it),
 * within a time limit. Then the device is trusted with the fingerprint of that key. Any other new
 * key for the device while it waits cancels it. */
void bt_rc_trust_arm_grant(const char * mac);
/* True while that grant can still complete; a request from the device waits for it. */
bool bt_rc_trust_grant_pending(const char * mac);

/* A new link key the kernel reported for a device (bt_link_key_watch.h): stored is the kernel's
 * store_hint, key_type its key type. It completes a pending grant (see above); otherwise a key that
 * is not stored always revokes the device, and a stored one revokes it unless it is the key the
 * trust was granted for (the fingerprint of the key as bluetoothd writes it to the info file),
 * since bluetoothd can fail to store the key it was given. */
void bt_rc_trust_link_key_event(const char * mac, bool stored, int key_type,
                                const uint8_t key[BT_RC_TRUST_LINK_KEY_SIZE]);
/* Revokes every device (the Remote Control PIN was replaced). */
void bt_rc_trust_clear(void);

/* Startup load from the settings; does not mark the list changed. */
void bt_rc_trust_import(const bt_rc_trust_entry_t * entries, int count);
/* Copies the list when it changed since the last import/export; true when it should be saved. */
bool bt_rc_trust_export_if_dirty(bt_rc_trust_entry_t * entries, int * count);

#endif /* BT_RC_TRUST_H */
