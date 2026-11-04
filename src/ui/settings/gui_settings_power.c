#include "../application/gui.h"
#include "gui_settings.h"
#include "controls/gui_setting_switches.h"
#include "../text/i18n/i18n.h"
#include "../network/gui_network.h"
#include "../widgets/builders/screen_builders.h"
#include <stdio.h>
#include "gui_settings_internal.h"

static lv_obj_t * settings_charging_screen;
static lv_obj_t * settings_usb_dac_charging_switch;
static lv_obj_t * settings_usb_dac_charging_row;
#if BOARD_R1_HARDWARE
static void usb_dac_charging_switch_event_cb(lv_event_t * e);
#endif
lv_obj_t * settings_power_screen;
static lv_obj_t * sleep_timer_screen;
static lv_obj_t * idle_shutdown_screen;

static setting_slider_t sleep_timer_ui;
static lv_obj_t * sleep_timer_remaining_btn;
static setting_slider_t idle_shutdown_ui;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;
static lv_obj_t * idle_action_poweroff_check;
static lv_obj_t * idle_action_suspend_check;

void gui_settings_open_sleep_timer(void) {
    if (!sleep_timer_screen) return;
    gui_settings_sync_sleep_timer_toggle();
    nav_push(sleep_timer_screen);
}

/* Index into SLEEP_TIMER_STEPS closest to `minutes' -- same reasoning as
 * screen_timeout_seconds_to_step_index() above. */
static int sleep_timer_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT, minutes);
}

/* Extracted from build_sleep_timer_screen() so it can be shared with its
 * own format_duration(), needed now that SLEEP_TIMER_STEPS reaches 180
 * (settings.c) and a bare "%d min" would read as "180 min" instead of the
 * much more readable "3 hr". */
static void format_sleep_timer_duration(char * buf, size_t buf_size, int minutes) {
    int hours = minutes / 60;
    int remainder = minutes % 60;
    if (hours == 0) snprintf(buf, buf_size, TR("%d min"), remainder);
    else if (remainder == 0) snprintf(buf, buf_size, TR("%d hr"), hours);
    else snprintf(buf, buf_size, TR("%d hr %d min"), hours, remainder);
}

/* Enables the duration control and shows the "Show Time Remaining" button
 * only while the timer is armed. */
static void sleep_timer_apply_visibility(bool active) {
    setting_slider_set_enabled(&sleep_timer_ui, active);
    lv_obj_set_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN, !active);
}

/* Arms or disarms the sleep timer directly, synchronizing state with the
 * quick drawer sleep timer toggle. */
static void sleep_timer_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    quick_drawer_sleep_timer_set_active(active);
    sleep_timer_apply_visibility(active);
}

/* This only sets the duration the quick-drawer sleep icon/enable toggle
 * above arms next time (or restarts the CURRENT countdown at, if already
 * armed -- same real-time-restart behavior ExtendedSleepTimer.lua's own
 * duration slider has, matching what a user dragging this while counting
 * down would expect: the countdown they're looking at should reflect the
 * duration they just picked, not the one that was armed before). */
static void sleep_timer_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = SLEEP_TIMER_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.sleep_timer_minutes = minutes;
        setting_slider_set_index(&sleep_timer_ui, index);
        if (quick_drawer_sleep_timer_is_active()) quick_drawer_sleep_timer_set_active(true);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save_async(&current_settings);
    }
}

/* "Show Time Remaining" button -- same info ExtendedSleepTimer.lua's own
 * "Show Time Remaining" settings row surfaces via a toast, in the same
 * H:MM:SS (or M:SS under an hour) shape as that plugin's format_remaining().
 * Hidden whenever the timer is disarmed (sleep_timer_switch_event_cb /
 * gui_settings_sync_sleep_timer_toggle), so this is only ever reachable
 * while armed -- no "timer is off" fallback message needed here. */
static void sleep_timer_show_remaining_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int total_seconds = quick_drawer_sleep_timer_remaining_seconds();
    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int secs = total_seconds % 60;
    char buf[48];
    if (hours > 0) snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d:%02d"), hours, minutes, secs);
    else snprintf(buf, sizeof(buf), TR("Time remaining: %d:%02d"), minutes, secs);
    show_info_toast(buf);
}

