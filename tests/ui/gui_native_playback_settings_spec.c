/* Native Playback & Controls settings callbacks, source-sliced from their
 * production owners. UI/backend services are local host stubs; controls are
 * real LVGL objects and events. This never opens audio or hardware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "../../src/core/persistence/settings.h"
#include "../../src/audio/engine/audio.h"
#include "../../src/ui/player/screen/gui_player.h"
#include "../../src/ui/settings/gui_settings.h"
#include "../../src/ui/shell/gui_shell.h"
#include "../../src/ui/shell/gui_navigation.h"
#include "../../src/ui/widgets/builders/screen_builders.h"
#include "../../src/plugins/manager/plugin_manager.h"
#include "../../src/ui/text/i18n/i18n.h"
#include "../../src/hardware/input/hw_buttons.h"
#include "../../src/hardware/device/headphone_status.h"
#include "../../src/ui/settings/input/gui_button_mapping.h"
#include "../../src/ui/application/gui.h"

player_settings_t current_settings;
lv_style_t style_theme_screen_bg, style_theme_card_bg, style_theme_text_primary, style_theme_text_muted;
static lv_style_t accent_style;
static lv_display_t *display;
static lv_obj_t *built_screen;
static lv_obj_t *captured_switches[16];
static lv_obj_t *captured_click_rows[16];
static pill_list_item_t captured_items[16];
static int captured_count;
static lv_obj_t *resume_mode_screen;
static lv_obj_t *resume_mode_list;
static lv_obj_t *resume_option_buttons[3];
static bool resume_option_selected[3];
static char resume_option_labels[3][64];
static int resume_option_count;
static lv_event_cb_t wrong_event_callback;
static int settings_save_count, settings_save_async_count, nav_push_count, info_toast_count;
static int audio_crossfade_count, arm_count, crossfade_icon_count, drawer_refresh_count;
static int volume_lock_backend_count, inline_remote_backend_count, lock_backend_count;
static int active_track_index = 4;
static bool active_track = true;
static lv_obj_t *pushed_screen;
static char effects[512];
lv_obj_t *settings_crossfade_toggle_img, *settings_gapless_toggle_img;
static lv_obj_t *settings_car_summary;

static void record(const char *text) {
    size_t used = strlen(effects);
    snprintf(effects + used, sizeof(effects) - used, "%s;", text);
}
static void clear_effects(void) {
    effects[0] = '\0';
    settings_save_count = settings_save_async_count = nav_push_count = info_toast_count = 0;
    audio_crossfade_count = arm_count = crossfade_icon_count = drawer_refresh_count = 0;
    volume_lock_backend_count = inline_remote_backend_count = lock_backend_count = 0;
}
static bool test_font_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                            uint32_t letter, uint32_t next_letter) {
    LV_UNUSED(letter); LV_UNUSED(next_letter); memset(dsc, 0, sizeof(*dsc));
    dsc->resolved_font = font; dsc->adv_w = 12; dsc->box_w = 10; dsc->box_h = 16; return true;
}
static const void *test_font_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf) {
    LV_UNUSED(dsc); LV_UNUSED(draw_buf); return NULL;
}
lv_font_t lv_font_montserrat_16 = { .get_glyph_dsc = test_font_glyph, .get_glyph_bitmap = test_font_bitmap, .line_height = 20 };

const char *i18n_tr(const char *text) { return text; }
lv_style_t *gui_theme_accent_style(void) { return &accent_style; }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
static void settings_summary_loaded_cb(lv_event_t *e) { LV_UNUSED(e); }
void finalize_screen_navigation(lv_obj_t *screen) { LV_UNUSED(screen); }
void generic_back_cb(lv_event_t *e) { LV_UNUSED(e); }
void gui_button_mapping_row_cb(lv_event_t *e) { LV_UNUSED(e); }
void gui_button_mapping_set_category_summary(lv_obj_t *label) { LV_UNUSED(label); }
void settings_save(const player_settings_t *settings) { assert(settings == &current_settings); settings_save_count++; record("save-sync"); }
void settings_save_async(const player_settings_t *settings) { assert(settings == &current_settings); settings_save_async_count++; record("save-async"); }
void audio_set_crossfade_enabled(bool enabled) { audio_crossfade_count++; record(enabled ? "audio-crossfade-on" : "audio-crossfade-off"); }
bool gui_player_has_active_track(void) { return active_track; }
int player_queue_model_current(void) { return active_track_index; }
void arm_next_track_for_audio(int index) { assert(index == active_track_index); arm_count++; record("arm-next"); }
void refresh_quick_drawer_crossfade_icon(void) { crossfade_icon_count++; record("crossfade-icon"); }
void gui_shell_refresh_quick_drawer_expansion_toggles(void) { drawer_refresh_count++; record("drawer-refresh"); }
void hw_buttons_set_volume_lock_screen_off(bool enabled) { volume_lock_backend_count++; lock_backend_count = enabled ? 1 : 0; record(enabled ? "hardware-lock-on" : "hardware-lock-off"); }
void headphone_status_refresh_earpods_adc(void) { inline_remote_backend_count++; record("remote-refresh"); }
void plugin_manager_playback_list_item_clicked(int index) { LV_UNUSED(index); }
void plugin_manager_music_controls_list_item_clicked(int index) { LV_UNUSED(index); }
int plugin_manager_get_playback_list_item_count(void) { return 0; }
const char *plugin_manager_get_playback_list_item_label(int index) { LV_UNUSED(index); return ""; }
void plugin_manager_get_playback_list_item_options(int index, const char **icon, int32_t *height,
                                                    int32_t *width, const char **text_size) {
    LV_UNUSED(index); LV_UNUSED(icon); LV_UNUSED(height); LV_UNUSED(width); LV_UNUSED(text_size);
}
int plugin_manager_get_music_controls_list_item_count(void) { return 0; }
const char *plugin_manager_get_music_controls_list_item_label(int index) { LV_UNUSED(index); return ""; }
void plugin_manager_get_music_controls_list_item_options(int index, const char **icon, int32_t *height,
                                                          int32_t *width, const char **text_size) {
    LV_UNUSED(index); LV_UNUSED(icon); LV_UNUSED(height); LV_UNUSED(width); LV_UNUSED(text_size);
}
int append_plugin_list_rows(pill_list_item_t *items, int count, int max_items,
                            plugin_list_item_count_cb_t count_fn, plugin_list_item_label_cb_t label_fn,
                            plugin_list_item_options_cb_t options_fn, lv_event_cb_t click_cb) {
    LV_UNUSED(items); LV_UNUSED(max_items); LV_UNUSED(count_fn);
    LV_UNUSED(label_fn); LV_UNUSED(options_fn); LV_UNUSED(click_cb); return count;
}
lv_obj_t *add_pill_summary_label(lv_obj_t *row) { LV_UNUSED(row); return NULL; }
void show_info_toast(const char *text) { assert(strcmp(text, "Applies next time you launch the app") == 0); info_toast_count++; record("resume-info"); }
void nav_push(lv_obj_t *screen) { assert(screen); pushed_screen = screen; nav_push_count++; record("nav-push"); }
lv_obj_t *build_subsonic_list_screen(const char *title, lv_obj_t **title_label, lv_obj_t **list) {
    lv_obj_t *screen = lv_obj_create(lv_screen_active());
    *title_label = lv_label_create(screen); lv_label_set_text(*title_label, title);
    *list = lv_obj_create(screen); return screen;
}
static void music_controls_menu_cb(lv_event_t *e) { LV_UNUSED(e); }
void gui_volume_settings_car_mode_row_cb(lv_event_t *e) { LV_UNUSED(e); }

lv_obj_t *build_pill_list_screen(const char *title, lv_event_cb_t back_cb,
                                 const pill_list_item_t *items, int item_count,
                                 lv_style_t *toggle_accent_style, int32_t row_gap, int32_t icon_scale_pct) {
    LV_UNUSED(title); LV_UNUSED(back_cb); LV_UNUSED(toggle_accent_style); LV_UNUSED(row_gap); LV_UNUSED(icon_scale_pct);
    assert(item_count <= (int)(sizeof(captured_items) / sizeof(captured_items[0])));
    captured_count = item_count;
    built_screen = lv_obj_create(lv_screen_active());
    for (int i = 0; i < item_count; i++) {
        captured_items[i] = items[i];
        if (items[i].accessory == PILL_ACCESSORY_TOGGLE) {
            lv_obj_t *sw = lv_switch_create(built_screen);
            if (items[i].toggle_initial_state) lv_obj_add_state(sw, LV_STATE_CHECKED);
            if (items[i].out_toggle_img) *items[i].out_toggle_img = sw;
            captured_switches[i] = sw;
            if (items[i].on_toggle_change)
                lv_obj_add_event_cb(sw, items[i].on_toggle_change, LV_EVENT_VALUE_CHANGED, items[i].user_data);
        } else if (items[i].on_click) {
            lv_obj_t *row = lv_obj_create(built_screen);
            captured_click_rows[i] = row;
            lv_obj_add_event_cb(row, items[i].on_click, LV_EVENT_CLICKED, items[i].user_data);
        }
        if (items[i].out_row) *items[i].out_row = lv_obj_create(built_screen);
    }
    return built_screen;
}
lv_obj_t *add_pill_option_row(lv_obj_t *parent, const char *label, bool selected,
                              lv_event_cb_t callback, void *user_data) {
    if (resume_option_count >= 3) resume_option_count = 0;
    int index = resume_option_count++;
    snprintf(resume_option_labels[index], sizeof(resume_option_labels[index]), "%s", label);
    resume_option_selected[index] = selected;
    lv_obj_t *row = lv_obj_create(parent);
    resume_option_buttons[index] = row;
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
    return row;
}

#include "native_playback_settings_fixture.inc"

static void send(lv_obj_t *obj, lv_event_code_t code) { assert(obj); lv_obj_send_event(obj, code, NULL); }
static void wrong_event_dispatch(lv_event_t *e) {
    assert(lv_event_get_code(e) == LV_EVENT_RELEASED);
    assert(wrong_event_callback);
    wrong_event_callback(e);
}
static void send_wrong_event(lv_obj_t *obj, lv_event_cb_t callback) {
    assert(obj && callback);
    lv_obj_t *event_source = lv_obj_create(lv_screen_active());
    assert(event_source);
    wrong_event_callback = callback;
    lv_obj_add_event_cb(event_source, wrong_event_dispatch, LV_EVENT_RELEASED, NULL);
    send(event_source, LV_EVENT_RELEASED);
    wrong_event_callback = NULL;
    lv_obj_delete(event_source);
}
static int row_by_label(const char *label) {
    for (int i = 0; i < captured_count; i++) if (strcmp(captured_items[i].label, label) == 0) return i;
    return -1;
}
static void assert_effects(const char *expected) { assert(strcmp(effects, expected) == 0); }

static void test_playback_rows_and_coupling(void) {
    current_settings.crossfade_enabled = false;
    current_settings.gapless_enabled = true;
    assert(settings_crossfade_toggle_img == NULL && settings_gapless_toggle_img == NULL);
    build_music_playback_screen();
    assert(captured_count == 5);
    assert(strcmp(captured_items[0].label, "Resume Last Track") == 0);
    assert(strcmp(captured_items[1].label, "Crossfade") == 0 && captured_items[1].accessory == PILL_ACCESSORY_TOGGLE);
    assert(strcmp(captured_items[2].label, "Gapless") == 0 && captured_items[2].accessory == PILL_ACCESSORY_TOGGLE);
    assert(strcmp(captured_items[3].label, "Car Mode") == 0);
    assert(strcmp(captured_items[4].label, "Buttons & Remote") == 0);
    for (int i = 0; i < captured_count; i++) assert(strstr(captured_items[i].label, "Duration") == NULL);
    int crossfade = row_by_label("Crossfade"), gapless = row_by_label("Gapless");
    assert(crossfade >= 0 && gapless >= 0);
    assert(!lv_obj_has_state(captured_switches[crossfade], LV_STATE_CHECKED));
    assert(lv_obj_has_state(captured_switches[gapless], LV_STATE_CHECKED));

    clear_effects(); send_wrong_event(captured_switches[crossfade], crossfade_switch_event_cb);
    send_wrong_event(captured_switches[gapless], gapless_switch_event_cb);
    assert(settings_save_async_count == 0 && audio_crossfade_count == 0);
    assert(!current_settings.crossfade_enabled && current_settings.gapless_enabled);
    lv_obj_add_state(captured_switches[crossfade], LV_STATE_CHECKED);
    send(captured_switches[crossfade], LV_EVENT_VALUE_CHANGED);
    assert(current_settings.crossfade_enabled && current_settings.gapless_enabled);
    assert(lv_obj_has_state(captured_switches[gapless], LV_STATE_CHECKED));
    assert(settings_save_async_count == 1 && audio_crossfade_count == 1 && arm_count == 1);
    assert_effects("audio-crossfade-on;save-async;arm-next;crossfade-icon;drawer-refresh;");

    clear_effects(); lv_obj_clear_state(captured_switches[crossfade], LV_STATE_CHECKED);
    send(captured_switches[crossfade], LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.crossfade_enabled && current_settings.gapless_enabled);
    assert(settings_save_async_count == 1 && audio_crossfade_count == 1 && arm_count == 1);
    assert_effects("audio-crossfade-off;save-async;arm-next;crossfade-icon;drawer-refresh;");

    lv_obj_add_state(captured_switches[crossfade], LV_STATE_CHECKED);
    send(captured_switches[crossfade], LV_EVENT_VALUE_CHANGED);
    clear_effects(); lv_obj_clear_state(captured_switches[gapless], LV_STATE_CHECKED);
    send(captured_switches[gapless], LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.gapless_enabled && !current_settings.crossfade_enabled);
    assert(!lv_obj_has_state(captured_switches[crossfade], LV_STATE_CHECKED));
    assert(!lv_obj_has_state(captured_switches[gapless], LV_STATE_CHECKED));
    assert(settings_save_async_count == 2 && audio_crossfade_count == 1 && arm_count == 2);
    assert_effects("audio-crossfade-off;save-async;arm-next;crossfade-icon;drawer-refresh;save-async;arm-next;drawer-refresh;");

    active_track = false; clear_effects();
    lv_obj_add_state(captured_switches[gapless], LV_STATE_CHECKED);
    send(captured_switches[gapless], LV_EVENT_VALUE_CHANGED);
    assert(current_settings.gapless_enabled && !current_settings.crossfade_enabled);
    assert(settings_save_async_count == 1 && arm_count == 0);
    assert_effects("save-async;drawer-refresh;");
    active_track = true;
}

static void test_volume_lock_and_inline_remote(void) {
    current_settings.volume_lock_screen_off = false;
    current_settings.inline_remote_enabled = true;
    build_music_controls_screen();
    assert(captured_count == 3);
    assert(strcmp(captured_items[1].label, "Volume Lock (Screen Off)") == 0);
    assert(strcmp(captured_items[2].label, "In-line Remote") == 0);
    int lock = row_by_label("Volume Lock (Screen Off)"), remote = row_by_label("In-line Remote");
    assert(lock >= 0 && remote >= 0);
    assert(!lv_obj_has_state(captured_switches[lock], LV_STATE_CHECKED));
    assert(lv_obj_has_state(captured_switches[remote], LV_STATE_CHECKED));

    clear_effects(); send_wrong_event(captured_switches[lock], volume_lock_switch_event_cb);
    assert(settings_save_count == 0 && volume_lock_backend_count == 0);
    lv_obj_add_state(captured_switches[lock], LV_STATE_CHECKED);
    send(captured_switches[lock], LV_EVENT_VALUE_CHANGED);
    assert(current_settings.volume_lock_screen_off && settings_save_count == 1);
    assert(volume_lock_backend_count == 1 && lock_backend_count == 1);
    assert_effects("save-sync;hardware-lock-on;");
    clear_effects(); lv_obj_clear_state(captured_switches[lock], LV_STATE_CHECKED);
    send(captured_switches[lock], LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.volume_lock_screen_off && settings_save_count == 1);
    assert(volume_lock_backend_count == 1 && lock_backend_count == 0);
    assert_effects("save-sync;hardware-lock-off;");

    clear_effects(); send_wrong_event(captured_switches[remote], inline_remote_switch_event_cb);
    assert(settings_save_async_count == 0 && inline_remote_backend_count == 0);
    lv_obj_clear_state(captured_switches[remote], LV_STATE_CHECKED);
    send(captured_switches[remote], LV_EVENT_VALUE_CHANGED);
    assert(!current_settings.inline_remote_enabled && settings_save_async_count == 1);
    assert(inline_remote_backend_count == 1);
    assert_effects("remote-refresh;save-async;");
    clear_effects(); lv_obj_add_state(captured_switches[remote], LV_STATE_CHECKED);
    send(captured_switches[remote], LV_EVENT_VALUE_CHANGED);
    assert(current_settings.inline_remote_enabled && settings_save_async_count == 1);
    assert(inline_remote_backend_count == 1);
    assert_effects("remote-refresh;save-async;");
}

static void assert_resume_selection(int selected) {
    assert(resume_option_count == 3);
    for (int i = 0; i < 3; i++) assert(resume_option_selected[i] == (i == selected));
}
static void assert_resume_child_count(void) {
    assert(resume_mode_list);
    assert(lv_obj_get_child_count(resume_mode_list) == 3);
}
static void test_resume_picker(void) {
    resume_mode_screen = build_resume_mode_screen();
    assert(resume_mode_screen && resume_mode_list);
    current_settings.resume_mode = 0;
    captured_count = 0;
    build_music_playback_screen();
    int resume_row = row_by_label("Resume Last Track");
    assert(resume_row >= 0);
    clear_effects(); send_wrong_event(captured_click_rows[resume_row], resume_mode_settings_row_cb);
    assert(nav_push_count == 0 && resume_option_count == 0);
    send(captured_click_rows[resume_row], LV_EVENT_CLICKED);
    assert(nav_push_count == 1 && pushed_screen == resume_mode_screen);
    assert(strcmp(resume_option_labels[0], "Off") == 0);
    assert(strcmp(resume_option_labels[1], "Resume and Play") == 0);
    assert(strcmp(resume_option_labels[2], "Resume, but Paused") == 0);
    assert_resume_selection(0);

    current_settings.resume_mode = 1; populate_resume_mode_screen(); assert_resume_child_count(); assert_resume_selection(1);
    current_settings.resume_mode = 2; populate_resume_mode_screen(); assert_resume_child_count(); assert_resume_selection(2);
    current_settings.resume_mode = 99; populate_resume_mode_screen();
    assert_resume_child_count();
    for (int i = 0; i < 3; i++) assert(!resume_option_selected[i]);

    clear_effects(); send_wrong_event(resume_option_buttons[2], resume_mode_option_row_cb);
    assert(current_settings.resume_mode == 99 && settings_save_count == 0);
    send(resume_option_buttons[1], LV_EVENT_CLICKED);
    assert(current_settings.resume_mode == 1 && settings_save_count == 1 && info_toast_count == 1);
    assert_resume_child_count();
    assert_resume_selection(1);
    assert_effects("save-sync;resume-info;");
    clear_effects(); send(resume_option_buttons[0], LV_EVENT_CLICKED);
    assert(current_settings.resume_mode == 0 && settings_save_count == 1 && info_toast_count == 1);
    assert_resume_child_count();
    assert_resume_selection(0);
    assert_effects("save-sync;resume-info;");
}

int main(void) {
    lv_init();
    display = lv_display_create(240, 320); assert(display);
    static uint8_t buffer[240 * 16 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    test_playback_rows_and_coupling();
    test_volume_lock_and_inline_remote();
    test_resume_picker();
    puts("Native playback settings: crossfade/gapless coupling, volume lock, inline remote, and resume picker passed");
    lv_display_delete(display); lv_deinit();
    return 0;
}
