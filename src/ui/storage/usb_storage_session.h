#ifndef USB_STORAGE_SESSION_H
#define USB_STORAGE_SESSION_H

#include <stdbool.h>

typedef struct {
    bool host_seen;
} usb_storage_session_t;

/* Latch host enumeration for the whole cable session. Controller state can
 * disappear before the unplug poll, or change during suspend/mode switches.
 * Returns true exactly once when a confirmed Storage session is unplugged.
 * A selected/bound gadget alone is not evidence of a computer connection. */
static inline bool usb_storage_session_poll(usb_storage_session_t * session,
                                            bool connected, bool storage_configured) {
    if (connected) {
        session->host_seen |= storage_configured;
        return false;
    }
    bool ended = session->host_seen;
    session->host_seen = false;
    return ended;
}

/* Keep the historical host latch for unplug-time cache/rescan handling, but
 * release local-operation guards once ADB is positively identified on the
 * still-connected cable. Callers must pass false during a mode transition or
 * whenever the live gadget state is unknown. */
static inline bool usb_storage_session_blocks_local_work(
        const usb_storage_session_t * session, bool connected,
        bool adb_confirmed) {
    return session->host_seen && connected && !adb_confirmed;
}

#endif
