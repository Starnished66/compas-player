#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "assets.h"
#include "gui_navigation.h"
#include "gui_settings.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"
#include "settings.h"

player_settings_t current_settings;
const uint32_t accent_palette[ACCENT_PALETTE_COUNT] = { 0x2196F3 };
lv_style_t style_theme_screen_bg;
lv_style_t style_theme_card_bg;
lv_style_t style_theme_text_muted;
lv_style_t style_theme_text_primary;
void gui_equalizer_accent_changed(void);

#ifndef ACCENT_SPEC_FINAL
#include "accent_baseline_fixture.c"
#else
#include "gui_accent.h"
#include "accent_final_fixture.c"
static void accent_test_build_screens(void) { gui_accent_build_screens(); }
static void accent_test_teardown(void) { gui_accent_teardown(); }
static void accent_test_row_cb(lv_event_t * event) { gui_accent_row_cb(event); }
#endif

static char effects[8192];
static size_t effects_size;
static int eq_sync_count;
static int finalize_count;
static int nav_push_count;
static int settings_save_count;
static int settings_save_async_count;
static int accent_apply_count;
static int dynamic_set_count;
static int titlebar_set_count;
static int invalidation_count;
static int dead_zone_count;
static bool cover_available;
static uint32_t cover_color = 0x00AA55;
static uint32_t active_accent_color = 0x336699;
static lv_obj_t * last_pushed;
static lv_obj_t * accent_header_action;
static lv_obj_t * header_options_screen;

static void effect(const char * format, ...);
#include <stdarg.h>
static void effect(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(effects + effects_size, sizeof(effects) - effects_size, format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(effects) - effects_size);
    effects_size += (size_t)n;
}

const char * i18n_tr(const char * text) { return text; }
const lv_font_t * gui_theme_font(gui_font_role_t role) { (void)role; return &lv_font_montserrat_16; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
lv_style_t * gui_theme_accent_style(void) { return &style_theme_card_bg; }
const char * asset_path(const char * relative) { return relative; }
void generic_back_cb(lv_event_t * event) { (void)event; }
void settings_save(const player_settings_t * settings) { (void)settings; settings_save_count++; effect("save\n"); }
void settings_save_async(const player_settings_t * settings) {
    (void)settings; settings_save_async_count++; effect("save-async\n");
}
void gui_equalizer_accent_changed(void) { eq_sync_count++; effect("eq-accent-sync\n"); }
void gui_navigation_invalidate_theme_snapshots(void) { invalidation_count++; effect("snapshots-invalidated\n"); }

/* Theme/settings adapters are deterministic test doubles; this suite does
 * not write settings files or exercise the real theme backend. */
bool gui_theme_cover_accent(uint32_t * out_rgb) {
    if (cover_available && out_rgb) *out_rgb = cover_color;
    return cover_available;
}
uint32_t gui_theme_accent_rgb(void) {
    return current_settings.accent_dynamic && cover_available ? cover_color : active_accent_color;
}
lv_color_t accent_lv_color(void) { return lv_color_hex(gui_theme_accent_rgb()); }

void gui_theme_apply_accent(uint32_t rgb) {
    accent_apply_count++;
    effect("accent-apply:%06X\n", (unsigned)(rgb & 0xFFFFFF));
    active_accent_color = rgb & 0xFFFFFF;
    current_settings.accent_color = active_accent_color;
    current_settings.accent_dynamic = false;
    settings_save(&current_settings);
    gui_settings_accent_changed();
}
void gui_theme_set_accent_dynamic(bool on) {
    dynamic_set_count++;
    effect("dynamic:%d\n", on ? 1 : 0);
    current_settings.accent_dynamic = on;
    settings_save(&current_settings);
    gui_settings_accent_changed();
}
void gui_theme_set_titlebar_tint(bool on) {
    titlebar_set_count++;
    effect("titlebar-tint:%d\n", on ? 1 : 0);
    current_settings.titlebar_icon_tint = on;
    settings_save_async(&current_settings);
    gui_navigation_invalidate_theme_snapshots();
}

lv_obj_t * build_screen_header(lv_obj_t * screen, const char * title, lv_event_cb_t back_cb,
                               const char * trailing_asset, lv_event_cb_t trailing_cb) {
    (void)back_cb;
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, title);
    effect("header:%s\n", title);
    if (trailing_asset && trailing_cb) {
        lv_obj_t * button = lv_btn_create(screen);
        lv_obj_add_event_cb(button, trailing_cb, LV_EVENT_CLICKED, NULL);
        if (strcmp(title, "Accent Color") == 0) {
            accent_header_action = button;
            header_options_screen = screen;
        }
    }
    return label;
}
void finalize_screen_navigation(lv_obj_t * screen) {
    (void)screen;
    finalize_count++;
    effect("finalize\n");
}
void nav_push(lv_obj_t * screen) {
    nav_push_count++;
    last_pushed = screen;
    effect("nav-push:%s\n", screen == accent_color_screen ? "picker" :
           screen == accent_options_screen ? "options" : screen ? "other" : "null");
}
void register_swipe_dead_zone(lv_obj_t * object) {
    assert(object);
    dead_zone_count++;
    effect("dead-zone-register\n");
}
void unregister_swipe_dead_zone(lv_obj_t * object) { (void)object; }