/* "Show Time Remaining" button below the duration card. */
static void build_sleep_timer_remaining_btn(lv_obj_t * body) {
    sleep_timer_remaining_btn = lv_obj_create(body);
    lv_obj_set_size(sleep_timer_remaining_btn, lv_pct(90), BOARD_SCALE_PX(70));
    lv_obj_add_style(sleep_timer_remaining_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(sleep_timer_remaining_btn, 0, 0);
    lv_obj_set_style_radius(sleep_timer_remaining_btn, 10, 0);
    lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_timer_remaining_btn, sleep_timer_show_remaining_cb, LV_EVENT_CLICKED, NULL);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * remaining_label = lv_label_create(sleep_timer_remaining_btn);
    lv_label_set_text(remaining_label, TR("Show Time Remaining"));
    lv_obj_add_style(remaining_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remaining_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(remaining_label);
}

/* Mirrors build_screen_timeout_screen()'s layout (enable row + slider
 * card), plus one more row: a "Show Time Remaining" button neither that
 * screen nor this one needed before ExtendedSleepTimer.lua's own version
 * of this same feature demonstrated why one's genuinely useful here (the
 * status bar has nowhere to show a running countdown, unlike the quick
 * drawer's own quick_drawer_sleep_label). */
static lv_obj_t * build_sleep_timer_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Sleep Timer"), generic_back_cb, NULL, NULL);

    lv_obj_t * body = build_settings_body(scr);
    sleep_timer_ui = build_setting_slider(body, &(setting_slider_cfg_t) {
        .toggle_label = TR("Enable Sleep Timer"),
        .checked = quick_drawer_sleep_timer_is_active(),
        .switch_cb = sleep_timer_switch_event_cb,
        .caption = TR("Stop after"),
        .max_index = SLEEP_TIMER_STEP_COUNT - 1,
        .index = sleep_timer_minutes_to_step_index(current_settings.sleep_timer_minutes),
        .steps = SLEEP_TIMER_STEPS,
        .fmt = format_sleep_timer_duration,
        .slider_cb = sleep_timer_slider_event_cb,
    });

    build_sleep_timer_remaining_btn(body);

    finalize_screen_navigation(scr);
    return scr;
}

/* All IDLE_SHUTDOWN_STEPS values are whole minutes, so unlike
 * format_screen_timeout() this never needs a seconds remainder -- "10m" /
 * "1h" / "2h", not "1h 30m", since 90 isn't one of the steps. */
static void format_idle_shutdown(char * buf, size_t buf_size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, buf_size, "%dm", minutes);
        return;
    }
    snprintf(buf, buf_size, "%dh", minutes / 60);
}

static int idle_shutdown_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT, minutes);
}

static void idle_shutdown_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.idle_shutdown_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);

    ui_set_greyed(idle_action_section, !current_settings.idle_shutdown_enabled);
    setting_slider_set_enabled(&idle_shutdown_ui, current_settings.idle_shutdown_enabled);
}

/* Mutually-exclusive idle-action choice (Power Off vs Suspend to RAM) --
 * replaces the old single "Suspend instead of power off" switch, which
 * modeled this same 2-way choice as a toggle acting on an implicit
 * opposite (plain poweroff being "off"). Same accent-border-highlight
 * pill-row selection language as populate_usb_mode_screen()'s own Storage/
 * USB DAC rows (add_pill_row_base()), but inline on this same screen
 * rather than a separate list screen, since it stays right above the
 * slider it's a sibling setting of. */
static void idle_action_choice_cb(lv_event_t * e);

/* Paints the selection ring and check mark for the chosen idle action. */
static void idle_action_apply_selection(bool suspend) {
    lv_obj_set_style_border_width(idle_action_poweroff_row, suspend ? 0 : 3, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, suspend ? 3 : 0, 0);
    lv_obj_set_flag(idle_action_poweroff_check, LV_OBJ_FLAG_HIDDEN, suspend);
    lv_obj_set_flag(idle_action_suspend_check, LV_OBJ_FLAG_HIDDEN, !suspend);
}

/* One selectable idle-action row inside `section`. `suspend` is the
 * action this row stands for; it travels as the click's user_data. */
