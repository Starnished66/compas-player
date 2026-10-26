#ifndef REBOOT_HANDOFF_H
#define REBOOT_HANDOFF_H

typedef enum {
    REBOOT_HANDOFF_UNAVAILABLE,
    REBOOT_HANDOFF_SIGNAL_SETUP_FAILED,
    REBOOT_HANDOFF_REQUEST_FAILED,
    REBOOT_HANDOFF_REQUESTED
} reboot_handoff_result_t;

/* Ask BusyBox init to run its orderly shutdown path. The optional hook runs
 * after signal setup but before notifying init, so a supervisor can close
 * writable trace descriptors before shutdown hooks unmount storage. Returns
 * after a successful request's bounded grace period; callers then use their
 * normal raw-reboot fallback. */
reboot_handoff_result_t reboot_request_orderly_shutdown(void (*before_request)(void));

#endif
