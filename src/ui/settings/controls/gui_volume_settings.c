#include "../setup/gui_setup.h"
#include "gui_volume_settings.h"
#include "../gui_native_audio.h"
#include "../gui_settings.h"
#include "../../../audio/engine/audio.h"
#include "../../player/screen/gui_player.h"
#include "../../shell/gui_shell.h"
#include "../../shell/gui_navigation.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/native_audio_settings.h"
#include "../../../core/persistence/settings.h"
#include "../../text/i18n/i18n.h"

#include <stdio.h>

static lv_obj_t * startup_volume_screen;
static setting_slider_t startup_volume_ui;
static setting_slider_t max_volume_ui;
static setting_slider_t car_mode_volume_ui;
static lv_obj_t * car_mode_screen;
static lv_obj_t * car_mode_autoresume_switch;
static lv_obj_t * car_mode_gain_switch;

static void format_percent(char * buf, size_t buf_size, int percent) {
    snprintf(buf, buf_size, "%d%%", percent);
}

static void startup_volume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_volume_fixed_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);

    setting_slider_set_enabled(&startup_volume_ui, current_settings.startup_volume_fixed_enabled);
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as
 * screen_timeout_slider_event_cb -- this only sets what the NEXT launch
 * starts at, not the current session's live volume, so there's no reason
 * to call audio_set_volume() here at all. */
static void startup_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.startup_volume_fixed_percent = (int) percent;
        setting_slider_set_index(&startup_volume_ui, percent);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save_async(&current_settings);
    }
}

/* Unlike startup_volume_slider_event_cb, this DOES apply live: the whole
 * point is an audible ceiling, so dragging it below the current volume
 * must bring playback down immediately rather than waiting for the next
 * volume change to reveal the cap. audio_set_max_volume_percent() itself
 * clamps all volume requests and immediately lowers live output when needed.
 * The previous value is captured here so the Player mirror and saved
 * last-used volume follow the new cap when it is lowered. */
static void max_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        float previous_volume = audio_get_volume();
        current_settings.max_volume_percent = (int) percent;
        setting_slider_set_index(&max_volume_ui, percent);
        audio_set_max_volume_percent((int) percent);
        float capped_volume = audio_get_volume();
        if (capped_volume < previous_volume) {
            int32_t current_percent = (int32_t) (capped_volume * 100.0f + 0.5f);
            gui_player_set_volume_percent(current_percent);
            gui_player_remember_volume_percent(current_percent);
            refresh_volume_topbar(current_percent);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        settings_save_async(&current_settings);
    }
}

/* Startup volume and the live maximum share one page; 100% means uncapped. */
static void volume_screen_loaded_cb(lv_event_t *e) {
    (void)e;
    setting_slider_sync(&startup_volume_ui, current_settings.startup_volume_fixed_enabled, current_settings.startup_volume_fixed_percent);
    setting_slider_set_index(&max_volume_ui, current_settings.max_volume_percent);
}

static lv_obj_t * build_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, volume_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Volume limits"), generic_back_cb, NULL, NULL);
    lv_obj_t * content = build_settings_body(scr);
    add_section_header(content, TR("Startup volume"));
    startup_volume_ui = build_setting_slider(content, &(setting_slider_cfg_t) {
        .toggle_label = TR("Launch at a fixed volume"),
        .checked = current_settings.startup_volume_fixed_enabled,
        .switch_cb = startup_volume_switch_event_cb,
        .caption = TR("Volume"), .max_index = 100,
        .index = current_settings.startup_volume_fixed_percent,
        .fmt = format_percent, .slider_cb = startup_volume_slider_event_cb,
    });
    add_section_header(content, TR("Max volume"));
    build_setting_hint(content, TR("Limits the player volume, including buttons, remote controls and plugins."));
    max_volume_ui = build_setting_slider(content, &(setting_slider_cfg_t) {
        .caption = TR("Volume"), .min_index = 1, .max_index = 100,
        .index = current_settings.max_volume_percent,
        .fmt = format_percent, .slider_cb = max_volume_slider_event_cb,
    });
    finalize_screen_navigation(scr);
    return scr;
}

