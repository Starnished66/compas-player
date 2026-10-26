#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include "../../../core/persistence/settings.h"
#include "../../shell/gui_shell.h"
#include "../../text/rendering/ui_label_text.h"

#define TR(text) (text)
player_settings_t current_settings;
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }

static uint32_t fake_tick;
static bool audio_playing;
static int pause_calls, save_calls, snapshot_dirty_calls, drawer_active;
static bool drawer_image_on;
static int info_toast_calls;
static char info_toast_text[96];

static uint32_t fake_tick_get(void) { return fake_tick; }
void settings_save(const player_settings_t *s) { assert(s == &current_settings); save_calls++; }
bool audio_is_playing(void) { return audio_playing; }
void toggle_play_pause(void) { pause_calls++; audio_playing = false; }
void quick_drawer_set_toggle_state(int index, bool enabled) { assert(index == 2); drawer_active = enabled; }
bool quick_drawer_set_toggle_image(lv_obj_t *image, const void *source) { (void)image; drawer_image_on = source && strstr((const char *)source,"on") != NULL; return true; }
const void *quick_drawer_toggle_src(int toggle, bool on) { assert(toggle == 2); return on ? "sleep_on" : "sleep_off"; }
const char *asset_path(const char *path) { return path; }
void quick_drawer_mark_snapshot_dirty(void) { snapshot_dirty_calls++; }
void show_info_toast(const char *text) { info_toast_calls++; snprintf(info_toast_text,sizeof(info_toast_text),"%s",text); }

#include "sleep_idle_production_extract.inc"

