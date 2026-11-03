#include "gui_setting_switches.h"
#include "../../player/screen/gui_player.h"
#include "../../player/screen/gui_lyrics.h"
#include "../../shell/gui_shell.h"
#include "../../shell/gui_navigation.h"
#include "../../../core/persistence/settings.h"
#include "../../../hardware/device/headphone_status.h"
#include "../../../hardware/indicators/led_control.h"
#include "../../../hardware/power/charge_limiter.h"

/* True when the event is a switch value change; *checked is the new state. */
static bool switch_value_changed(lv_event_t * e, bool * checked) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return false;
    *checked = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    return true;
}

void inline_remote_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.inline_remote_enabled = checked;
    headphone_status_refresh_earpods_adc();
    settings_save_async(&current_settings);
}

void lyrics_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.lyrics_enabled = checked;
    settings_save_async(&current_settings);
}

void swipe_up_home_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.swipe_up_home_enabled = checked;
    gui_navigation_refresh_home_snapshot(); /* Home's frame exists only while the gesture is on */
    lv_obj_t * active = lv_screen_active();
    gui_shell_set_home_indicator_visible(current_settings.swipe_up_home_enabled &&
                                         active != gui_shell_get_home_screen() &&
                                         active != gui_lyrics_get_screen());
    settings_save_async(&current_settings);
}

void hide_player_topbar_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.hide_player_topbar = checked;
    settings_save_async(&current_settings);
    sync_player_topbar_visibility(lv_screen_active());
    player_transition_mark_dirty(); /* topbar/back-button target-state visibility just changed -- see the cache's own doc comment */
}

void led_indicator_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.led_indicator_enabled = checked;
    led_control_apply(current_settings.led_indicator_enabled);
    settings_save_async(&current_settings);
}

void charge_limiter_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.charge_limiter_enabled = checked;
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    settings_save_async(&current_settings);
}

void safe_charging_switch_event_cb(lv_event_t * e) {
    bool checked;
    if (!switch_value_changed(e, &checked)) return;
    current_settings.safe_charging_enabled = checked;
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    settings_save_async(&current_settings);
}