void gui_volume_settings_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(startup_volume_screen);
}

static void car_mode_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.car_mode_volume_percent = (int) percent;
        setting_slider_set_index(&car_mode_volume_ui, percent);
        if (current_settings.car_mode_enabled) {
            audio_set_volume((float) percent / 100.0f);
            int32_t applied_percent = (int32_t) (audio_get_volume() * 100.0f + 0.5f);
            gui_player_set_volume_percent(applied_percent);
            refresh_volume_topbar(applied_percent);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        settings_save_async(&current_settings);
    }
}

static void car_mode_enable_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_car_mode_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void car_mode_autoresume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.car_mode_autoresume_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

static void car_mode_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    gui_settings_sync_car_mode();
}

/* Independent of the general playback resume setting. */
static void build_car_mode_autoresume(lv_obj_t * body) {
    build_setting_toggle_card(body, TR("Auto-resume"), TR("Resume playback when external power turns the player on."),
        current_settings.car_mode_autoresume_enabled, car_mode_autoresume_switch_event_cb,
        &car_mode_autoresume_switch);
}

/* Share the native Low/High gain control with Sound. */
static void build_car_mode_gain_row(lv_obj_t * body) {
    car_mode_gain_switch = gui_native_audio_add_gain_row(body);
}

static lv_obj_t * build_car_mode_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Car mode"), generic_back_cb, NULL, NULL);

    lv_obj_t * body = build_settings_body(scr);

    car_mode_volume_ui = build_setting_slider(body, &(setting_slider_cfg_t) {
        .toggle_label = TR("Car mode"),
        .checked = current_settings.car_mode_enabled,
        .switch_cb = car_mode_enable_switch_event_cb,
        .caption = TR("Volume"),
        .max_index = 100,
        .index = current_settings.car_mode_volume_percent,
        .fmt = format_percent,
        .slider_cb = car_mode_volume_slider_event_cb,
        .control_always_on = true, /* the volume can be set before Car Mode is on */
    });

    build_car_mode_autoresume(body);
    build_car_mode_gain_row(body);
    lv_obj_add_event_cb(scr, car_mode_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    gui_settings_sync_car_mode();

    finalize_screen_navigation(scr);
    return scr;
}

void gui_volume_settings_car_mode_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_open_car_mode();
}

void gui_settings_open_car_mode(void) {
    if (!car_mode_screen) return;
    gui_settings_sync_car_mode();
    nav_push(car_mode_screen);
}

void gui_settings_sync_car_mode(void) {
    if (car_mode_volume_ui.toggle) {
        lv_obj_set_state(car_mode_volume_ui.toggle, LV_STATE_CHECKED, current_settings.car_mode_enabled);
    }
    if (car_mode_volume_ui.slider) {
        lv_slider_set_value(car_mode_volume_ui.slider, current_settings.car_mode_volume_percent, LV_ANIM_OFF);
        setting_slider_set_index(&car_mode_volume_ui, current_settings.car_mode_volume_percent);
    }
    if (car_mode_autoresume_switch) {
        if (current_settings.car_mode_autoresume_enabled) lv_obj_add_state(car_mode_autoresume_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(car_mode_autoresume_switch, LV_STATE_CHECKED);
    }
    if (car_mode_gain_switch) gui_native_audio_refresh_gain();
}

void gui_volume_settings_build_startup_max_screens(void) {
    startup_volume_screen = build_volume_screen();
}

void gui_volume_settings_build_car_mode_screen(void) {
    car_mode_screen = build_car_mode_screen();
}

void gui_volume_settings_teardown_startup_max_screens(void) {
    if (startup_volume_screen) { lv_obj_delete(startup_volume_screen); startup_volume_screen = NULL; }
    startup_volume_ui = (setting_slider_t) { 0 };
    max_volume_ui = (setting_slider_t) { 0 };
}

void gui_volume_settings_teardown_car_mode_screen(void) {
    if (car_mode_screen) { lv_obj_delete(car_mode_screen); car_mode_screen = NULL; }
    car_mode_autoresume_switch = NULL;
    car_mode_gain_switch = NULL;
    car_mode_volume_ui = (setting_slider_t) { 0 };
}
