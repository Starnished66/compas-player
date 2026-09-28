#ifndef GUI_PLUGIN_STORE_H
#define GUI_PLUGIN_STORE_H

#include <lvgl/lvgl.h>

void gui_plugin_store_init(void);
void gui_plugin_store_teardown(void);
void gui_plugin_store_row_cb(lv_event_t * e);
void poll_plugin_store(void);

#endif
