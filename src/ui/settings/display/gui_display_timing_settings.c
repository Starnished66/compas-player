#include "gui_display_timing_settings.h"

#include "../../../hardware/indicators/backlight.h"
#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/settings.h"
#include "../../appearance/gui_theme.h"

#include <stdio.h>

static lv_obj_t * screen_timeout_screen;
static lv_obj_t * screen_dimming_screen;
static setting_slider_t screen_timeout_ui;
static setting_slider_t screen_dimming_ui;

/* "45s" below 1 minute, "1m"/"2m" on an exact minute, "1m 30s" otherwise --
 * matches the 30s-10min range (SCREEN_TIMEOUT_MIN/MAX_SECONDS) without ever
 * needing more than whole minutes+seconds. */
static void format_screen_timeout(char * buf, size_t buf_size, int seconds) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%ds", seconds);
        return;
    }
    int minutes = seconds / 60;
    int remainder = seconds % 60;
    if (remainder == 0) snprintf(buf, buf_size, "%dm", minutes);
    else snprintf(buf, buf_size, "%dm %ds", minutes, remainder);
}

/* Index of the closest shared timeout preset. */
static int screen_timeout_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT, seconds);
}

static void screen_timeout_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_timeout_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);

    setting_slider_set_enabled(&screen_timeout_ui, current_settings.screen_timeout_enabled);
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as the EQ sliders
 * (eq_preamp_slider_event_cb etc.) -- writing settings.c's file on every
 * drag tick would be needless disk I/O for a value that only matters once
 * the user lets go. The slider itself moves over step indices (0..
 * SCREEN_TIMEOUT_STEP_COUNT-1), mapped onto the shared preset table. */
static void screen_timeout_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_TIMEOUT_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_timeout_seconds = seconds;
        setting_slider_set_index(&screen_timeout_ui, index);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save_async(&current_settings);
    }
}

static lv_obj_t * build_screen_timeout_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Timeout"), generic_back_cb, NULL, NULL);

    screen_timeout_ui = build_setting_slider(scr, &(setting_slider_cfg_t) {
        .toggle_label = TR("Turn off screen automatically"),
        .checked = current_settings.screen_timeout_enabled,
        .switch_cb = screen_timeout_switch_event_cb,
        .caption = TR("Turn off after"),
        .max_index = SCREEN_TIMEOUT_STEP_COUNT - 1,
        .index = screen_timeout_seconds_to_step_index(current_settings.screen_timeout_seconds),
        .steps = SCREEN_TIMEOUT_STEPS,
        .fmt = format_screen_timeout,
        .slider_cb = screen_timeout_slider_event_cb,
    });
    lv_obj_align(screen_timeout_ui.card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_display_timing_settings_timeout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_timeout_screen);
}

static int screen_dim_delay_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT, seconds);
}

static void screen_dimming_ui_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_dimming_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (!current_settings.screen_dimming_enabled) {
        backlight_set_dimmed(false);
    }
    settings_save_async(&current_settings);

    setting_slider_set_enabled(&screen_dimming_ui, current_settings.screen_dimming_enabled);
}

static void screen_dim_delay_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_DIM_DELAY_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_dim_delay_seconds = seconds;
        setting_slider_set_index(&screen_dimming_ui, index);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save_async(&current_settings);
    }
}

static void screen_dimming_screen_loaded_cb(lv_event_t * e) {
    int effective_max_index = SCREEN_DIM_DELAY_STEP_COUNT - 1;
    if (current_settings.screen_timeout_enabled) {
        effective_max_index = -1;
        for (int i = SCREEN_DIM_DELAY_STEP_COUNT - 1; i >= 0; i--) {
            if (SCREEN_DIM_DELAY_STEPS[i] < current_settings.screen_timeout_seconds) {
                effective_max_index = i;
                break;
            }
        }
        if (effective_max_index == -1) {
            effective_max_index = 0;
        }
    }

    setting_slider_set_max_index(&screen_dimming_ui, effective_max_index);

    int current_index = screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds);
    if (current_index > effective_max_index) {
        current_settings.screen_dim_delay_seconds = SCREEN_DIM_DELAY_STEPS[effective_max_index];
        lv_slider_set_value(screen_dimming_ui.slider, effective_max_index, LV_ANIM_OFF);
        setting_slider_set_index(&screen_dimming_ui, effective_max_index);
        settings_save_async(&current_settings);
    }
}

static lv_obj_t * build_screen_dimming_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, screen_dimming_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Dimming"), generic_back_cb, NULL, NULL);

    screen_dimming_ui = build_setting_slider(scr, &(setting_slider_cfg_t) {
        .toggle_label = TR("Dim screen before timeout"),
        .checked = current_settings.screen_dimming_enabled,
        .switch_cb = screen_dimming_ui_switch_event_cb,
        .caption = TR("Dim after"),
        .max_index = SCREEN_DIM_DELAY_STEP_COUNT - 1,
        .index = screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds),
        .steps = SCREEN_DIM_DELAY_STEPS,
        .fmt = format_screen_timeout,
        .slider_cb = screen_dim_delay_slider_event_cb,
    });
    lv_obj_align(screen_dimming_ui.card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_display_timing_settings_dimming_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_dimming_screen);
}


void gui_display_timing_settings_build_screens(void) {
    screen_timeout_screen = build_screen_timeout_screen();
    screen_dimming_screen = build_screen_dimming_screen();
}

void gui_display_timing_settings_teardown(void) {
    if (screen_timeout_screen) { lv_obj_delete(screen_timeout_screen); screen_timeout_screen = NULL; }
    if (screen_dimming_screen) { lv_obj_delete(screen_dimming_screen); screen_dimming_screen = NULL; }
}
