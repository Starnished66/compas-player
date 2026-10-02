#pragma once

#include <lvgl/lvgl.h>
#include <stdbool.h>

bool gui_setup_show_if_needed(void);
void gui_setup_teardown(void);
bool gui_setup_is_active(void);
void gui_setup_after_reload(void);
lv_obj_t * gui_setup_get_screen(void);
void gui_setup_open_timezone(void);
void gui_setup_open_wifi(void);
void gui_setup_open_plugins(void);
