#include "../setup/gui_setup.h"
#include "gui_volume_settings.h"
#include "../gui_settings.h"
#include "../../../audio/engine/audio.h"
#include "../../player/screen/gui_player.h"
#include "../../shell/gui_shell.h"
#include "../../shell/gui_navigation.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../plugins/manager/plugin_manager.h"
#include "../../../core/persistence/settings.h"
#include "../../text/i18n/i18n.h"

static lv_obj_t * startup_volume_screen;
static lv_obj_t * max_volume_screen;
static lv_obj_t * startup_volume_switch;
static lv_obj_t * startup_volume_slider_card;
static lv_obj_t * startup_volume_value_label;
static lv_obj_t * startup_volume_slider;
static lv_obj_t * max_volume_value_label;
static lv_obj_t * max_volume_slider;
static lv_obj_t * car_mode_volume_value_label;
static lv_obj_t * car_mode_volume_slider;
static lv_obj_t * car_mode_hint_label;
static lv_obj_t * car_mode_screen;
static lv_obj_t * car_mode_enable_switch;
static lv_obj_t * car_mode_autoresume_switch;
static lv_obj_t * car_mode_gain_row;
static lv_obj_t * car_mode_gain_dropdown;

static void startup_volume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_volume_fixed_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.startup_volume_fixed_enabled) {
        lv_obj_remove_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
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
        lv_label_set_text_fmt(startup_volume_value_label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* Mirrors build_screen_timeout_screen()'s own layout (toggle row + a
 * dark card holding a live-readout slider, shown/hidden with the toggle) --
 * see that function's comments for the reasoning behind the specific
 * measurements reused here. */
static lv_obj_t * build_startup_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Startup Volume"), generic_back_cb, NULL, NULL);

    /* Flex row with SPACE_BETWEEN layout to prevent text/switch overlap. */
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
    lv_label_set_text(enable_label, TR("Launch at a fixed volume"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    startup_volume_switch = lv_switch_create(enable_row);
    lv_obj_add_style(startup_volume_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.startup_volume_fixed_enabled) lv_obj_add_state(startup_volume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_volume_switch, startup_volume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    startup_volume_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, 100, current_settings.startup_volume_fixed_percent,
        startup_volume_slider_event_cb, &startup_volume_slider, &startup_volume_value_label);
    if (!current_settings.startup_volume_fixed_enabled) lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(startup_volume_value_label, "%d%%", current_settings.startup_volume_fixed_percent);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_volume_settings_startup_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(startup_volume_screen);
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
        lv_label_set_text_fmt(max_volume_value_label, "%d%%", (int) percent);
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

/* Simpler than build_startup_volume_screen(): no enable toggle, since 100%
 * (the default) already means "uncapped" with no separate off state to
 * track. */
static lv_obj_t * build_max_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Max Volume"), generic_back_cb, NULL, NULL);

    lv_obj_t * content = scr;
#if defined(BOARD_TEMPOTEC_V1)
    /* Keep the explanatory text and slider reachable on V1 without letting
     * the settings header or home-indicator area scroll away. */
    content = lv_obj_create(scr);
    int32_t content_y = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100),
                    BOARD_SCREEN_HEIGHT - content_y - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_pos(content, 0, content_y);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_top(content, BOARD_SCALE_PY(20), 0);
    lv_obj_set_style_pad_row(content, BOARD_SCALE_PY(20), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
#else
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20;
#endif

    lv_obj_t * row = lv_obj_create(content);
    lv_obj_set_width(row, lv_pct(90));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
#if defined(BOARD_TEMPOTEC_V1)
    /* The content parent's flex column keeps the slider below this row as
     * font-size changes alter the explanation's wrapped height. */
#else
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, content_top);
#endif
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 12, 0);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, TR("Limits the player volume, including buttons, remote controls and plugins."));
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(label, 1);

    build_setting_slider_card(content, row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        1, 100, current_settings.max_volume_percent,
        max_volume_slider_event_cb, &max_volume_slider, &max_volume_value_label);
    lv_label_set_text_fmt(max_volume_value_label, "%d%%", current_settings.max_volume_percent);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_volume_settings_max_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(max_volume_screen);
}

