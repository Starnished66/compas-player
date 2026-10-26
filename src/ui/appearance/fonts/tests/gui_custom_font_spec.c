#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "../../../../core/persistence/settings.h"
#include "../../../text/rendering/fallback_font.h"
#include "../../../shell/gui_navigation.h"
#include "../../../shell/gui_notifications.h"
#include "../../../player/screen/gui_lyrics.h"
#include "../../../shell/gui_shell.h"
#include "../../../settings/gui_settings.h"
#include "../../gui_theme.h"
#include "../../../text/i18n/i18n.h"
#include "../../../widgets/builders/screen_builders.h"
#include "../../../text/fonts/ui_fonts.h"

#define TRACE_CAPACITY 8192
static char trace_log[TRACE_CAPACITY];
static size_t trace_length;
static char discovered_names[MAX_CUSTOM_FONTS_DISCOVERED][64];
static int discovered_count;
static const char * active_font_name = "Default";
static bool apply_should_fail;
static lv_obj_t * pushed_screen;
static int active_invalidation_count;

lv_style_t style_theme_screen_bg;
lv_style_t style_theme_card_bg;
lv_style_t style_theme_text_primary;
lv_style_t style_theme_text_muted;
player_settings_t current_settings;

static void tracef(const char * format, ...);
static void flush_ready(lv_display_t * display, const lv_area_t * area,
                        uint8_t * pixels);
static void font_test_report_style_change(lv_style_t * style);
static void font_test_invalidate(lv_obj_t * object);

#define lv_obj_report_style_change font_test_report_style_change
#define lv_obj_invalidate font_test_invalidate

#if defined(CUSTOM_FONT_SPEC_FINAL)
#include "../gui_custom_font.c"
static void picker_test_build(void) { (void)gui_custom_font_build_screen(); }
static void picker_test_teardown(void) { gui_custom_font_teardown(); }
static void picker_test_row_cb(lv_event_t * e) { gui_custom_font_row_cb(e); }
#else
#include "custom_font_baseline_fixture.c"
static void picker_test_build(void) { custom_font_screen = build_custom_font_screen(); }
static void picker_test_teardown(void) {
    if (custom_font_screen) { lv_obj_delete(custom_font_screen); custom_font_screen = NULL; }
}
static void picker_test_row_cb(lv_event_t * e) { custom_font_row_cb(e); }
#endif

#undef lv_obj_report_style_change
#undef lv_obj_invalidate

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
void settings_save(const player_settings_t * settings) {
    assert(settings == &current_settings);
    tracef("save:%s\n", settings->custom_font);
}
int fallback_font_discover_custom(char out_names[][64], int max_count) {
    int count = discovered_count < max_count ? discovered_count : max_count;
    for (int i = 0; i < count; ++i)
        snprintf(out_names[i], 64, "%s", discovered_names[i]);
    tracef("discover:%d\n", count);
    return count;
}
const char * fallback_font_get_custom_name(void) { return active_font_name; }
bool fallback_font_apply_custom(const char * name) {
    tracef("apply:%s\n", name);
    if (apply_should_fail) return false;
    active_font_name = strcmp(name, "Default") == 0 ? "Default" : discovered_names[0];
    if (strcmp(name, "Default") != 0) {
        for (int i = 0; i < discovered_count; ++i)
            if (strcmp(name, discovered_names[i]) == 0) active_font_name = discovered_names[i];
    }
    return true;
}
void gui_navigation_invalidate_font_snapshots(void) { tracef("snapshots-invalidated\n"); }
void nav_reset_to_home(void) { tracef("nav-home\n"); }
void nav_push(lv_obj_t * screen) { pushed_screen = screen; tracef("nav-push\n"); }
void generic_back_cb(lv_event_t * e) { (void)e; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
void show_error_toast(const char * text) { tracef("error:%s\n", text); }
void screen_builders_refresh_all_font_geometry(void) { tracef("all-geometry\n"); }
void screen_builders_refresh_font_geometry(lv_obj_t * root) {
    assert(root != NULL);
    tracef("active-geometry\n");
}
void gui_settings_refresh_font_geometry(void) { tracef("settings-geometry\n"); }
void compact_list_refresh_all(void) { tracef("compact-refresh\n"); }
void gui_lyrics_refresh_layout(void) { tracef("lyrics-refresh\n"); }
void quick_drawer_mark_snapshot_dirty(void) { tracef("drawer-dirty\n"); }
static void font_test_report_style_change(lv_style_t * style) {
    assert(style == NULL);
    tracef("style-change\n");
}
static void font_test_invalidate(lv_obj_t * object) {
    assert(object != NULL);
    if (object == lv_screen_active()) active_invalidation_count++;
    tracef("invalidate\n");
}

lv_obj_t * build_screen_header(lv_obj_t * screen, const char * title,
                               lv_event_cb_t back_cb, const char * trailing_asset,
                               lv_event_cb_t trailing_cb) {
    (void)back_cb; (void)trailing_asset; (void)trailing_cb;
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, title);
    return label;
}
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * text, bool selected,
                               lv_event_cb_t callback, void * user_data) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 220, 32);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (selected) lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    return row;
}

