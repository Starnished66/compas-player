#ifndef GUI_PLUGIN_STORE_H
#define GUI_PLUGIN_STORE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include "plugin_store.h"

void gui_plugin_store_init(void);
void gui_plugin_store_teardown(void);
void gui_plugin_store_row_cb(lv_event_t * e);
/* Refresh the catalog and open a requested plugin's details after the store
 * is visible. Pass NULL or an empty ID to open the complete catalog. */
bool gui_plugin_store_open_recommendation(const char * id);
bool gui_plugin_store_open_picker(bool (*is_selected)(const char * id),
                                  void (*toggle)(const char * id, const char * name));
bool gui_plugin_store_operation_active(void);
bool gui_plugin_store_setup_catalog_prepare(void);
bool gui_plugin_store_setup_catalog_ready(void);
bool gui_plugin_store_setup_get_plugin(const char * id, plugin_store_result_t * row,
                                      plugin_store_details_t * details);
void poll_plugin_store(void);

#endif
