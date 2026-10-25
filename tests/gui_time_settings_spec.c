/* Production-backed clock/timezone characterization. The baseline runner
 * extracts the old functions and state from frozen gui_settings.c/player.c;
 * the final runner source-includes gui_time_settings.c. Clock and timezone
 * services are always mocked, so this test never changes the host clock/TZ. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "settings.h"
#include "app_clock.h"
#include "timezone_apply.h"
#include "timezone_data.h"
#include "gui_shell.h"
#include "gui_notifications.h"
#include "gui_navigation.h"
#include "gui_setup.h"
#include "gui_theme.h"
#include "gui_plugins.h"
#include "screen_builders.h"
#include "i18n.h"

/* Kept as a focused compatibility seam when this callback moves out of the
 * player implementation. */
void clock_24h_switch_event_cb(lv_event_t * e);

player_settings_t current_settings;
lv_style_t style_theme_screen_bg;
lv_style_t style_theme_card_bg;
lv_style_t style_theme_text_primary;
lv_style_t style_theme_text_muted;

static lv_style_t accent_style;
static bool test_font_glyph(const lv_font_t * font, lv_font_glyph_dsc_t * dsc,
                            uint32_t letter, uint32_t next_letter) {
    LV_UNUSED(letter);
    LV_UNUSED(next_letter);
    memset(dsc, 0, sizeof(*dsc));
    dsc->resolved_font = font;
    dsc->adv_w = 12;
    dsc->box_w = 10;
    dsc->box_h = 16;
    return true;
}
static const void * test_font_bitmap(lv_font_glyph_dsc_t * dsc, lv_draw_buf_t * draw_buf) {
    LV_UNUSED(dsc);
    LV_UNUSED(draw_buf);
    return NULL;
}
static lv_font_t test_font = {
    .get_glyph_dsc = test_font_glyph,
    .get_glyph_bitmap = test_font_bitmap,
    .line_height = 20,
};
/* This host configuration disables Montserrat 16 but LVGL's display
 * constructor still references LV_FONT_DEFAULT before our test theme is set. */
lv_font_t lv_font_montserrat_16 = {
    .get_glyph_dsc = test_font_glyph,
    .get_glyph_bitmap = test_font_bitmap,
    .line_height = 20,
};
static lv_display_t * display;
static lv_obj_t * built_region_screen;
static lv_obj_t * built_clock_screen;
static lv_obj_t * built_set_time_screen;
static lv_obj_t * built_city_screen;
static lv_obj_t * last_pushed_screen;
static int nav_depth;
static int settings_save_count;
static int settings_save_async_count;
static int app_clock_automatic_count;
static int app_clock_set_time_count;
static int clock_persistence_count;
static int refresh_clock_count;
static int timezone_apply_count;
static int error_toast_count;
static int info_toast_count;
static bool timezone_apply_succeeds = true;
static bool app_clock_automatic;
static struct tm mocked_localtime;
static int mocked_set_hour;
static int mocked_set_minute;
static int delete_order[8];
static int delete_count;
static const char * last_applied_timezone;
static lv_obj_t * compact_list;
static compact_list_click_cb_t compact_click;
static char compact_labels[TIMEZONE_TABLE_COUNT][COMPACT_LIST_LABEL_MAX];
static int compact_label_count;
static char build_order[8][64];
static int build_count;
static char effect_trace[1024];
static size_t effect_trace_len;
static void trace_effect(const char *s) { size_t n=strlen(s); assert(n<sizeof(effect_trace)-effect_trace_len); memcpy(effect_trace+effect_trace_len,s,n+1); effect_trace_len+=n; }

#ifdef GUI_TIME_SETTINGS_FINAL
#include "gui_time_settings.c"
#define time_spec_build_screens() gui_time_settings_build_screens()
#define time_spec_teardown() gui_time_settings_teardown()
#define time_spec_clock_row_cb(e) gui_time_settings_clock_row_cb(e)
#else
#include "gui_time_settings_baseline_fixture.c"
#define time_spec_build_screens() gui_time_spec_build_screens()
#define time_spec_teardown() gui_time_spec_teardown()
#define time_spec_clock_row_cb(e) gui_time_spec_clock_row_cb(e)
#endif

