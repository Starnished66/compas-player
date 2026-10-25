/* Volume/Car Mode UI characterization against the frozen Settings slice or
 * the eventual production module. Audio/player/plugin effects are all mocked. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include "settings.h"
#include "audio.h"
#include "gui_player.h"
#include "gui_theme.h"
#include "gui_navigation.h"
#include "screen_builders.h"
#include "gui_shell.h"
#include "gui_settings.h"
#include "plugin_manager.h"
#include "i18n.h"

player_settings_t current_settings;
lv_style_t style_theme_screen_bg, style_theme_text_primary, style_theme_text_muted;
static lv_style_t accent_style;
static lv_display_t * display;
static lv_obj_t * built[4], * startup_screen, * max_screen, * car_screen;
static lv_obj_t * sliders[4], * last_screen, * last_slider;
static int build_count, slider_count, save_sync_count, save_async_count, nav_count;
static int audio_get_count, audio_set_count, cap_set_count;
static int player_set_count, remember_count, car_enabled_count, topbar_count;
static int plugin_find_count, plugin_get_count, plugin_set_count, expansion_refresh_count;
static bool last_car_mode_enabled;
static bool plugin_gain_enabled, last_plugin_set_value;
static int quick_index = 3;
static bool audio_volume_capped;
static float audio_volume = 0.8f;
static char effects[2048];

static void trace(const char * s) { strncat(effects, s, sizeof(effects) - strlen(effects) - 1); }
static bool test_font_glyph(const lv_font_t * font, lv_font_glyph_dsc_t * dsc, uint32_t letter, uint32_t next) {
    LV_UNUSED(letter); LV_UNUSED(next); memset(dsc, 0, sizeof(*dsc)); dsc->resolved_font = font; dsc->adv_w = 12; dsc->box_w = 10; dsc->box_h = 16; return true;
}
static const void * test_font_bitmap(lv_font_glyph_dsc_t * dsc, lv_draw_buf_t * buf) { LV_UNUSED(dsc); LV_UNUSED(buf); return NULL; }
static lv_font_t test_font = { .get_glyph_dsc = test_font_glyph, .get_glyph_bitmap = test_font_bitmap, .line_height = 20 };
lv_font_t lv_font_montserrat_16 = { .get_glyph_dsc = test_font_glyph, .get_glyph_bitmap = test_font_bitmap, .line_height = 20 };

#ifdef VOLUME_SPEC_FINAL
#include "gui_volume_settings.c"
#define build_startup_max() gui_volume_settings_build_startup_max_screens()
#define build_car() gui_volume_settings_build_car_mode_screen()
#define teardown_startup_max() gui_volume_settings_teardown_startup_max_screens()
#define teardown_car() gui_volume_settings_teardown_car_mode_screen()
#define startup_row(e) gui_volume_settings_startup_volume_row_cb(e)
#define max_row(e) gui_volume_settings_max_volume_row_cb(e)
#define car_row(e) gui_volume_settings_car_mode_row_cb(e)
#else
#include "gui_volume_settings_baseline_fixture.c"
#define build_startup_max() volume_spec_build_startup_max()
#define build_car() volume_spec_build_car()
#define teardown_startup_max() volume_spec_teardown_startup_max()
#define teardown_car() volume_spec_teardown_car()
#define startup_row(e) volume_spec_startup_row_cb(e)
#define max_row(e) volume_spec_max_row_cb(e)
#define car_row(e) volume_spec_car_row_cb(e)
#endif

const char * i18n_tr(const char * text) { return text; }
const lv_font_t * gui_theme_font(gui_font_role_t role) { LV_UNUSED(role); return &test_font; }
lv_style_t * gui_theme_accent_style(void) { return &accent_style; }
void settings_save(const player_settings_t * s) { assert(s == &current_settings); save_sync_count++; trace("save-sync;"); }
void settings_save_async(const player_settings_t * s) { assert(s == &current_settings); save_async_count++; trace("save-async;"); }
void nav_push(lv_obj_t * screen) { assert(screen); nav_count++; trace("nav-push;"); }
void nav_pop(void) { }
void generic_back_cb(lv_event_t * e) { LV_UNUSED(e); }
void finalize_screen_navigation(lv_obj_t * screen) { LV_UNUSED(screen); }
lv_obj_t * build_screen_header(lv_obj_t * screen, const char * title, lv_event_cb_t back, const char * trailing, lv_event_cb_t cb) {
    LV_UNUSED(back); LV_UNUSED(trailing); LV_UNUSED(cb);
    assert(build_count < 4); built[build_count++] = screen; last_screen = screen;
    if (strcmp(title, "Startup Volume") == 0) startup_screen = screen;
    else if (strcmp(title, "Max Volume") == 0) max_screen = screen;
    else if (strcmp(title, "Car Mode") == 0) car_screen = screen;
    lv_obj_t * label = lv_label_create(screen); lv_label_set_text(label, title); return label;
}
lv_obj_t * build_setting_slider_card(lv_obj_t * parent, lv_obj_t * row, int32_t width, int32_t height,
                                     int32_t min, int32_t max, int32_t value, lv_event_cb_t cb,
                                     lv_obj_t ** slider_out, lv_obj_t ** label_out) {
    LV_UNUSED(row); LV_UNUSED(width); LV_UNUSED(height);
    lv_obj_t * card = lv_obj_create(parent);
    last_slider = lv_slider_create(card); lv_slider_set_range(last_slider, min, max); lv_slider_set_value(last_slider, value, LV_ANIM_OFF);
    if (cb) { lv_obj_add_event_cb(last_slider, cb, LV_EVENT_VALUE_CHANGED, NULL); lv_obj_add_event_cb(last_slider, cb, LV_EVENT_RELEASED, NULL); lv_obj_add_event_cb(last_slider, cb, LV_EVENT_PRESS_LOST, NULL); }
    assert(slider_count < 4); sliders[slider_count++] = last_slider;
    *slider_out = last_slider; *label_out = lv_label_create(card);
    return card;
}
void gui_style_settings_dropdown(lv_obj_t * obj) { LV_UNUSED(obj); }
void refresh_volume_topbar(int32_t percent) { (void)percent; topbar_count++; trace("topbar;"); }
void gui_shell_refresh_quick_drawer_expansion_toggles(void) { expansion_refresh_count++; trace("drawer-refresh;"); }
float audio_get_volume(void) { audio_get_count++; trace("audio-get;"); return audio_volume; }
void audio_set_volume(float value) { audio_set_count++; audio_volume = value; trace("audio-set;"); }
void audio_set_max_volume_percent(int percent) { cap_set_count++; trace("cap-set;"); if ((int)(audio_volume * 100) > percent) { audio_volume = (float)percent / 100.0f; audio_volume_capped = true; } }
void gui_player_set_volume_percent(int32_t percent) { (void)percent; player_set_count++; trace("player-set;"); }
void gui_player_remember_volume_percent(int32_t percent) { (void)percent; remember_count++; trace("remember;"); }
void gui_player_set_car_mode_enabled(bool enabled) { last_car_mode_enabled=enabled; car_enabled_count++; trace(enabled ? "car-enabled:on;" : "car-enabled:off;"); }
int plugin_manager_find_quick_toggle_by_id(const char * id) { assert(strcmp(id, "gain") == 0); plugin_find_count++; trace("plugin-find;"); return quick_index; }
bool plugin_manager_get_quick_toggle_value(int index) { assert(index == quick_index); plugin_get_count++; trace("plugin-get;"); return plugin_gain_enabled; }
void plugin_manager_quick_toggle_set(int index, bool value) { assert(index == quick_index); last_plugin_set_value = plugin_gain_enabled = value; plugin_set_count++; trace(value ? "plugin-set:on;" : "plugin-set:off;"); }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }

static lv_obj_t * find_nth_type(lv_obj_t * root, const lv_obj_class_t * type, int * index) {
    if (lv_obj_check_type(root, type)) {
        if ((*index)-- == 0) return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t * found = find_nth_type(lv_obj_get_child(root, (int32_t)i), type, index);
        if (found) return found;
    }
    return NULL;
}

static void clear_effects(void) { effects[0] = 0; audio_get_count = audio_set_count = cap_set_count = player_set_count = remember_count = topbar_count = 0; save_sync_count = save_async_count = 0; plugin_find_count = plugin_get_count = plugin_set_count = expansion_refresh_count = 0; }
static void send(lv_obj_t * o, lv_event_code_t code) { assert(o); lv_obj_send_event(o, code, NULL); }
static void build_all(void) { build_startup_max(); build_car(); }
static void teardown_all(void) { teardown_startup_max(); teardown_car(); }
static void test_startup(void) {
    clear_effects(); current_settings.startup_volume_fixed_enabled = true; current_settings.startup_volume_fixed_percent = 37;
    slider_count = 0; build_count = 0; build_startup_max(); lv_obj_t * slider = sliders[0];
    int switch_index = 0;
    lv_obj_t * fixed_switch = find_nth_type(startup_screen, &lv_switch_class, &switch_index);
    assert(fixed_switch);
    lv_obj_clear_state(fixed_switch, LV_STATE_CHECKED); send(fixed_switch, LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.startup_volume_fixed_enabled && save_sync_count == 1);
    assert(lv_obj_has_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_state(fixed_switch, LV_STATE_CHECKED); send(fixed_switch, LV_EVENT_VALUE_CHANGED);
    assert(current_settings.startup_volume_fixed_enabled && save_sync_count == 2);
    assert(!lv_obj_has_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(effects,"save-sync;save-sync;")==0);
    assert(audio_set_count == 0 && audio_get_count == 0);
    clear_effects();
    const int startup_values[]={0,52,100};
    for(size_t i=0;i<sizeof(startup_values)/sizeof(startup_values[0]);i++) {
        char expected[16]; snprintf(expected,sizeof(expected),"%d%%",startup_values[i]);
        lv_slider_set_value(slider,startup_values[i],LV_ANIM_OFF); send(slider,LV_EVENT_VALUE_CHANGED);
        assert(current_settings.startup_volume_fixed_percent==startup_values[i]);
        assert(strcmp(lv_label_get_text(startup_volume_value_label),expected)==0);
        assert(save_sync_count==(int)i && audio_set_count==0 && audio_get_count==0 && player_set_count==0);
        send(slider,LV_EVENT_RELEASED);
        assert(save_sync_count==(int)i+1 && strcmp(effects, i==0 ? "save-sync;" : (i==1 ? "save-sync;save-sync;" : "save-sync;save-sync;save-sync;"))==0);
    }
    assert(audio_set_count==0 && audio_get_count==0 && player_set_count==0);
    teardown_startup_max();
}
static void test_max_cap_order(void) {
    clear_effects(); current_settings.max_volume_percent = 100; audio_volume = 0.8f;
    slider_count = 0; build_count = 0; build_startup_max(); lv_obj_t * slider = sliders[1];
    lv_slider_set_value(slider, 50, LV_ANIM_OFF); send(slider, LV_EVENT_VALUE_CHANGED);
    assert(current_settings.max_volume_percent == 50 && audio_get_count == 2 && cap_set_count == 1);
    assert(player_set_count == 1 && remember_count == 1 && topbar_count == 1);
    assert(strcmp(effects, "audio-get;cap-set;audio-get;player-set;remember;topbar;") == 0);
    send(slider, LV_EVENT_PRESS_LOST); assert(save_async_count == 1);
    assert(strcmp(lv_label_get_text(max_volume_value_label),"50%") == 0);
    clear_effects();
    /* Raising the cap to its maximum changes the configured ceiling but cannot raise live volume. */
    lv_slider_set_value(slider,100,LV_ANIM_OFF); send(slider,LV_EVENT_VALUE_CHANGED);
    assert(current_settings.max_volume_percent==100 && audio_volume==0.5f);
    assert(audio_get_count==2 && cap_set_count==1 && player_set_count==0 && topbar_count==0);
    assert(strcmp(effects,"audio-get;cap-set;audio-get;")==0);
    send(slider,LV_EVENT_RELEASED); assert(save_async_count==1);
    /* Minimum cap clamps live volume to 1% and mirrors the applied value in order. */
    clear_effects(); lv_slider_set_value(slider,1,LV_ANIM_OFF); send(slider,LV_EVENT_VALUE_CHANGED);
    assert(current_settings.max_volume_percent==1 && audio_volume==0.01f);
    assert(audio_get_count==2 && cap_set_count==1 && player_set_count==1 && remember_count==1 && topbar_count==1);
    assert(strcmp(effects,"audio-get;cap-set;audio-get;player-set;remember;topbar;")==0);
    send(slider,LV_EVENT_RELEASED); assert(save_async_count==1);
    assert(strcmp(lv_label_get_text(max_volume_value_label),"1%") == 0);
    teardown_startup_max();
}
static void test_car_mode(void) {
    clear_effects(); current_settings.car_mode_enabled = false; current_settings.car_mode_volume_percent = 40;
    current_settings.car_mode_autoresume_enabled = false; quick_index = 3;
    gui_settings_sync_car_mode(); /* The public sync hook is harmless before screen creation. */
    assert(plugin_find_count == 0);
    gui_settings_open_car_mode(); assert(nav_count == 0 && plugin_find_count == 0);
    slider_count = 0; build_count = 0; build_car();
    /* Last created slider is Car Mode's. Disabled mode records preference only. */
    lv_obj_t * slider = sliders[slider_count - 1];
    lv_slider_set_value(slider, 65, LV_ANIM_OFF); send(slider, LV_EVENT_VALUE_CHANGED);
    assert(current_settings.car_mode_volume_percent == 65 && audio_set_count == 0 && player_set_count == 0);
    send(slider, LV_EVENT_RELEASED); assert(save_async_count == 1);
    current_settings.car_mode_enabled = true; clear_effects();
    lv_slider_set_value(slider, 72, LV_ANIM_OFF); send(slider, LV_EVENT_VALUE_CHANGED);
    assert(audio_set_count == 1 && audio_get_count == 1 && player_set_count == 1 && topbar_count == 1);
    assert(strcmp(effects, "audio-set;audio-get;player-set;topbar;") == 0);
    const int car_values[]={0,100};
    for(size_t i=0;i<sizeof(car_values)/sizeof(car_values[0]);i++) {
        clear_effects(); lv_slider_set_value(slider,car_values[i],LV_ANIM_OFF); send(slider,LV_EVENT_VALUE_CHANGED);
        assert(current_settings.car_mode_volume_percent==car_values[i]);
        assert(audio_set_count==1 && audio_get_count==1 && player_set_count==1 && topbar_count==1);
        assert(strcmp(effects,"audio-set;audio-get;player-set;topbar;")==0);
        send(slider,LV_EVENT_RELEASED); assert(save_async_count==1);
    }
    /* Switch callbacks and plugin-owned Gain delegation remain part of this UI. */
    int switch_index = 0, dropdown_index = 0;
    lv_obj_t * enable_switch = find_nth_type(car_screen, &lv_switch_class, &switch_index);
    switch_index = 1;
    lv_obj_t * autoresume_switch = find_nth_type(car_screen, &lv_switch_class, &switch_index);
    lv_obj_t * gain_dropdown = find_nth_type(car_screen, &lv_dropdown_class, &dropdown_index);
    assert(enable_switch && autoresume_switch && gain_dropdown);
    clear_effects(); lv_obj_add_state(enable_switch, LV_STATE_CHECKED); send(enable_switch, LV_EVENT_VALUE_CHANGED);
    assert(car_enabled_count == 1 && last_car_mode_enabled && save_async_count == 0);
    lv_obj_clear_state(enable_switch, LV_STATE_CHECKED); send(enable_switch, LV_EVENT_VALUE_CHANGED);
    assert(car_enabled_count == 2 && !last_car_mode_enabled && save_async_count == 0);
    lv_obj_add_state(autoresume_switch, LV_STATE_CHECKED); send(autoresume_switch, LV_EVENT_VALUE_CHANGED);
    assert(current_settings.car_mode_autoresume_enabled && save_async_count == 1);
    lv_obj_clear_state(autoresume_switch, LV_STATE_CHECKED); send(autoresume_switch, LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.car_mode_autoresume_enabled && save_async_count == 2);
    assert(strcmp(effects,"car-enabled:on;car-enabled:off;save-async;save-async;")==0);
    clear_effects(); lv_dropdown_set_selected(gain_dropdown, 1); send(gain_dropdown, LV_EVENT_VALUE_CHANGED);
    assert(lv_dropdown_get_selected(gain_dropdown) == 1);
    assert(plugin_find_count == 1 && plugin_set_count == 1 && expansion_refresh_count == 1);
    assert(last_plugin_set_value && plugin_gain_enabled);
    assert(strcmp(effects, "plugin-find;plugin-set:on;drawer-refresh;") == 0);
    clear_effects(); lv_dropdown_set_selected(gain_dropdown, 0); send(gain_dropdown, LV_EVENT_VALUE_CHANGED);
    assert(lv_dropdown_get_selected(gain_dropdown) == 0);
    assert(plugin_find_count == 1 && plugin_set_count == 1 && expansion_refresh_count == 1);
    assert(!last_plugin_set_value && !plugin_gain_enabled);
    assert(strcmp(effects, "plugin-find;plugin-set:off;drawer-refresh;") == 0);
    /* Screen-loaded synchronizes plugin availability/value. */
    clear_effects(); send(car_screen, LV_EVENT_SCREEN_LOADED);
    assert(plugin_find_count >= 1 && plugin_get_count >= 1);
    quick_index = -1; clear_effects(); gui_settings_sync_car_mode(); assert(plugin_find_count == 1 && plugin_get_count == 0);
    clear_effects(); send(gain_dropdown, LV_EVENT_VALUE_CHANGED); assert(plugin_set_count == 0 && expansion_refresh_count == 0);
    gui_settings_open_car_mode(); /* Screen exists; sync then push. */
    assert(nav_count == 1);
    teardown_car();
    int before = nav_count; gui_settings_open_car_mode(); assert(nav_count == before);
}
int main(void) {
    lv_init(); display = lv_display_create(240, 320); assert(display);
    static uint8_t buffer[240 * 16 * 4]; lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    test_startup(); test_max_cap_order(); test_car_mode(); teardown_all();
    printf("volume-ui-effects: startup-no-live; max-cap-order; car-mode-enabled-disabled-sync-open\n");
    lv_display_delete(display); lv_deinit(); return 0;
}