static void find_switches(lv_obj_t * parent, lv_obj_t ** out, int * count) {
    uint32_t children = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t * child = lv_obj_get_child(parent, (int32_t)i);
        if (lv_obj_check_type(child, &lv_switch_class)) {
            assert(*count < 8);
            out[(*count)++] = child;
        }
        find_switches(child, out, count);
    }
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(480, 800);
    assert(display);
    static uint8_t display_buffer[480 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    current_settings.accent_color = 0x336699;

    gui_settings_accent_changed();
    assert(eq_sync_count == 1 && accent_preview_swatch == NULL);
    accent_test_build_screens();
    assert(gui_settings_get_accent_screen() == accent_color_screen);
    assert(accent_options_screen && accent_color_screen);
    assert(header_options_screen == accent_color_screen);
    assert(finalize_count == 2 && dead_zone_count == 1);
    assert(strstr(effects, "header:Accent Options\nfinalize\nheader:Accent Color\n") != NULL);
    assert(strcmp(lv_label_get_text(accent_source_label), "Custom color") == 0);

    lv_obj_t * row = lv_btn_create(lv_layer_top());
    lv_obj_add_event_cb(row, accent_test_row_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(row, LV_EVENT_VALUE_CHANGED, NULL);
    assert(nav_push_count == 0);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == 1 && last_pushed == accent_color_screen);
    lv_obj_send_event(accent_header_action, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == 2 && last_pushed == accent_options_screen);
    lv_obj_delete(row);

    lv_obj_t * switches[8] = {0};
    int switch_count = 0;
    find_switches(accent_options_screen, switches, &switch_count);
    assert(switch_count == 3 && switches[0] == accent_dynamic_switch);
    int dynamic_before = dynamic_set_count;
    lv_obj_send_event(accent_dynamic_switch, LV_EVENT_CLICKED, NULL);
    assert(dynamic_set_count == dynamic_before);
    cover_available = true;
    lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
    lv_obj_send_event(accent_dynamic_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.accent_dynamic && dynamic_set_count == dynamic_before + 1);
    assert(strcmp(lv_label_get_text(accent_source_label), "From album art") == 0);
    cover_available = false;
    gui_settings_accent_changed();
    assert(strcmp(lv_label_get_text(accent_source_label), "From album art (no cover, using custom)") == 0);

    int async_before = settings_save_async_count;
    lv_obj_add_state(switches[1], LV_STATE_CHECKED);
    lv_obj_send_event(switches[1], LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.startup_splash_tint && settings_save_async_count == async_before + 1);
    lv_obj_add_state(switches[2], LV_STATE_CHECKED);
    lv_obj_send_event(switches[2], LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.titlebar_icon_tint && titlebar_set_count == 1);

    int applies_before = accent_apply_count;
    lv_obj_send_event(accent_presets[0], LV_EVENT_VALUE_CHANGED, NULL);
    assert(accent_apply_count == applies_before);
    lv_obj_send_event(accent_presets[0], LV_EVENT_CLICKED, NULL);
    assert(accent_apply_count == applies_before + 1 && current_settings.accent_color == accent_palette[0]);
    assert(!current_settings.accent_dynamic);

    /* No active indev is registered: this covers preview/release dispatch,
     * while the synthetic HSV below targets reentrant commit preservation. */
    lv_obj_send_event(accent_sv_area, LV_EVENT_PRESSED, NULL);
    assert(accent_apply_count == applies_before + 1);
    assert(strcmp(lv_label_get_text(accent_source_label), "Custom color") == 0);
    assert(accent_pick.s == 0 && accent_pick.v == 100);
    effect("preview:%u,%u,%u\n", accent_pick.h, accent_pick.s, accent_pick.v);
    accent_pick = (lv_color_hsv_t){ 123, 70, 80 };
    int eq_before_release = eq_sync_count;
    lv_obj_send_event(accent_sv_area, LV_EVENT_RELEASED, NULL);
    assert(accent_apply_count == applies_before + 2);
    assert(eq_sync_count == eq_before_release + 1);
    assert(accent_pick.h == 123 && accent_pick.s == 70 && accent_pick.v == 80);
    assert(!accent_committing);

    /* Preserve user settings across UI teardown/rebuild; no settings file is touched. */
    lv_obj_clear_state(accent_dynamic_switch, LV_STATE_CHECKED);
    lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
    lv_obj_send_event(accent_dynamic_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.accent_dynamic && current_settings.startup_splash_tint &&
           current_settings.titlebar_icon_tint);
    int eq_before_teardown = eq_sync_count;
    accent_test_teardown();
    assert(gui_settings_get_accent_screen() == NULL && accent_options_screen == NULL);
    gui_settings_accent_changed();
    assert(eq_sync_count == eq_before_teardown + 1);
    int pushes_before_guard = nav_push_count;
    lv_obj_t * guard = lv_btn_create(lv_layer_top());
    lv_obj_add_event_cb(guard, accent_options_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(guard, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == pushes_before_guard);
    lv_obj_delete(guard);

    int prior_finalizes = finalize_count;
    accent_test_build_screens();
    assert(finalize_count == prior_finalizes + 2);
    assert(gui_settings_get_accent_screen() == accent_color_screen);
    assert(lv_obj_has_state(accent_dynamic_switch, LV_STATE_CHECKED));
    assert(current_settings.startup_splash_tint && current_settings.titlebar_icon_tint);
    accent_test_teardown();
    assert(gui_settings_get_accent_screen() == NULL);

    printf("accent-effects:\n%sstate:builds=%d;nav=%d;eq-sync=%d;apply=%d;dynamic=%d;titlebar=%d;save=%d/%d;dead-zone=%d\n",
           effects, finalize_count, nav_push_count, eq_sync_count, accent_apply_count,
           dynamic_set_count, titlebar_set_count, settings_save_count,
           settings_save_async_count, dead_zone_count);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
