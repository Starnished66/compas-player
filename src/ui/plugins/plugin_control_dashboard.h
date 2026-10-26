#pragma once

#include <lvgl/lvgl.h>
#include <stdbool.h>

/* Native presentation for a plugin's existing show_settings_list rows.
 * The caller owns callback routing and row state; the dashboard owns only
 * the LVGL objects created below `parent`. */
typedef struct {
    int type;
    const char * label;
    bool toggle_value;
    int slider_min;
    int slider_max;
    int slider_value;
    const char * icon_path;
    const char * detail;
} plugin_control_dashboard_row_t;

typedef struct plugin_control_dashboard_options {
    int markers;
    bool accent_first;
    bool active;
    char detail[128];
} plugin_control_dashboard_options_t;

typedef void (*plugin_control_dashboard_tap_cb)(int slot, int row);
typedef void (*plugin_control_dashboard_value_cb)(int slot, int row, int value);

typedef struct {
    plugin_control_dashboard_tap_cb tap;
    plugin_control_dashboard_value_cb value;
} plugin_control_dashboard_callbacks_t;

/* Renders the conventional seven-row instrument layout: start/stop, one
 * tempo slider, tap tempo, then four compact settings actions. Returns false
 * if the row shape is unsupported; it never owns/copies callback state. */
bool plugin_control_dashboard_build(lv_obj_t * parent,
                                    const plugin_control_dashboard_row_t * rows,
                                    int count,
                                    int slot,
                                    const plugin_control_dashboard_options_t * options,
                                    const plugin_control_dashboard_callbacks_t * callbacks);

