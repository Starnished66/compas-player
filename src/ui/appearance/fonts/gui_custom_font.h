#ifndef GUI_CUSTOM_FONT_H
#define GUI_CUSTOM_FONT_H

#include <lvgl/lvgl.h>

/* UI-thread lifecycle for the Settings → Appearance font picker. */
void gui_custom_font_build_screen(void);
void gui_custom_font_teardown(void);
void gui_custom_font_row_cb(lv_event_t * e);

#endif /* GUI_CUSTOM_FONT_H */
