#pragma once
#include <lvgl/lvgl.h>

/* UI-thread lifecycle for the Accent Color and Accent Options screens.
 * Sync is safe before construction and after teardown. */
void gui_accent_build_screens(void);
void gui_accent_teardown(void);
void gui_accent_sync(void);
void gui_accent_row_cb(lv_event_t * e);
