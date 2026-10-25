#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "backlight.h"
#include "gui_navigation.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"
#include "settings.h"
#include "ui_fonts.h"

#include "display_timing_settings_steps_fixture.c"

#define TRACE_CAPACITY 8192
static char trace_log[TRACE_CAPACITY];
static size_t trace_length;
static int save_count;
static int backlight_count;
static bool last_backlight_dimmed;
static lv_obj_t * pushed_screen;

lv_style_t style_theme_screen_bg;
lv_style_t style_theme_text_primary;
lv_style_t style_theme_text_muted;
player_settings_t current_settings;

static void tracef(const char * format, ...);
static void flush_ready(lv_display_t * display, const lv_area_t * area,
                        uint8_t * pixels);

#include "screen_builders_nearest_step_fixture.c"

#if defined(DISPLAY_TIMING_SPEC_FINAL)
#include "gui_display_timing_settings.c"
static void timing_test_build_screens(void) {
    gui_display_timing_settings_build_screens();
}
static void timing_test_teardown(void) {
    gui_display_timing_settings_teardown();
}
static void timing_test_timeout_row_cb(lv_event_t * e) {
    gui_display_timing_settings_timeout_row_cb(e);
}
static void timing_test_dimming_row_cb(lv_event_t * e) {
    gui_display_timing_settings_dimming_row_cb(e);
}
#else
#include "display_timing_baseline_fixture.c"
static void timing_test_build_screens(void) {
    display_timing_test_build_screens();
}
static void timing_test_teardown(void) {
    display_timing_test_teardown();
}
static void timing_test_timeout_row_cb(lv_event_t * e) {
    screen_timeout_row_cb(e);
}
static void timing_test_dimming_row_cb(lv_event_t * e) {
    screen_dimming_row_cb(e);
}
#endif

static void tracef(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(trace_log + trace_length, TRACE_CAPACITY - trace_length,
                            format, args);
    va_end(args);
    assert(written >= 0 && (size_t)written < TRACE_CAPACITY - trace_length);
    trace_length += (size_t)written;
}

const char * i18n_tr(const char * text) { return text; }
const lv_font_t * gui_theme_font(gui_font_role_t role) {
    (void)role;
    return LV_FONT_DEFAULT;
}
lv_style_t * gui_theme_accent_style(void) { return &style_theme_text_primary; }
void settings_save(const player_settings_t * settings) {
    assert(settings == &current_settings);
    save_count++;
    tracef("save:timeout=%d;timeout_on=%d;dim=%d;dim_on=%d\n",
           settings->screen_timeout_seconds, settings->screen_timeout_enabled,
           settings->screen_dim_delay_seconds, settings->screen_dimming_enabled);
}
void backlight_set_dimmed(bool dimmed) {
    backlight_count++;
    last_backlight_dimmed = dimmed;
    tracef("backlight:%s\n", dimmed ? "dimmed" : "lit");
}
void nav_push(lv_obj_t * screen) {
    pushed_screen = screen;
    tracef("nav:%s\n", screen == screen_timeout_screen ? "timeout" :
                        screen == screen_dimming_screen ? "dimming" : "other");
}
void generic_back_cb(lv_event_t * e) { (void)e; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }

