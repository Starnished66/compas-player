#ifndef GUI_SETTING_SWITCHES_H
#define GUI_SETTING_SWITCHES_H

#include <lvgl/lvgl.h>

/* Settings switch callbacks that only apply a setting and its side effect.
 * crossfade/gapless stay in gui_player.c because they re-arm the next track. */
void lyrics_switch_event_cb(lv_event_t * e);
void inline_remote_switch_event_cb(lv_event_t * e);
void swipe_up_home_switch_event_cb(lv_event_t * e);
void hide_player_topbar_switch_event_cb(lv_event_t * e);
void led_indicator_switch_event_cb(lv_event_t * e);
void charge_limiter_switch_event_cb(lv_event_t * e);
void safe_charging_switch_event_cb(lv_event_t * e);

#endif /* GUI_SETTING_SWITCHES_H */
