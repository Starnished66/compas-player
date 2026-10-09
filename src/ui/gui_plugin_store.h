#ifndef GUI_PLUGIN_STORE_H
#define GUI_PLUGIN_STORE_H

#define GUI_SETUP_LAYOUT_PLUGIN_ID "org.compas.vinyl_player"

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "plugin_store.h"

typedef enum {
    STORE_VIEW_PLUGINS = 0,
    STORE_VIEW_LAYOUTS,
    STORE_VIEW_THEMES,
    STORE_VIEW_UPDATES,
} gui_plugin_store_view_t;

void gui_plugin_store_init(void);
void gui_plugin_store_teardown(void);
void gui_plugin_store_row_cb(lv_event_t * e);
bool gui_plugin_store_open_player_layouts(void);
bool gui_plugin_store_open_themes(void);
bool gui_plugin_store_open_updates(void);
bool gui_plugin_store_open_layout_picker(bool (*is_selected)(const char * id),
                                         void (*select)(const char * id, const char * name));
uint64_t gui_plugin_store_setup_populate_layout_suggestions(
    lv_obj_t * parent, bool (*is_selected)(const char * id),
    void (*select)(const char * id, const char * name));
bool gui_plugin_store_open_picker(bool (*is_selected)(const char * id),
                                  void (*toggle)(const char * id, const char * name),
                                  void (*done)(void));
bool gui_plugin_store_operation_active(void);
bool gui_plugin_store_setup_catalog_prepare(void);
bool gui_plugin_store_setup_catalog_ready(void);
bool gui_plugin_store_setup_get_plugin(const char * id, plugin_store_result_t * row,
                                      plugin_store_details_t * details);
void poll_plugin_store(void);

#endif