static void value_event(lv_obj_t *obj) { lv_obj_send_event(obj,LV_EVENT_VALUE_CHANGED,NULL); }
static void test_sleep_settings_and_deadline(void) {
    sleep_timer_switch=lv_switch_create(lv_screen_active());
    sleep_timer_slider=lv_slider_create(lv_screen_active());
    sleep_timer_value_label=lv_label_create(lv_screen_active());
    sleep_timer_slider_card=lv_obj_create(lv_screen_active());
    sleep_timer_remaining_btn=lv_button_create(lv_screen_active());
    lv_obj_add_event_cb(sleep_timer_switch,sleep_timer_switch_event_cb,LV_EVENT_ALL,NULL);
    quick_drawer_sleep_icon=lv_image_create(lv_screen_active());
    quick_drawer_sleep_label=lv_label_create(lv_screen_active());
    lv_slider_set_range(sleep_timer_slider,0,SLEEP_TIMER_STEP_COUNT-1);
    lv_obj_add_flag(sleep_timer_slider_card,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sleep_timer_remaining_btn,LV_OBJ_FLAG_HIDDEN);
    static const char *const labels[]={"5 min","10 min","15 min","20 min","30 min","45 min","1 hr","1 hr 30 min","2 hr","3 hr"};

    assert(!quick_drawer_sleep_timer_is_active());
    lv_obj_send_event(sleep_timer_switch,LV_EVENT_CLICKED,NULL);
    assert(!quick_drawer_sleep_timer_is_active() && save_calls==0);
    value_event(sleep_timer_switch);
    assert(!drawer_active && lv_obj_has_flag(sleep_timer_slider_card,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(sleep_timer_remaining_btn,LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_state(sleep_timer_switch,LV_STATE_CHECKED);
    value_event(sleep_timer_switch);
    assert(quick_drawer_sleep_timer_is_active() && drawer_active);
    assert(strcmp(lv_label_get_text(quick_drawer_sleep_label),"5m")==0);
    assert(!lv_obj_has_flag(sleep_timer_slider_card,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(sleep_timer_remaining_btn,LV_OBJ_FLAG_HIDDEN));

    lv_obj_add_event_cb(sleep_timer_slider,sleep_timer_slider_event_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(sleep_timer_slider,sleep_timer_slider_event_cb,LV_EVENT_RELEASED,NULL);
    for(int i=0;i<SLEEP_TIMER_STEP_COUNT;i++) {
        lv_slider_set_value(sleep_timer_slider,i,LV_ANIM_OFF);
        lv_obj_send_event(sleep_timer_slider,LV_EVENT_VALUE_CHANGED,NULL);
        assert(current_settings.sleep_timer_minutes==SLEEP_TIMER_STEPS[i]);
        assert(strcmp(lv_label_get_text(sleep_timer_value_label),labels[i])==0);
        lv_obj_send_event(sleep_timer_slider,LV_EVENT_RELEASED,NULL);
        assert(save_calls==i+1);
    }
    assert(save_calls==SLEEP_TIMER_STEP_COUNT);
    fake_tick=10000;
    lv_slider_set_value(sleep_timer_slider,8,LV_ANIM_OFF);
    lv_obj_send_event(sleep_timer_slider,LV_EVENT_VALUE_CHANGED,NULL);
    assert(quick_drawer_sleep_timer_remaining_seconds()==7200); /* changing an armed timer restarts its deadline */
    fake_tick+=58u*60u*1000u-3000u;
    assert(quick_drawer_sleep_timer_remaining_seconds()==3723);
    lv_obj_add_event_cb(sleep_timer_remaining_btn,sleep_timer_show_remaining_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_send_event(sleep_timer_remaining_btn,LV_EVENT_CLICKED,NULL);
    assert(info_toast_calls==1 && strcmp(info_toast_text,"Time remaining: 1:02:03")==0);
    fake_tick+=58u*60u*1000u+3000u;
    assert(quick_drawer_sleep_timer_remaining_seconds()==240);
    lv_obj_send_event(sleep_timer_remaining_btn,LV_EVENT_CLICKED,NULL);
    assert(info_toast_calls==2 && strcmp(info_toast_text,"Time remaining: 4:00")==0);
    fake_tick+=3u*60u*1000u; poll_sleep_timer();
    assert(strcmp(lv_label_get_text(quick_drawer_sleep_label),"1m")==0); /* rounds up during final live minute */
    int unchanged_poll_snapshot_count=snapshot_dirty_calls;
    poll_sleep_timer();
    assert(snapshot_dirty_calls==unchanged_poll_snapshot_count);
    audio_playing=true;
    fake_tick+=1u*60u*1000u;
    poll_sleep_timer();
    assert(!quick_drawer_sleep_timer_is_active() && pause_calls==1 && !audio_playing);
    assert(!drawer_active && !drawer_image_on);
    assert(!lv_obj_has_state(sleep_timer_switch,LV_STATE_CHECKED));
    assert(lv_obj_has_flag(sleep_timer_slider_card,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(sleep_timer_remaining_btn,LV_OBJ_FLAG_HIDDEN));
    assert(quick_drawer_sleep_timer_remaining_seconds()==0);

    /* Expiry while playback is already paused only disarms; it never power-cycles or pauses again. */
    audio_playing=false; fake_tick+=1; quick_drawer_sleep_timer_set_active(true); fake_tick+=120u*60u*1000u; poll_sleep_timer();
    assert(!quick_drawer_sleep_timer_is_active() && pause_calls==1);
}

static void test_idle_shutdown_settings_only(void) {
    idle_shutdown_switch=lv_switch_create(lv_screen_active());
    lv_obj_add_event_cb(idle_shutdown_switch,idle_shutdown_switch_event_cb,LV_EVENT_ALL,NULL);
    idle_shutdown_slider=lv_slider_create(lv_screen_active());
    idle_shutdown_value_label=lv_label_create(lv_screen_active());
    idle_shutdown_slider_card=lv_obj_create(lv_screen_active());
    idle_action_section=lv_obj_create(lv_screen_active());
    idle_action_poweroff_row=lv_button_create(lv_screen_active());
    idle_action_suspend_row=lv_button_create(lv_screen_active());
    idle_action_poweroff_check=lv_label_create(idle_action_poweroff_row);
    idle_action_suspend_check=lv_label_create(idle_action_suspend_row);
    lv_slider_set_range(idle_shutdown_slider,0,IDLE_SHUTDOWN_STEP_COUNT-1);
    lv_obj_add_flag(idle_action_section,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_shutdown_slider_card,LV_OBJ_FLAG_HIDDEN);
    lv_obj_send_event(idle_shutdown_switch,LV_EVENT_CLICKED,NULL);
    assert(!current_settings.idle_shutdown_enabled && save_calls==SLEEP_TIMER_STEP_COUNT);
    assert(!current_settings.idle_shutdown_enabled && lv_obj_has_flag(idle_action_section,LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_state(idle_shutdown_switch,LV_STATE_CHECKED);
    value_event(idle_shutdown_switch);
    assert(current_settings.idle_shutdown_enabled && !lv_obj_has_flag(idle_action_section,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(idle_shutdown_slider_card,LV_OBJ_FLAG_HIDDEN));
    assert(save_calls==SLEEP_TIMER_STEP_COUNT+1);
    lv_obj_clear_state(idle_shutdown_switch,LV_STATE_CHECKED);
    value_event(idle_shutdown_switch);
    assert(!current_settings.idle_shutdown_enabled && lv_obj_has_flag(idle_action_section,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(idle_shutdown_slider_card,LV_OBJ_FLAG_HIDDEN));
    assert(save_calls==SLEEP_TIMER_STEP_COUNT+2);

    lv_obj_add_event_cb(idle_action_suspend_row,idle_action_choice_cb,LV_EVENT_CLICKED,(void *)(intptr_t)true);
    lv_obj_send_event(idle_action_suspend_row,LV_EVENT_CLICKED,NULL);
    assert(current_settings.idle_suspend_enabled);
    assert(lv_obj_get_style_border_width(idle_action_suspend_row,0)==3);
    assert(lv_obj_get_style_border_width(idle_action_poweroff_row,0)==0);
    assert(save_calls==SLEEP_TIMER_STEP_COUNT+3);
    assert(lv_obj_has_flag(idle_action_poweroff_check,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(idle_action_suspend_check,LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(idle_action_poweroff_row,idle_action_choice_cb,LV_EVENT_CLICKED,(void *)(intptr_t)false);
    lv_obj_send_event(idle_action_poweroff_row,LV_EVENT_CLICKED,NULL);
    assert(!current_settings.idle_suspend_enabled);
    assert(lv_obj_get_style_border_width(idle_action_poweroff_row,0)==3);
    assert(lv_obj_get_style_border_width(idle_action_suspend_row,0)==0);
    assert(save_calls==SLEEP_TIMER_STEP_COUNT+4);
    assert(!lv_obj_has_flag(idle_action_poweroff_check,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(idle_action_suspend_check,LV_OBJ_FLAG_HIDDEN));

    static const char *const labels[]={"10m","15m","30m","1h","2h"};
    lv_obj_add_event_cb(idle_shutdown_slider,idle_shutdown_slider_event_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(idle_shutdown_slider,idle_shutdown_slider_event_cb,LV_EVENT_RELEASED,NULL);
    for(int i=0;i<IDLE_SHUTDOWN_STEP_COUNT;i++) {
        lv_slider_set_value(idle_shutdown_slider,i,LV_ANIM_OFF);
        lv_obj_send_event(idle_shutdown_slider,LV_EVENT_VALUE_CHANGED,NULL);
        assert(current_settings.idle_shutdown_minutes==IDLE_SHUTDOWN_STEPS[i]);
        assert(strcmp(lv_label_get_text(idle_shutdown_value_label),labels[i])==0);
        assert(save_calls==SLEEP_TIMER_STEP_COUNT+4);
    }
    int saves_before_release=save_calls;
    lv_obj_send_event(idle_shutdown_slider,LV_EVENT_RELEASED,NULL);
    assert(save_calls==saves_before_release+1);
    /* This spec includes only setting callbacks; no idle timer executor or power API is linked. */
}

int main(void) {
    lv_init();
    lv_tick_set_cb(fake_tick_get);
    assert(lv_display_create(240,320));
    current_settings.sleep_timer_minutes=5;
    current_settings.idle_shutdown_minutes=10;
    current_settings.idle_shutdown_enabled=false;
    current_settings.idle_suspend_enabled=false;
    test_sleep_settings_and_deadline();
    test_idle_shutdown_settings_only();
    printf("sleep/idle settings characterization passed (saves=%d, snapshots=%d)\n",save_calls,snapshot_dirty_calls);
    return 0;
}
