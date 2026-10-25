#pragma once
#include <lvgl/lvgl.h>

/* UI-thread update controls. Popup construction and teardown are separate
 * lifecycle phases to preserve Settings initialization and reload ordering.
 * Teardown does not cancel active updates. */
void gui_updates_build_popups(void);
void gui_updates_teardown(void);

/* Legacy firmware callback and main-loop polling entry points. */
void firmware_update_row_cb(lv_event_t * e);
void poll_firmware_ota(void);
/* Settings Player Update row callback. */
void gui_updates_player_row_cb(lv_event_t * e);
