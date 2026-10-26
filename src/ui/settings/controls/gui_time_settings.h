#ifndef GUI_TIME_SETTINGS_H
#define GUI_TIME_SETTINGS_H

#include <lvgl/lvgl.h>

/* UI-thread lifecycle for the Date & Time screens. The module retains its
 * existing City-label backing storage across teardown, matching prior use. */
void gui_time_settings_build_screens(void);
void gui_time_settings_teardown(void);
void gui_time_settings_clock_row_cb(lv_event_t * e);

/* Kept in gui.h for existing Player callers; implementation is owned here. */
void clock_24h_switch_event_cb(lv_event_t * e);

#endif /* GUI_TIME_SETTINGS_H */
