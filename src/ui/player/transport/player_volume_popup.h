#ifndef PLAYER_VOLUME_POPUP_H
#define PLAYER_VOLUME_POPUP_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

extern lv_obj_t * volume_popup;
extern lv_obj_t * volume_popup_backdrop;

void poll_volume_popup_timeout(void);
void configure_native_slider_rail(lv_obj_t * slider);
bool gui_player_volume_control_hit_test(lv_point_t point);
/* UI-thread lifecycle. Layout-only rebuilds retain the popup; full teardown
 * flushes pending hardware volume and saves it before deleting its widgets. */
void player_volume_popup_init(void);
void show_volume_popup(int32_t percent);
void player_volume_popup_teardown(void);
void player_volume_popup_refresh_assets(void);
void player_volume_popup_cancel_pending(void);
bool player_volume_popup_is_dragged(void);

#endif /* PLAYER_VOLUME_POPUP_H */