static void dispatch_clock_row_cb(lv_event_t * event) {
    time_spec_clock_row_cb(event);
}

static void flush_ready(lv_display_t * d, const lv_area_t * area, uint8_t * pixels) {
    LV_UNUSED(area);
    LV_UNUSED(pixels);
    lv_display_flush_ready(d);
}

const char * i18n_tr(const char * text) { return text; }

const lv_font_t * gui_theme_font(gui_font_role_t role) {
    LV_UNUSED(role);
    return &test_font;
}

lv_style_t * gui_theme_accent_style(void) { return &accent_style; }

void settings_save(const player_settings_t * settings) {
    assert(settings == &current_settings);
    settings_save_count++;
    trace_effect("save;");
}

void settings_save_async(const player_settings_t * settings) {
    assert(settings == &current_settings);
    settings_save_async_count++;
    trace_effect("save-async;");
}

void app_clock_localtime(struct tm * out) { *out = mocked_localtime; }

void app_clock_set_automatic(bool automatic) {
    app_clock_automatic = automatic;
    app_clock_automatic_count++;
    trace_effect(automatic ? "automatic:on;" : "automatic:off;");
}

void app_clock_set_local_time(int hour, int minute) {
    mocked_set_hour = hour;
    mocked_set_minute = minute;
    app_clock_set_time_count++;
    trace_effect("set-time;");
}

void app_clock_get_persistence(int64_t * manual_epoch, int64_t * system_reference) {
    if (manual_epoch) *manual_epoch = 123456;
    if (system_reference) *system_reference = 654321;
    clock_persistence_count++;
    trace_effect("persistence;");
}

bool timezone_apply(const char * iana_id) {
    last_applied_timezone = iana_id;
    timezone_apply_count++;
    trace_effect("timezone-apply;");
    return timezone_apply_succeeds;
}

void refresh_clock_label(void) { refresh_clock_count++; trace_effect("refresh;"); }
void refresh_battery_topbar(void) {}
void generic_back_cb(lv_event_t * event) { LV_UNUSED(event); }
void show_error_toast(const char * message) { LV_UNUSED(message); error_toast_count++; }
void show_info_toast(const char * message) { LV_UNUSED(message); info_toast_count++; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
int32_t ui_list_row_width(void) { return 448; }

void nav_push(lv_obj_t * screen) {
    last_pushed_screen = screen;
    nav_depth++;
}

void nav_pop(void) { nav_depth--; trace_effect("nav-pop;"); }
void nav_remove_stack_slot(int index) {
    assert(index >= 0 && index < nav_depth);
    nav_depth--;
}
int gui_navigation_get_depth(void) { return nav_depth; }

static void screen_deleted(lv_event_t * event) {
    lv_obj_t * screen = lv_event_get_target(event);
    const char * title = lv_obj_get_user_data(screen);
    int id = title && strcmp(title, "Time Zone") == 0 ? 1 :
             title && strcmp(title, "Clock") == 0 ? 2 :
             title && strcmp(title, "Set Time") == 0 ? 3 : 4;
    assert(delete_count < (int)(sizeof(delete_order) / sizeof(delete_order[0])));
    delete_order[delete_count++] = id;
}

static void set_screen_title(lv_obj_t * screen, const char * title) {
    lv_obj_set_user_data(screen, (void *)title);
    lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, NULL);
    assert(build_count < (int)(sizeof(build_order) / sizeof(build_order[0])));
    snprintf(build_order[build_count++], sizeof(build_order[0]), "%s", title);
}