lv_obj_t * build_screen_header(lv_obj_t * screen, const char * title,
                               lv_event_cb_t back_cb, const char * trailing_asset,
                               lv_event_cb_t trailing_cb) {
    (void)back_cb; (void)trailing_asset; (void)trailing_cb;
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, title);
    return label;
}
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
lv_obj_t * build_setting_slider_card(lv_obj_t * parent, lv_obj_t * align_target,
                                     int32_t card_height, int32_t track_top_offset,
                                     int32_t min_range, int32_t max_range,
                                     int32_t initial_val, lv_event_cb_t slider_cb,
                                     lv_obj_t ** out_slider,
                                     lv_obj_t ** out_value_label) {
    (void)align_target; (void)card_height; (void)track_top_offset;
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_t * slider = lv_slider_create(card);
    lv_slider_set_range(slider, min_range, max_range);
    lv_slider_set_value(slider, initial_val, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * value = lv_label_create(card);
    *out_slider = slider;
    *out_value_label = value;
    return card;
}

static void flush_ready(lv_display_t * display, const lv_area_t * area,
                        uint8_t * pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static lv_display_t * make_display(void) {
    static uint8_t pixels[240 * 32 * 4];
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    return display;
}

static void reset_trace(void) {
    trace_length = 0;
    trace_log[0] = '\0';
}

static void test_screen_timeout(void) {
    current_settings.screen_timeout_enabled = false;
    current_settings.screen_timeout_seconds = 58;
    current_settings.screen_dimming_enabled = false;
    current_settings.screen_dim_delay_seconds = 60;
    timing_test_build_screens();

    assert(screen_timeout_screen && screen_dimming_screen);
    assert(lv_slider_get_value(screen_timeout_slider) == 2); /* 58s is nearest to 60s */
    assert(strcmp(lv_label_get_text(screen_timeout_value_label), "58s") == 0);
    assert(lv_slider_get_value(screen_dimming_slider) == 4); /* 60s dim preset */
    assert(strcmp(lv_label_get_text(screen_dimming_value_label), "1m") == 0);
    assert(lv_obj_has_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t * callback_probe = lv_obj_create(NULL);
    lv_obj_add_event_cb(callback_probe, timing_test_timeout_row_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(callback_probe, LV_EVENT_VALUE_CHANGED, NULL);
    assert(pushed_screen == NULL);
    lv_obj_send_event(callback_probe, LV_EVENT_CLICKED, NULL);
    assert(pushed_screen == screen_timeout_screen);
    lv_obj_delete(callback_probe);

    pushed_screen = NULL;
    callback_probe = lv_obj_create(NULL);
    lv_obj_add_event_cb(callback_probe, timing_test_dimming_row_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(callback_probe, LV_EVENT_VALUE_CHANGED, NULL);
    assert(pushed_screen == NULL);
    lv_obj_send_event(callback_probe, LV_EVENT_CLICKED, NULL);
    assert(pushed_screen == screen_dimming_screen);
    lv_obj_delete(callback_probe);

    save_count = 0;
    lv_obj_add_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_send_event(screen_timeout_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.screen_timeout_enabled);
    assert(!lv_obj_has_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(save_count == 1);

    lv_slider_set_value(screen_timeout_slider, 5, LV_ANIM_OFF);
    lv_obj_send_event(screen_timeout_slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.screen_timeout_seconds == 600);
    assert(strcmp(lv_label_get_text(screen_timeout_value_label), "10m") == 0);
    assert(save_count == 1); /* VALUE_CHANGED is live-only */
    lv_obj_send_event(screen_timeout_slider, LV_EVENT_RELEASED, NULL);
    assert(save_count == 2);

    lv_obj_clear_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_send_event(screen_timeout_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!current_settings.screen_timeout_enabled);
    assert(lv_obj_has_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(save_count == 3);
    printf("timeout-effects:\n%s", trace_log);
}

static void test_dimming_clamp(void) {
    current_settings.screen_timeout_enabled = true;
    current_settings.screen_timeout_seconds = 60;
    current_settings.screen_dim_delay_seconds = 120;
    save_count = 0;
    lv_obj_send_event(screen_dimming_screen, LV_EVENT_SCREEN_LOADED, NULL);
    assert(lv_slider_get_min_value(screen_dimming_slider) == 0);
    assert(lv_slider_get_max_value(screen_dimming_slider) == 3);
    assert(current_settings.screen_dim_delay_seconds == 30);
    assert(lv_slider_get_value(screen_dimming_slider) == 3);
    assert(strcmp(lv_label_get_text(screen_dimming_value_label), "30s") == 0);
    assert(save_count == 1);

    reset_trace();
    current_settings.screen_timeout_seconds = 15;
    current_settings.screen_dim_delay_seconds = 30;
    lv_obj_send_event(screen_dimming_screen, LV_EVENT_SCREEN_LOADED, NULL);
    assert(lv_slider_get_max_value(screen_dimming_slider) == 1);
    assert(current_settings.screen_dim_delay_seconds == 10);
    assert(lv_slider_get_value(screen_dimming_slider) == 1);
    assert(strcmp(lv_label_get_text(screen_dimming_value_label), "10s") == 0);
    assert(save_count == 2);
    printf("clamp-effects:\n%s", trace_log);
}

static void test_dimming_switch_and_slider(void) {
    current_settings.screen_timeout_enabled = false;
    current_settings.screen_dimming_enabled = true;
    save_count = 0;
    lv_obj_add_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_send_event(screen_dimming_screen, LV_EVENT_SCREEN_LOADED, NULL);
    assert(lv_slider_get_max_value(screen_dimming_slider) == SCREEN_DIM_DELAY_STEP_COUNT - 1);
    assert(save_count == 0);
    backlight_count = 0;
    save_count = 0;
    reset_trace();
    lv_obj_clear_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_send_event(screen_dimming_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!current_settings.screen_dimming_enabled);
    assert(lv_obj_has_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(backlight_count == 1 && !last_backlight_dimmed);
    assert(save_count == 1);
    assert(strcmp(trace_log,
                  "backlight:lit\n"
                  "save:timeout=15;timeout_on=0;dim=10;dim_on=0\n") == 0);

    lv_obj_add_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_send_event(screen_dimming_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.screen_dimming_enabled);
    assert(!lv_obj_has_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(backlight_count == 1); /* enabling does not force an immediate dim */
    assert(save_count == 2);

    lv_slider_set_value(screen_dimming_slider, 2, LV_ANIM_OFF);
    lv_obj_send_event(screen_dimming_slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.screen_dim_delay_seconds == SCREEN_DIM_DELAY_STEPS[2]);
    assert(strcmp(lv_label_get_text(screen_dimming_value_label), "15s") == 0);
    assert(save_count == 2);
    lv_obj_send_event(screen_dimming_slider, LV_EVENT_RELEASED, NULL);
    assert(save_count == 3);
    printf("dimming-effects:\n%s", trace_log);
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    (void)make_display();
    lv_style_init(&style_theme_screen_bg);
    lv_style_init(&style_theme_text_primary);
    lv_style_init(&style_theme_text_muted);

    reset_trace();
    test_screen_timeout();
    test_dimming_clamp();
    test_dimming_switch_and_slider();
    timing_test_teardown();
    assert(screen_timeout_screen == NULL && screen_dimming_screen == NULL);
    puts("GUI display timing settings characterization passed");
    return 0;
}