static lv_display_t * make_display(void) {
    static uint8_t pixels[240 * 32 * 4];
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    return display;
}

static void flush_ready(lv_display_t * display, const lv_area_t * area,
                        uint8_t * pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static lv_obj_t * find_option(const char * expected) {
    assert(custom_font_list);
    for (uint32_t i = 0; i < lv_obj_get_child_count(custom_font_list); ++i) {
        lv_obj_t * row = lv_obj_get_child(custom_font_list, i);
        if (!lv_obj_check_type(row, &lv_obj_class)) continue;
        if (lv_obj_get_child_count(row) == 0) continue;
        lv_obj_t * label = lv_obj_get_child(row, 0);
        if (lv_obj_check_type(label, &lv_label_class) &&
            strcmp(lv_label_get_text(label), expected) == 0) return row;
    }
    return NULL;
}

static void click_row(lv_obj_t * row) {
    assert(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t * system_mask(void) {
    lv_obj_t * sys = lv_layer_sys();
    assert(sys);
    return lv_obj_get_child_count(sys) ? lv_obj_get_child(sys, 0) : NULL;
}

static lv_timer_t * timer_for_mask(lv_obj_t * mask) {
    for (lv_timer_t * timer = lv_timer_get_next(NULL); timer;
         timer = lv_timer_get_next(timer))
        if (lv_timer_get_user_data(timer) == mask) return timer;
    return NULL;
}

static void fire_timer(lv_timer_t * timer) {
    assert(timer);
    lv_obj_t * mask = system_mask();
    assert(mask && timer_for_mask(mask) == timer);
    lv_tick_inc(34);
    lv_timer_handler();
    assert(system_mask() == mask && timer_for_mask(mask) == timer);
    lv_tick_inc(1);
    lv_timer_handler();
    assert(system_mask() == NULL);
}

static void reset_trace(void) { trace_length = 0; trace_log[0] = '\0'; }

static void test_picker(void) {
    strcpy(discovered_names[0], "Aqua");
    strcpy(discovered_names[1], "Serif");
    discovered_count = 0;
    discovered_custom_font_count = 0;
    active_font_name = "Default";
    current_settings.custom_font[0] = '\0';
    picker_test_build();
    assert(gui_settings_get_custom_font_screen());

    /* The appearance row callback is a CLICKED-only navigation entry point. */
    lv_obj_t * caller = lv_obj_create(NULL);
    lv_obj_add_event_cb(caller, picker_test_row_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(caller, LV_EVENT_VALUE_CHANGED, NULL);
    assert(pushed_screen == NULL);
    lv_obj_send_event(caller, LV_EVENT_CLICKED, NULL);
    assert(pushed_screen == gui_settings_get_custom_font_screen());
    assert(discovered_count == 0);
    assert(lv_obj_get_child_count(custom_font_list) == 2);
    lv_obj_t * empty_note = lv_obj_get_child(custom_font_list, 1);
    assert(empty_note && strcmp(lv_label_get_text(empty_note),
                                 "No .ttf fonts found in /Fonts") == 0);
    discovered_count = 2;
    click_row(caller);
    assert(lv_obj_get_child_count(custom_font_list) == 3);
    lv_obj_t * default_row = find_option("Default (Built-in)");
    lv_obj_t * aqua_row = find_option("Aqua");
    lv_obj_t * serif_row = find_option("Serif");
    assert(default_row && aqua_row && serif_row);
    assert(lv_obj_has_state(default_row, LV_STATE_CHECKED));
    assert(!lv_obj_has_state(aqua_row, LV_STATE_CHECKED));

    /* The active-font selection is a true no-op: no mask, apply, or effects. */
    active_font_name = "Aqua";
    snprintf(current_settings.custom_font, sizeof(current_settings.custom_font), "Aqua");
    click_row(caller);
    default_row = find_option("Default (Built-in)");
    aqua_row = find_option("Aqua");
    serif_row = find_option("Serif");
    assert(default_row && aqua_row && serif_row);
    assert(!lv_obj_has_state(default_row, LV_STATE_CHECKED));
    assert(lv_obj_has_state(aqua_row, LV_STATE_CHECKED));
    reset_trace();
    click_row(aqua_row);
    assert(system_mask() == NULL);
    assert(trace_length == 0);

    /* A different selection applies after a one-shot mask and preserves effect order. */
    click_row(serif_row);
    lv_obj_t * mask = system_mask();
    assert(mask && lv_obj_has_flag(mask, LV_OBJ_FLAG_CLICKABLE));
    lv_timer_t * apply_timer = timer_for_mask(mask);
    assert(apply_timer);
    fire_timer(apply_timer);
    assert(system_mask() == NULL);
    assert(strcmp(current_settings.custom_font, "Serif") == 0);
    assert(strcmp(active_font_name, "Serif") == 0);
    const char * expected_success =
        "invalidate\n"
        "apply:Serif\n"
        "snapshots-invalidated\n"
        "save:Serif\n"
        "style-change\n"
        "all-geometry\n"
        "settings-geometry\n"
        "compact-refresh\n"
        "lyrics-refresh\n"
        "drawer-dirty\n"
        "nav-home\n"
        "active-geometry\n"
        "invalidate\n"
        "invalidate\n"
        "invalidate\n";
    assert(strcmp(trace_log, expected_success) == 0);
    assert(active_invalidation_count == 1);
    printf("success-effects:\n%s", trace_log);

    /* A queued row whose discovery snapshot is stale is rejected and unmasks. */
    active_font_name = "Default";
    click_row(caller);
    aqua_row = find_option("Aqua");
    assert(aqua_row);
    click_row(aqua_row);
    mask = system_mask();
    assert(mask && timer_for_mask(mask));
    discovered_count = 0;
    discovered_custom_font_count = 0;
    reset_trace();
    fire_timer(timer_for_mask(mask));
    assert(system_mask() == NULL);
    assert(strcmp(trace_log, "error:Font selection is no longer available\n") == 0);
    printf("stale-effects:\n%s", trace_log);

    /* The fallback-loader failure also removes the mask without post-apply effects. */
    discovered_count = 2;
    active_font_name = "Default";
    click_row(caller);
    serif_row = find_option("Serif");
    assert(serif_row);
    apply_should_fail = true;
    click_row(serif_row);
    mask = system_mask();
    assert(mask && timer_for_mask(mask));
    reset_trace();
    fire_timer(timer_for_mask(mask));
    assert(system_mask() == NULL);
    assert(strcmp(trace_log,
                  "apply:Serif\nerror:Failed to load font. Check format & memory.\n") == 0);
    assert(strcmp(current_settings.custom_font, "Serif") == 0);
    assert(strcmp(active_font_name, "Default") == 0);
    printf("failure-effects:\n%s", trace_log);
    apply_should_fail = false;

    lv_obj_delete(caller);
    picker_test_teardown();
    assert(gui_settings_get_custom_font_screen() == NULL);
    assert(system_mask() == NULL);
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    (void)make_display();
    lv_style_init(&style_theme_screen_bg);
    lv_style_init(&style_theme_card_bg);
    lv_style_init(&style_theme_text_primary);
    lv_style_init(&style_theme_text_muted);
    test_picker();
    puts("GUI custom font picker characterization passed");
    return 0;
}