lv_obj_t * build_screen_header(lv_obj_t * screen, const char * title,
                               lv_event_cb_t back_cb, const char * trailing_asset,
                               lv_event_cb_t trailing_cb) {
    LV_UNUSED(back_cb);
    LV_UNUSED(trailing_asset);
    LV_UNUSED(trailing_cb);
    if (strcmp(title, "Set Time") == 0) {
        built_set_time_screen = screen;
        set_screen_title(screen, title);
    }
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, title);
    return label;
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_cb,
                                  const pill_list_item_t * items, int item_count,
                                  lv_style_t * toggle_style, int32_t row_gap,
                                  int32_t icon_scale_pct) {
    LV_UNUSED(back_cb);
    LV_UNUSED(toggle_style);
    LV_UNUSED(row_gap);
    LV_UNUSED(icon_scale_pct);
    lv_obj_t * screen = lv_obj_create(NULL);
    set_screen_title(screen, title);
    if (strcmp(title, "Time Zone") == 0) built_region_screen = screen;
    if (strcmp(title, "Clock") == 0) built_clock_screen = screen;
    for (int i = 0; i < item_count; i++) {
        lv_obj_t * row = lv_obj_create(screen);
        lv_obj_set_size(row, 420, 64);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, items[i].label);
        if (items[i].out_row) *items[i].out_row = row;
        if (items[i].accessory == PILL_ACCESSORY_TOGGLE) {
            lv_obj_t * toggle = lv_switch_create(row);
            if (items[i].toggle_initial_state) lv_obj_add_state(toggle, LV_STATE_CHECKED);
            lv_obj_add_event_cb(toggle, items[i].on_toggle_change, LV_EVENT_VALUE_CHANGED,
                                items[i].user_data);
            if (items[i].out_toggle_img) *items[i].out_toggle_img = toggle;
        } else if (items[i].on_click) {
            lv_obj_add_event_cb(row, items[i].on_click, LV_EVENT_CLICKED, items[i].user_data);
        }
    }
    return screen;
}

lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_cb,
                                    const compact_list_item_t * items, int item_count,
                                    compact_list_click_cb_t on_click,
                                    compact_list_click_cb_t on_long_press,
                                    lv_obj_t ** out_list, lv_obj_t ** out_title_label,
                                    int32_t row_width, bool enable_now_playing,
                                    lv_style_t * now_playing_style) {
    LV_UNUSED(back_cb);
    LV_UNUSED(on_long_press);
    LV_UNUSED(row_width);
    LV_UNUSED(enable_now_playing);
    LV_UNUSED(now_playing_style);
    lv_obj_t * screen = lv_obj_create(NULL);
    set_screen_title(screen, title);
    built_city_screen = screen;
    compact_list = lv_obj_create(screen);
    compact_click = on_click;
    compact_label_count = item_count;
    assert(item_count <= TIMEZONE_TABLE_COUNT);
    for (int i = 0; i < item_count; i++) {
        snprintf(compact_labels[i], sizeof(compact_labels[i]), "%s", items[i].label);
        lv_obj_t * label = lv_label_create(compact_list);
        lv_label_set_text(label, items[i].label);
    }
    if (out_list) *out_list = compact_list;
    if (out_title_label) {
        *out_title_label = lv_label_create(screen);
        lv_label_set_text(*out_title_label, title);
    }
    return screen;
}

void finalize_screen_navigation(lv_obj_t * screen) { LV_UNUSED(screen); }
void configure_scrolling_row_label(lv_obj_t * label, int32_t width) {
    lv_obj_set_width(label, width);
}

