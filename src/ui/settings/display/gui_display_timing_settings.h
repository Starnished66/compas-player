#ifndef GUI_DISPLAY_TIMING_SETTINGS_H
#define GUI_DISPLAY_TIMING_SETTINGS_H

#include <lvgl/lvgl.h>

/* Owns the Screen Timeout and Screen Dimming screens and their private
 * widgets. Call lifecycle and row APIs on the LVGL UI thread. */
void gui_display_timing_settings_build_screens(void);
void gui_display_timing_settings_teardown(void);
void gui_display_timing_settings_timeout_row_cb(lv_event_t * e);
void gui_display_timing_settings_dimming_row_cb(lv_event_t * e);

#endif /* GUI_DISPLAY_TIMING_SETTINGS_H */
