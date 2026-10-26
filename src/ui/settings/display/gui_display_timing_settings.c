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
static lv_obj_t * screen_dimming_switch;
static lv_obj_t * screen_dimming_slider_card;
static lv_obj_t * screen_dimming_slider;
static lv_obj_t * screen_dimming_value_label;
static lv_obj_t * screen_timeout_switch;
static lv_obj_t * screen_timeout_slider_card;
static lv_obj_t * screen_timeout_value_label;
static lv_obj_t * screen_timeout_slider;

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
    settings_save(&current_settings);

    if (current_settings.screen_timeout_enabled) {
        lv_obj_remove_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
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
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_timeout_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_timeout_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Timeout"), generic_back_cb, NULL, NULL);

    /* Enable row configured with flex layout (SPACE_BETWEEN and flex-grow)
     * so the text label wraps cleanly across font sizes without overlapping
     * the switch. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Turn off screen automatically"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_timeout_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_timeout_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_timeout_enabled) lv_obj_add_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_timeout_switch, screen_timeout_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Rounded slider card with vertical clearance below the track for the
     * knob diameter and centered value label. */
    screen_timeout_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_TIMEOUT_STEP_COUNT - 1,
        screen_timeout_seconds_to_step_index(current_settings.screen_timeout_seconds),
        screen_timeout_slider_event_cb, &screen_timeout_slider, &screen_timeout_value_label);
    if (!current_settings.screen_timeout_enabled) lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_timeout_seconds);
    lv_label_set_text(screen_timeout_value_label, initial_buf);

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
    settings_save(&current_settings);

    if (current_settings.screen_dimming_enabled) {
        lv_obj_remove_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_dim_delay_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_DIM_DELAY_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_dim_delay_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
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

    lv_slider_set_range(screen_dimming_slider, 0, effective_max_index);

    int current_index = screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds);
    if (current_index > effective_max_index) {
        current_settings.screen_dim_delay_seconds = SCREEN_DIM_DELAY_STEPS[effective_max_index];
        lv_slider_set_value(screen_dimming_slider, effective_max_index, LV_ANIM_OFF);
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), current_settings.screen_dim_delay_seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_dimming_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, screen_dimming_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Screen Dimming"), generic_back_cb, NULL, NULL);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Dim screen before timeout"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_dimming_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_dimming_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_dimming_enabled) lv_obj_add_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_dimming_switch, screen_dimming_ui_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    screen_dimming_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_DIM_DELAY_STEP_COUNT - 1,
        screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds),
        screen_dim_delay_slider_event_cb, &screen_dimming_slider, &screen_dimming_value_label);
    if (!current_settings.screen_dimming_enabled) lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_dim_delay_seconds);
    lv_label_set_text(screen_dimming_value_label, initial_buf);

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