static lv_obj_t * find_label(lv_obj_t * root, const char * text) {
    if (!root) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) &&
        strcmp(lv_label_get_text(root), text) == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t * found = find_label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t * find_type_inner(lv_obj_t * root, const lv_obj_class_t * class_p,
                                  int * remaining) {
    if (!root) return NULL;
    if (lv_obj_check_type(root, class_p)) {
        if (*remaining == 0) return root;
        (*remaining)--;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t * found = find_type_inner(lv_obj_get_child(root, i), class_p, remaining);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t * find_type(lv_obj_t * root, const lv_obj_class_t * class_p, int ordinal) {
    return find_type_inner(root, class_p, &ordinal);
}

static lv_obj_t * row_with_label(lv_obj_t * screen, const char * text) {
    lv_obj_t * label = find_label(screen, text);
    return label ? lv_obj_get_parent(label) : NULL;
}

static void reset_effects(void) {
    settings_save_count = settings_save_async_count = 0;
    app_clock_automatic_count = app_clock_set_time_count = 0;
    clock_persistence_count = refresh_clock_count = 0;
    timezone_apply_count = error_toast_count = info_toast_count = 0;
    last_applied_timezone = NULL;
    effect_trace_len=0; effect_trace[0]=0;
}

static void test_build_order_and_clock(void) {
    time_spec_build_screens();
    assert(build_count == 3);
    assert(strcmp(build_order[0], "Time Zone") == 0);
    assert(strcmp(build_order[1], "Set Time") == 0);
    assert(strcmp(build_order[2], "Clock") == 0);
    assert(built_region_screen && built_set_time_screen && built_clock_screen);

    current_settings.clock_automatic = false;
    current_settings.clock_24h = true;
    mocked_localtime.tm_hour = 23;
    mocked_localtime.tm_min = 7;
    lv_obj_t * set_row = row_with_label(built_clock_screen, "Set Time");
    assert(set_row);
    lv_obj_send_event(set_row, LV_EVENT_CLICKED, NULL);
    assert(last_pushed_screen == built_set_time_screen);
    lv_obj_t * rollers = find_type(built_set_time_screen, &lv_roller_class, 0);
    assert(rollers && lv_roller_get_option_count(rollers) == 24);
    assert(lv_roller_get_selected(rollers) == 23);
    lv_obj_t * ampm = find_type(built_set_time_screen, &lv_roller_class, 2);
    assert(ampm && lv_obj_has_flag(ampm, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t * minute = find_type(built_set_time_screen, &lv_roller_class, 1);
    assert(lv_roller_get_selected(minute) == 7);

    lv_roller_set_selected(rollers, 23, LV_ANIM_OFF);
    lv_roller_set_selected(minute, 7, LV_ANIM_OFF);
    lv_obj_t * save_label = find_label(built_set_time_screen, "Save");
    assert(save_label);
    lv_obj_send_event(lv_obj_get_parent(save_label), LV_EVENT_CLICKED, NULL);
    assert(app_clock_set_time_count == 1 && mocked_set_hour == 23 && mocked_set_minute == 7);
    assert(!current_settings.clock_automatic && settings_save_count == 1);
    assert(clock_persistence_count == 1 && refresh_clock_count == 1);
    reset_effects();
    for (int hour=0;hour<24;hour++) {

        mocked_localtime.tm_hour=hour; mocked_localtime.tm_min=(hour%2)?59:0;
        lv_obj_send_event(set_row,LV_EVENT_CLICKED,NULL);
        assert(lv_roller_get_option_count(rollers)==24 && lv_roller_get_selected(rollers)==(uint32_t)hour);
        assert(lv_roller_get_selected(minute)==(uint32_t)mocked_localtime.tm_min);
        lv_obj_send_event(lv_obj_get_parent(save_label),LV_EVENT_CLICKED,NULL);
        assert(mocked_set_hour==hour && mocked_set_minute==mocked_localtime.tm_min);
    }
    assert(settings_save_count==24 && clock_persistence_count==24 && refresh_clock_count==24);
    assert(effect_trace_len==24u*strlen("set-time;persistence;save;refresh;nav-pop;"));
    assert(effect_trace_len==24u*42u && strncmp(effect_trace,"set-time;persistence;save;refresh;nav-pop;",42)==0);

    reset_effects();
    current_settings.clock_24h = false;
    mocked_localtime.tm_hour = 0;
    mocked_localtime.tm_min = 42;
    lv_obj_send_event(set_row, LV_EVENT_CLICKED, NULL);
    assert(lv_roller_get_option_count(rollers) == 12);
    assert(lv_roller_get_selected(rollers) == 11); /* midnight is 12 AM */
    assert(!lv_obj_has_flag(ampm, LV_OBJ_FLAG_HIDDEN));
    assert(lv_roller_get_selected(ampm) == 0);
    lv_obj_send_event(save_label ? lv_obj_get_parent(save_label) : NULL,
                      LV_EVENT_CLICKED, NULL);
    assert(mocked_set_hour == 0 && mocked_set_minute == 42);
    assert(settings_save_count == 1 && clock_persistence_count == 1 && refresh_clock_count == 1);

    mocked_localtime.tm_hour = 22;
    mocked_localtime.tm_min = 59;
    lv_obj_send_event(set_row, LV_EVENT_CLICKED, NULL);
    assert(lv_roller_get_selected(rollers) == 9 && lv_roller_get_selected(ampm) == 1);
    lv_obj_send_event(lv_obj_get_parent(save_label), LV_EVENT_CLICKED, NULL);
    assert(mocked_set_hour == 22 && mocked_set_minute == 59);
    assert(settings_save_count == 2 && clock_persistence_count == 2 && refresh_clock_count == 2);
    reset_effects();
    for (int ampm_value=0;ampm_value<2;ampm_value++) {

        for (int hour_index=0;hour_index<12;hour_index++) {

            mocked_localtime.tm_hour=(hour_index+1)%12 + (ampm_value ? 12 : 0);
            mocked_localtime.tm_min=59;
            lv_obj_send_event(set_row,LV_EVENT_CLICKED,NULL);
            assert(lv_roller_get_option_count(rollers)==12);
            assert(lv_roller_get_selected(rollers)==(uint32_t)hour_index);
            assert(lv_roller_get_selected(ampm)==(uint32_t)ampm_value);
            assert(lv_roller_get_selected(minute)==59);
            lv_obj_send_event(lv_obj_get_parent(save_label),LV_EVENT_CLICKED,NULL);
            assert(mocked_set_hour==mocked_localtime.tm_hour && mocked_set_minute==59);
        }
    }
    assert(settings_save_count==24 && clock_persistence_count==24 && refresh_clock_count==24);
    assert(effect_trace_len==24u*42u && strncmp(effect_trace,"set-time;persistence;save;refresh;nav-pop;",42)==0);

    current_settings.clock_automatic = true;
    reset_effects();
    lv_obj_send_event(set_row, LV_EVENT_CLICKED, NULL);
    assert(info_toast_count == 1 && last_pushed_screen == built_set_time_screen);
    assert(app_clock_set_time_count == 0 && settings_save_count == 0);

    current_settings.clock_automatic = false;
    lv_obj_t * automatic_row = row_with_label(built_clock_screen, "Automatic");
    assert(automatic_row);
    lv_obj_t * automatic_switch = find_type(automatic_row, &lv_switch_class, 0);
    assert(automatic_switch);
    lv_obj_add_state(automatic_switch, LV_STATE_CHECKED);
    lv_obj_send_event(automatic_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.clock_automatic && app_clock_automatic);
    assert(app_clock_automatic_count == 1 && settings_save_count == 1);
    assert(clock_persistence_count == 1 && refresh_clock_count == 1);
    assert(strcmp(effect_trace,"automatic:on;persistence;save;refresh;")==0);
    lv_obj_clear_state(automatic_switch,LV_STATE_CHECKED);
    lv_obj_send_event(automatic_switch,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!current_settings.clock_automatic && !app_clock_automatic);
    assert(app_clock_automatic_count==2 && settings_save_count==2 && clock_persistence_count==2 && refresh_clock_count==2);
    assert(strcmp(effect_trace,"automatic:on;persistence;save;refresh;automatic:off;persistence;save;refresh;")==0);
    puts("clock: build-order, automatic on/off, all 24h and 12h+AM/PM roller choices passed");
}

static void test_24h_switch_and_timezone_route(void) {
    lv_obj_t * clock_switch = find_type(row_with_label(built_clock_screen, "24-Hour Clock"),
                                        &lv_switch_class, 0);
    assert(clock_switch);
    reset_effects();
    lv_obj_send_event(clock_switch, LV_EVENT_CLICKED, NULL);
    assert(settings_save_async_count == 0 && refresh_clock_count == 0);
    lv_obj_add_state(clock_switch, LV_STATE_CHECKED);
    lv_obj_send_event(clock_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.clock_24h && settings_save_async_count == 1);
    assert(clock_persistence_count == 1 && refresh_clock_count == 1);
    assert(current_settings.clock_manual_epoch == 123456 &&
           current_settings.clock_system_reference == 654321);
    assert(strcmp(effect_trace,"persistence;save-async;refresh;")==0);
    lv_obj_t * set_time_row=row_with_label(built_clock_screen,"Set Time");
    assert(set_time_row);
    lv_obj_send_event(set_time_row,LV_EVENT_CLICKED,NULL);
    lv_obj_t * hour_roller=find_type(built_set_time_screen,&lv_roller_class,0);
    lv_obj_t * ampm_roller=find_type(built_set_time_screen,&lv_roller_class,2);
    assert(lv_roller_get_option_count(hour_roller)==24 && lv_obj_has_flag(ampm_roller,LV_OBJ_FLAG_HIDDEN));
    lv_obj_clear_state(clock_switch,LV_STATE_CHECKED);
    lv_obj_send_event(clock_switch,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!current_settings.clock_24h && settings_save_async_count==2);
    assert(clock_persistence_count==2 && refresh_clock_count==2);
    assert(strcmp(effect_trace,"persistence;save-async;refresh;persistence;save-async;refresh;")==0);
    lv_obj_send_event(set_time_row,LV_EVENT_CLICKED,NULL);
    assert(lv_roller_get_option_count(hour_roller)==12 && !lv_obj_has_flag(ampm_roller,LV_OBJ_FLAG_HIDDEN));

    snprintf(current_settings.timezone, sizeof(current_settings.timezone), "America/New_York");
    nav_depth = 0;
    gui_setup_open_timezone();
    assert(last_pushed_screen == built_region_screen && nav_depth == 1);
    nav_depth = 1;
    lv_obj_t * clock_entry = lv_obj_create(NULL);
    lv_obj_add_event_cb(clock_entry, dispatch_clock_row_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(clock_entry, LV_EVENT_CLICKED, NULL);
    assert(last_pushed_screen == built_clock_screen && nav_depth == 2);
    lv_obj_t * zone_row = row_with_label(built_clock_screen, "Time Zone");
    assert(zone_row);
    lv_obj_send_event(zone_row, LV_EVENT_CLICKED, NULL);
    assert(last_pushed_screen == built_region_screen && nav_depth == 3);

    lv_obj_t * america = row_with_label(built_region_screen, "America");
    assert(america);
    lv_obj_send_event(america, LV_EVENT_CLICKED, NULL);
    assert(built_city_screen && nav_depth == 4);
    assert(compact_label_count > 0 && compact_click);
    int new_york_index = -1;
    int belize_index = -1;
    for (int i = 0; i < compact_label_count; i++) {
        if (strstr(compact_labels[i], "New York (UTC-04:00) (current)")) new_york_index = i;
        if (strstr(compact_labels[i], "Belize")) belize_index = i;
        assert(strstr(compact_labels[i], "London") == NULL);
    }
    assert(new_york_index >= 0 && belize_index >= 0);

    reset_effects();
    timezone_apply_succeeds = false;
    compact_click(belize_index);
    assert(timezone_apply_count == 1 && strcmp(last_applied_timezone, "America/Belize") == 0);
    assert(error_toast_count == 1 && settings_save_count == 0 && refresh_clock_count == 0);
    assert(nav_depth == 4 && strcmp(current_settings.timezone, "America/New_York") == 0);

    timezone_apply_succeeds = true;
    reset_effects();
    compact_click(belize_index);
    assert(strcmp(current_settings.timezone, "America/Belize") == 0);
    assert(settings_save_count == 1 && refresh_clock_count == 1 && timezone_apply_count == 1);
    assert(nav_depth == 2);
    assert(strstr(lv_label_get_text(find_label(built_clock_screen, "Belize (UTC-06:00)")),
                  "Belize") != NULL);

    /* Reopening another region replaces the old city screen and its label
     * generation before building the next region's visible rows. */
    lv_obj_send_event(zone_row, LV_EVENT_CLICKED, NULL);
    lv_obj_t * europe = row_with_label(built_region_screen, "Europe");
    assert(europe);
    lv_obj_send_event(europe, LV_EVENT_CLICKED, NULL);
    bool saw_london = false;
    for (int i = 0; i < compact_label_count; i++) {
        if (strstr(compact_labels[i], "London")) saw_london = true;
        assert(strstr(compact_labels[i], "Belize") == NULL);
    }
    assert(saw_london && built_city_screen && nav_depth == 4);
    int london_index=-1;
    for(int i=0;i<compact_label_count;i++) if(strstr(compact_labels[i],"London")) london_index=i;
    assert(london_index>=0);
    reset_effects(); timezone_apply_succeeds=false;
    compact_click(london_index);
    assert(timezone_apply_count==1 && strcmp(last_applied_timezone,"Europe/London")==0);
    assert(strcmp(current_settings.timezone,"America/Belize")==0 && settings_save_count==0 && refresh_clock_count==0);
    assert(strcmp(effect_trace,"timezone-apply;")==0);
    reset_effects(); timezone_apply_succeeds=true;
    compact_click(london_index);
    assert(strcmp(current_settings.timezone,"Europe/London")==0);
    assert(settings_save_count==1 && refresh_clock_count==1 && timezone_apply_count==1);
    assert(strcmp(effect_trace,"timezone-apply;save;refresh;nav-pop;")==0);
    assert(nav_depth==2 && clock_timezone_value_label);
    assert(strcmp(lv_label_get_text(clock_timezone_value_label),"London (UTC+01:00)")==0);

    /* Every city exposed by the production region/city picker is fed through
     * the real callback; apply/save/refresh are capture stubs. */
    bool timezone_seen[TIMEZONE_TABLE_COUNT]={false};
    int timezone_visited=0;
    timezone_apply_succeeds=true;
    for(size_t region_index=0;region_index<TIMEZONE_REGION_COUNT;region_index++) {
        build_count=0; delete_count=0;
        gui_setup_open_timezone();
        lv_obj_t *region_row=row_with_label(built_region_screen,TIMEZONE_REGIONS[region_index]);
        assert(region_row);
        lv_obj_send_event(region_row,LV_EVENT_CLICKED,NULL);
        assert(built_city_screen && timezone_city_count>0 && compact_click);
        for(int city=0;city<timezone_city_count;city++) {
            int table_index=timezone_city_indices[city];
            assert(table_index>=0 && table_index<TIMEZONE_TABLE_COUNT && !timezone_seen[table_index]);
            timezone_seen[table_index]=true; timezone_visited++;
            nav_depth=4; reset_effects();
            compact_click(city);
            assert(strcmp(last_applied_timezone,TIMEZONE_TABLE[table_index].iana_id)==0);
            assert(strcmp(current_settings.timezone,TIMEZONE_TABLE[table_index].iana_id)==0);
            assert(timezone_apply_count==1 && settings_save_count==1 && refresh_clock_count==1);
            assert(strcmp(effect_trace,"timezone-apply;save;refresh;nav-pop;")==0);
            assert(nav_depth==2);
        }
    }
    assert(timezone_visited==TIMEZONE_TABLE_COUNT);
    for(int i=0;i<TIMEZONE_TABLE_COUNT;i++) assert(timezone_seen[i]);
    puts("timezone: all region/city choices, failure preservation, save/refresh ordering and stack collapse passed");
}

static void test_teardown_order(void) {
    delete_count = 0;
    time_spec_teardown();
    assert(delete_count == 4);
    assert(delete_order[0] == 1); /* Region */
    assert(delete_order[1] == 2); /* Clock */
    assert(delete_order[2] == 3); /* Set Time */
    assert(delete_order[3] == 4); /* lazy City */
    assert(!lv_obj_is_valid(built_region_screen));
    assert(!lv_obj_is_valid(built_clock_screen));
    assert(!lv_obj_is_valid(built_set_time_screen));
    assert(!lv_obj_is_valid(built_city_screen));
    puts("teardown: Region, Clock, Set Time, City order passed");
}

int main(void) {
    lv_init();
    display = lv_display_create(480, 800);
    assert(display);
    lv_display_set_theme(display, lv_theme_default_init(display, lv_color_white(),
                                                       lv_color_black(), true, &test_font));
    static uint8_t pixels[480 * 32 * 4];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_style_init(&style_theme_screen_bg);
    lv_style_init(&style_theme_card_bg);
    lv_style_init(&style_theme_text_primary);
    lv_style_init(&style_theme_text_muted);
    lv_style_init(&accent_style);

    test_build_order_and_clock();
    test_24h_switch_and_timezone_route();
    test_teardown_order();
    puts("PASS: GUI clock/timezone lifecycle characterization");
    return 0;
}