static void car_mode_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.car_mode_volume_percent = (int) percent;
        lv_label_set_text_fmt(car_mode_volume_value_label, "%d%%", (int) percent);
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

static void car_mode_gain_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int index = plugin_manager_find_quick_toggle_by_id("gain");
    if (index < 0) return;
    plugin_manager_quick_toggle_set(index, lv_dropdown_get_selected(lv_event_get_target(e)) == 1);
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

static void car_mode_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    gui_settings_sync_car_mode();
}

static lv_obj_t * build_car_mode_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Car Mode"), generic_back_cb, NULL, NULL);

    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    /* Car Mode enable row */
    lv_obj_t * enable_row = lv_obj_create(body);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, TR("Car Mode"));
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    car_mode_enable_switch = lv_switch_create(enable_row);
    lv_obj_add_style(car_mode_enable_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.car_mode_enabled) lv_obj_add_state(car_mode_enable_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(car_mode_enable_switch, car_mode_enable_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    car_mode_hint_label = lv_label_create(body);
    lv_label_set_text(car_mode_hint_label, TR("Car Mode is disabled."));
    lv_obj_set_width(car_mode_hint_label, lv_pct(90));
    lv_label_set_long_mode(car_mode_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(car_mode_hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(car_mode_hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    if (current_settings.car_mode_enabled) lv_obj_add_flag(car_mode_hint_label, LV_OBJ_FLAG_HIDDEN);

    /* Car Mode volume row */
    lv_obj_t * car_mode_row = lv_obj_create(body);
    lv_obj_set_width(car_mode_row, lv_pct(90));
    lv_obj_set_height(car_mode_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(car_mode_row, 0, 0);
    lv_obj_set_style_border_width(car_mode_row, 0, 0);
    lv_obj_remove_flag(car_mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(car_mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(car_mode_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(car_mode_row, 12, 0);

    lv_obj_t * car_mode_label = lv_label_create(car_mode_row);
    lv_label_set_text(car_mode_label, TR("Car Mode Volume"));
    lv_obj_add_style(car_mode_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(car_mode_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(car_mode_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(car_mode_label, 1);

    build_setting_slider_card(body, car_mode_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, 100, current_settings.car_mode_volume_percent,
        car_mode_volume_slider_event_cb, &car_mode_volume_slider, &car_mode_volume_value_label);
    lv_label_set_text_fmt(car_mode_volume_value_label, "%d%%", current_settings.car_mode_volume_percent);

    /* Independent of the general playback resume setting. */
    lv_obj_t * autoresume_row = lv_obj_create(body);
    lv_obj_set_width(autoresume_row, lv_pct(90));
    lv_obj_set_height(autoresume_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(autoresume_row, 0, 0);
    lv_obj_set_style_border_width(autoresume_row, 0, 0);
    lv_obj_remove_flag(autoresume_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(autoresume_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(autoresume_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(autoresume_row, 12, 0);
    lv_obj_t * autoresume_label = lv_label_create(autoresume_row);
    lv_label_set_text(autoresume_label, TR("Auto-resume"));
    lv_obj_add_style(autoresume_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(autoresume_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(autoresume_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(autoresume_label, 1);
    car_mode_autoresume_switch = lv_switch_create(autoresume_row);
    lv_obj_add_style(car_mode_autoresume_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.car_mode_autoresume_enabled) lv_obj_add_state(car_mode_autoresume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(car_mode_autoresume_switch, car_mode_autoresume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * autoresume_hint = lv_label_create(body);
    lv_label_set_text(autoresume_hint, TR("Resume playback when external power turns the player on."));
    lv_obj_set_width(autoresume_hint, lv_pct(90));
    lv_label_set_long_mode(autoresume_hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(autoresume_hint, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(autoresume_hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);

    /* Gain remains owned by the Gain plugin; this selector uses its registered
     * quick toggle so the plugin keeps its existing persistence and behavior. */
    car_mode_gain_row = lv_obj_create(body);
    lv_obj_set_width(car_mode_gain_row, lv_pct(90));
    lv_obj_set_height(car_mode_gain_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(car_mode_gain_row, 0, 0);
    lv_obj_set_style_border_width(car_mode_gain_row, 0, 0);
    lv_obj_remove_flag(car_mode_gain_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(car_mode_gain_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(car_mode_gain_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(car_mode_gain_row, 12, 0);
    lv_obj_t * gain_label = lv_label_create(car_mode_gain_row);
    lv_label_set_text(gain_label, TR("Gain"));
    lv_obj_add_style(gain_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(gain_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_flex_grow(gain_label, 1);
    car_mode_gain_dropdown = lv_dropdown_create(car_mode_gain_row);
    lv_dropdown_set_options(car_mode_gain_dropdown, TR("Low\nHigh"));
    gui_style_settings_dropdown(car_mode_gain_dropdown);
    lv_obj_set_width(car_mode_gain_dropdown, BOARD_SCALE_PX(170));
    lv_obj_add_event_cb(car_mode_gain_dropdown, car_mode_gain_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
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
    if (car_mode_enable_switch) {
        if (current_settings.car_mode_enabled) lv_obj_add_state(car_mode_enable_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(car_mode_enable_switch, LV_STATE_CHECKED);
    }
    if (car_mode_hint_label) {
        if (current_settings.car_mode_enabled) lv_obj_add_flag(car_mode_hint_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(car_mode_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (car_mode_volume_slider) {
        lv_slider_set_value(car_mode_volume_slider, current_settings.car_mode_volume_percent, LV_ANIM_OFF);
    }
    if (car_mode_volume_value_label) {
        lv_label_set_text_fmt(car_mode_volume_value_label, "%d%%", current_settings.car_mode_volume_percent);
    }
    if (car_mode_autoresume_switch) {
        if (current_settings.car_mode_autoresume_enabled) lv_obj_add_state(car_mode_autoresume_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(car_mode_autoresume_switch, LV_STATE_CHECKED);
    }
    if (car_mode_gain_row && car_mode_gain_dropdown) {
        int index = plugin_manager_find_quick_toggle_by_id("gain");
        if (index < 0) {
            lv_obj_add_flag(car_mode_gain_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(car_mode_gain_row, LV_OBJ_FLAG_HIDDEN);
            lv_dropdown_set_selected(car_mode_gain_dropdown,
                plugin_manager_get_quick_toggle_value(index) ? 1U : 0U);
        }
    }
}

void gui_volume_settings_build_startup_max_screens(void) {
    startup_volume_screen = build_startup_volume_screen();
    max_volume_screen = build_max_volume_screen();
}

void gui_volume_settings_build_car_mode_screen(void) {
    car_mode_screen = build_car_mode_screen();
}

void gui_volume_settings_teardown_startup_max_screens(void) {
    if (startup_volume_screen) { lv_obj_delete(startup_volume_screen); startup_volume_screen = NULL; }
    if (max_volume_screen) { lv_obj_delete(max_volume_screen); max_volume_screen = NULL; }
}

void gui_volume_settings_teardown_car_mode_screen(void) {
    if (car_mode_screen) { lv_obj_delete(car_mode_screen); car_mode_screen = NULL; }
    car_mode_enable_switch = NULL;
    car_mode_autoresume_switch = NULL;
    car_mode_gain_row = NULL;
    car_mode_gain_dropdown = NULL;
    car_mode_hint_label = NULL;
    car_mode_volume_value_label = NULL;
    car_mode_volume_slider = NULL;
}
