#pragma once
#include <lvgl/lvgl.h>
#include "../widgets/builders/screen_builders.h"

typedef struct {
    const char *icon, *title, *explanation, *waiting;
    const char *enable_label, *stop_label;
    bool enabled, active, show_explanation;
    lv_event_cb_t enable_cb, stop_cb, transport_cb;
    bool transport;
} gui_dac_screen_options_t;
typedef struct { lv_obj_t *content, *hero, *title, *chip, *stream, *transport, *play; } gui_dac_screen_t;
void gui_network_dac_screen_build(lv_obj_t *content, const gui_dac_screen_options_t *options,
                                  gui_dac_screen_t *out);
void gui_network_dac_screen_action(gui_dac_screen_t *screen, const gui_dac_screen_options_t *options);
void gui_network_dac_screen_transport(gui_dac_screen_t *screen, lv_event_cb_t callback);
lv_obj_t *gui_network_dac_screen_create(const char *title, lv_obj_t **content);
void gui_network_dac_screen_confirm(gui_popup_t *popup, const char *title, const char *stop_label,
                                    lv_event_cb_t confirm_cb, lv_event_cb_t dismiss_cb);
