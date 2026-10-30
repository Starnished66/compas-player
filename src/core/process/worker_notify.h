#ifndef WORKER_NOTIFY_H
#define WORKER_NOTIFY_H

/* Tells the UI loop that a worker published something, so it applies the result
 * on its next pass instead of at the next poll. ui_wake.c owns the real
 * function; it is weak here so a worker module that is built or tested without
 * the UI (track_probe_test links track_probe.c alone) still links. */
extern void ui_wake_notify(void) __attribute__((weak));

static inline void worker_notify_ui(void) {
    if (ui_wake_notify) ui_wake_notify();
}

#endif
