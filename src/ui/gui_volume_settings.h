#ifndef GUI_VOLUME_SETTINGS_H
#define GUI_VOLUME_SETTINGS_H

#include <lvgl/lvgl.h>

/* UI-thread lifecycle. Startup/Max and Car Mode stay separate because the
 * Settings coordinator builds and deletes them at different points. */
void gui_volume_settings_build_startup_max_screens(void);
void gui_volume_settings_build_car_mode_screen(void);
void gui_volume_settings_teardown_startup_max_screens(void);
void gui_volume_settings_teardown_car_mode_screen(void);

void gui_volume_settings_startup_volume_row_cb(lv_event_t * e);
void gui_volume_settings_max_volume_row_cb(lv_event_t * e);
void gui_volume_settings_car_mode_row_cb(lv_event_t * e);

#endif /* GUI_VOLUME_SETTINGS_H */
