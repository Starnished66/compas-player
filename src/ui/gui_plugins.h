#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include "plugin_manager.h"

void gui_plugins_init(void);
/* Deletes every pool screen this module owns so gui_reload.c's in-process
 * UI reload can call gui_plugins_init() again from a clean slate. */
void gui_plugins_teardown(void);
bool gui_plugin_show_progress(const char * title, const char * message, int percent);
bool gui_plugin_update_progress(const char * message, int percent);
bool gui_plugin_progress_is_visible(void);
void gui_plugin_close_progress(void);
void configure_scrolling_row_label(lv_obj_t * label, int32_t width);
