#ifndef GUI_SHELL_SETTINGS_H
#define GUI_SHELL_SETTINGS_H

#include <lvgl/lvgl.h>

/* Lazy Appearance screens owned by the Settings shell-customization flow. */
void gui_shell_settings_quick_drawer_row_cb(lv_event_t * e);
void gui_shell_settings_music_order_row_cb(lv_event_t * e);
void gui_shell_settings_topbar_row_cb(lv_event_t * e);

/* Keep these teardown phases split around gui_plugin_order_editor_teardown(). */
void gui_shell_settings_teardown_order(void);
void gui_shell_settings_teardown_after_plugin_order(void);

#endif /* GUI_SHELL_SETTINGS_H */