static lv_obj_t * add_idle_action_row(lv_obj_t * section, const char * label, bool suspend,
                                       lv_obj_t ** out_check) {
    lv_obj_t * row = add_pill_row_base(section, label);
    lv_obj_add_style(row, &style_theme_card_bg, 0);
    /* Keep the accent outline last: style_theme_card_bg also defines a
     * border color and otherwise hides the selection accent. The shared
     * outline style is transparent, so restore the card fill explicitly. */
    lv_obj_add_style(row, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) suspend);
    lv_obj_t * check = lv_label_create(row);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_add_style(check, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(check, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(check, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
    lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    *out_check = check;
    return row;
}

static void idle_action_choice_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool suspend = (bool) (intptr_t) lv_event_get_user_data(e);
    current_settings.idle_suspend_enabled = suspend;
    settings_save_async(&current_settings);
    idle_action_apply_selection(suspend);
}

static void idle_shutdown_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = IDLE_SHUTDOWN_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.idle_shutdown_minutes = minutes;
        setting_slider_set_index(&idle_shutdown_ui, index);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save_async(&current_settings);
    }
}

/* Transparent column for the idle-action choice. It is content-sized, so the
 * rows follow font-size changes instead of overflowing a fixed height
 * (GitHub issue #91). */
static void create_idle_action_section(lv_obj_t * content) {
    idle_action_section = lv_obj_create(content);
    lv_obj_set_size(idle_action_section, ui_list_row_width(), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_all(idle_action_section, 0, 0);
    lv_obj_set_style_pad_row(idle_action_section, BOARD_SCALE_PX(10), 0);
    lv_obj_set_flex_flow(idle_action_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
}

/* Power Off / Suspend choice, the next item in the column. Greyed out and
 * disabled with the idle-shutdown toggle in idle_shutdown_switch_event_cb(). */
static void build_idle_action_section(lv_obj_t * content) {
    create_idle_action_section(content);

    lv_obj_t * explain = lv_label_create(idle_action_section);
    lv_label_set_text(explain, TR("Choose what happens when idle:"));
    lv_obj_add_style(explain, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explain, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);

    idle_action_poweroff_row = add_idle_action_row(idle_action_section, TR("Power Off"),
        false, &idle_action_poweroff_check);
    idle_action_suspend_row = add_idle_action_row(idle_action_section, TR("Suspend to RAM"),
        true, &idle_action_suspend_check);
    idle_action_apply_selection(current_settings.idle_suspend_enabled);
    ui_set_greyed(idle_action_section, !current_settings.idle_shutdown_enabled);
}

/* Toggle and timeout slider in one card at the top; the idle-action choice
 * sits below it, so the card keeps the same look as every other slider page. */
static void build_idle_timeout_card(lv_obj_t * content) {
    idle_shutdown_ui = build_setting_slider(content, &(setting_slider_cfg_t) {
        .toggle_label = TR("Automatically go idle"),
        .checked = current_settings.idle_shutdown_enabled,
        .switch_cb = idle_shutdown_switch_event_cb,
        .caption = TR("Go idle after"),
        .max_index = IDLE_SHUTDOWN_STEP_COUNT - 1,
        .index = idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes),
        .steps = IDLE_SHUTDOWN_STEPS,
        .fmt = format_idle_shutdown,
        .slider_cb = idle_shutdown_slider_event_cb,
    });
}

static lv_obj_t * build_idle_shutdown_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Idle Shutdown"), generic_back_cb, NULL, NULL);

    lv_obj_t * content = build_settings_body(scr);

    build_idle_timeout_card(content);
    build_idle_action_section(content);

    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Power list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
PLUGIN_ROW_CLICK_CB(plugin_power_list_item_click_cb, plugin_manager_power_list_item_clicked)

static lv_obj_t * build_settings_charging_screen(void) {
    pill_list_item_t items[] = {
        { TR("Charge Limit (85%)"), PILL_ACCESSORY_TOGGLE,
          current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL },
        { TR("Safe Charging (500mA)"), PILL_ACCESSORY_TOGGLE,
          current_settings.safe_charging_enabled, NULL, safe_charging_switch_event_cb, NULL },
        { TR("LED charge indicator"), PILL_ACCESSORY_TOGGLE,
          current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL },
#if BOARD_R1_HARDWARE
        {
            .label = TR("Disable charging\nin USB DAC mode"),
            .accessory = PILL_ACCESSORY_TOGGLE,
            .toggle_initial_state = current_settings.usb_dac_disable_charging,
            .on_toggle_change = usb_dac_charging_switch_event_cb,
            .out_toggle_img = &settings_usb_dac_charging_switch,
            .row_height = BOARD_SCALE_PY(128),
            .out_row = &settings_usb_dac_charging_row,
        },
#endif
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Charging"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
#if BOARD_R1_HARDWARE
    if (settings_usb_dac_charging_row && settings_usb_dac_charging_switch) {
        lv_obj_t * label = lv_obj_get_child(settings_usb_dac_charging_row, 0);
        /* This two-line setting must stay wrapped during font refreshes. */
        lv_obj_remove_flag(label, LV_OBJ_FLAG_USER_3);
        lv_obj_set_style_pad_top(label, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), 0);
    }
#endif
    finalize_screen_navigation(scr);
    return scr;
}

#if BOARD_R1_HARDWARE
static void usb_dac_charging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_network_set_usb_dac_charging_disabled(
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

void gui_settings_sync_usb_dac_charging_toggle(void) {
    if (!settings_usb_dac_charging_switch) return;
    lv_obj_set_state(settings_usb_dac_charging_switch, LV_STATE_CHECKED,
                     current_settings.usb_dac_disable_charging);
}

static void settings_charging_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_charging_screen, build_settings_charging_screen);
}

