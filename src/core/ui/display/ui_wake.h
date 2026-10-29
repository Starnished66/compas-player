#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Initialize once, before starting workers. The full event loop is an opt-in
 * prototype: COMPAS_EVENT_UI=1. Descriptors live until process exit, so producers never
 * race a close/reuse. No worker may call LVGL through this interface. */
void ui_wake_init(void);
/* True only for the COMPAS_EVENT_UI=1 event loop. */
bool ui_wake_enabled(void);
/* Callable from any thread. Cuts the owner loop's sleep short and flags that
 * a worker published a result. */
void ui_wake_notify(void);
/* Owner loop: was anything published since the last call? */
bool ui_wake_take_jobs(void);
/* Owner loop: sleep up to timeout_ms, or until ui_wake_notify() (the plain
 * sleep when there is no pipe). Does not touch the touch descriptor. */
void ui_wake_sleep(uint32_t timeout_ms);
/* Wait for worker notification, touch readiness, or the next LVGL deadline.
 * The touch descriptor is owned and consumed exclusively by LVGL. */
enum { UI_WAKE_WORKER = 1, UI_WAKE_TOUCH = 2, UI_WAKE_TOUCH_ERROR = 4 };
unsigned ui_wake_wait(int touch_fd, uint32_t timeout_ms);
