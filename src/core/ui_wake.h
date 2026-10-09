#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Initialize once, before starting workers. Opt-in prototype:
 * COMPAS_EVENT_UI=1. Descriptors live until process exit, so producers never
 * race a close/reuse. No worker may call LVGL through this interface. */
void ui_wake_init(void);
bool ui_wake_enabled(void);
void ui_wake_notify(void);
/* Wait for worker notification, touch readiness, or the next LVGL deadline.
 * The touch descriptor is owned and consumed exclusively by LVGL. */
enum { UI_WAKE_WORKER = 1, UI_WAKE_TOUCH = 2, UI_WAKE_TOUCH_ERROR = 4 };
unsigned ui_wake_wait(int touch_fd, uint32_t timeout_ms);