static void sleep_timer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_settings_open_sleep_timer();
}

PLUGIN_ROW_CLICK_CB(plugin_music_timers_list_item_click_cb, plugin_manager_music_timers_list_item_clicked)

static lv_obj_t * build_settings_power_screen(void) {
    pill_list_item_t items[3 + PLUGIN_MAX_POWER_LIST_ITEMS + PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS];
    lv_obj_t * sleep_row = NULL;
    items[0] = (pill_list_item_t){ TR("Sleep Timer"), PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };
    items[0].out_row = &sleep_row;
    items[1] = (pill_list_item_t){ TR("Idle Shutdown"), PILL_ACCESSORY_CHEVRON, false, push_screen_slot_cb, NULL, &idle_shutdown_screen };
    items[2] = (pill_list_item_t){ TR("Charging"), PILL_ACCESSORY_CHEVRON, false, settings_charging_row_cb, NULL, NULL };
    int count = 3;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS,
                                    plugin_manager_get_music_timers_list_item_count,
                                    plugin_manager_get_music_timers_list_item_label,
                                    plugin_manager_get_music_timers_list_item_options,
                                    plugin_music_timers_list_item_click_cb);
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_POWER_LIST_ITEMS,
                                    plugin_manager_get_power_list_item_count,
                                    plugin_manager_get_power_list_item_label,
                                    plugin_manager_get_power_list_item_options,
                                    plugin_power_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Power"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_sleep_summary = add_pill_summary_label(sleep_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

void settings_power_build_screens(void) {
    sleep_timer_screen = build_sleep_timer_screen();
    idle_shutdown_screen = build_idle_shutdown_screen();
    settings_power_screen = build_settings_power_screen();
}

void settings_power_teardown_screens(void) {
    delete_screen(&sleep_timer_screen);
    sleep_timer_ui = (setting_slider_t) { 0 };
    sleep_timer_remaining_btn = NULL;
    delete_screen(&idle_shutdown_screen);
    idle_shutdown_ui = (setting_slider_t) { 0 };
    idle_action_section = NULL;
    idle_action_poweroff_row = idle_action_suspend_row = NULL;
    idle_action_poweroff_check = idle_action_suspend_check = NULL;
    delete_screen(&settings_charging_screen);
    settings_usb_dac_charging_switch = settings_usb_dac_charging_row = NULL;
    delete_screen(&settings_power_screen);
}

/* Same bidirectional-sync role as gui_settings_sync_crossfade_toggle()
 * above, for the Sleep Timer screen's own enable switch -- called from
 * gui_shell_drawer.c whenever the drawer icon arms/disarms the timer, or the
 * countdown expires on its own, so this screen never shows a stale state
 * the next time it's opened. Also enables/greys the duration control and shows/hides
 * the "Show Time Remaining" button, matching sleep_timer_switch_event_cb's
 * own behavior when the switch is toggled from this screen directly. */
void gui_settings_sync_sleep_timer_toggle(void) {
    if (!sleep_timer_ui.toggle) return;
    bool active = quick_drawer_sleep_timer_is_active();
    lv_obj_set_state(sleep_timer_ui.toggle, LV_STATE_CHECKED, active);
    sleep_timer_apply_visibility(active);
}
