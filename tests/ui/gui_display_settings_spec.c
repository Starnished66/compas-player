/* Callback characterization for the display/library/system settings controls.
 * The tested callbacks and rotation helper come directly from gui_settings.c;
 * LVGL is real and hardware, persistence, toast, and reload effects are narrow
 * stubs so this spec never changes a device or host setting. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include "../../src/core/persistence/settings.h"
#include "../../src/ui/settings/gui_settings.c"

player_settings_t current_settings;
lv_font_t lv_font_montserrat_16;

static int save_sync_count, save_async_count, artist_apply_count;
static int reload_count, info_toast_count, rotation_invalidations;
static bool last_artist_value;
static char effects[128];
static char last_toast[96];

static void trace(const char *effect) {
    size_t used = strlen(effects);
    assert(used + strlen(effect) + 1U <= sizeof(effects));
    memcpy(effects + used, effect, strlen(effect) + 1U);
}

void settings_save(const player_settings_t *settings) {
    assert(settings == &current_settings);
    save_sync_count++;
    trace("save;");
}

void settings_save_async(const player_settings_t *settings) {
    assert(settings == &current_settings);
    save_async_count++;
    trace("save-async;");
}

void gui_library_set_artist_images_enabled(bool enabled) {
    last_artist_value = enabled;
    artist_apply_count++;
    trace("artist;");
}

void gui_reload_request(void) {
    reload_count++;
    trace("reload;");
}

void show_info_toast(const char *message) {
    assert(message && message[0]);
    snprintf(last_toast, sizeof(last_toast), "%s", message);
    info_toast_count++;
    trace("toast;");
}

void gui_navigation_invalidate_theme_snapshots(void) {
    rotation_invalidations++;
    trace("invalidate;");
}

void gui_themes_open_picker(void) {}
void gui_player_layout_reload_request(void) {}
void gui_plugin_manage_row_cb(lv_event_t *event) { (void)event; }
void gui_file_manager_tile_cb(lv_event_t *event) { (void)event; }
void bt_dac_settings_row_cb(lv_event_t *event) { (void)event; }
void gui_emulators_row_cb(lv_event_t *event) { (void)event; }
bool gui_plugin_store_open_player_layouts(void) { return false; }
void player_layouts_rescan(void) {}
bool player_layouts_session_clear_selection(void) { return false; }
const player_layout_info_t *player_layouts_get(int index) { (void)index; return NULL; }
const char *player_layouts_effective_id(void) { return "default"; }
bool player_layouts_get_preview(int index, char *out, size_t size) {
    (void)index; if (out && size) out[0] = '\0'; return false;
}
int player_layouts_find(const char *id) { (void)id; return -1; }
int player_layouts_count(void) { return 1; }
lv_obj_t *add_pill_chevron_row(lv_obj_t *list, const char *text, lv_event_cb_t callback) {
    (void)list; (void)text; (void)callback; return NULL;
}
void configure_cover_card_grid(lv_obj_t *list, int columns) { (void)list; (void)columns; }
lv_obj_t *add_cover_card(lv_obj_t *list, const char *label, const char *src, int columns,
                         lv_event_cb_t callback, void *user_data) {
    (void)list; (void)label; (void)src; (void)columns; (void)callback; (void)user_data; return NULL;
}
lv_color_t accent_lv_color(void) { return lv_color_black(); }
void nav_push(lv_obj_t *screen) { (void)screen; }
bool usb_mode_control_detect_current(usb_mode_t *out_mode) { (void)out_mode; return false; }
lv_obj_t *gui_network_get_usb_dac_overlay(void) { return NULL; }
bool start_usb_mode_switch(usb_mode_t target) { (void)target; return false; }

uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }

lv_obj_t *build_subsonic_list_screen(const char *title, lv_obj_t **title_label,
                                     lv_obj_t **list) {
    lv_obj_t *screen = lv_obj_create(NULL);
    assert(screen && title && title_label && list);
    *title_label = lv_label_create(screen);
    lv_label_set_text(*title_label, title);
    *list = lv_obj_create(screen);
    assert(*list);
    return screen;
}

lv_obj_t *add_pill_option_row(lv_obj_t *parent, const char *label, bool selected,
                              lv_event_cb_t callback, void *user_data) {
    lv_obj_t *row = lv_obj_create(parent);
    assert(row && label && callback);
    lv_obj_set_user_data(row, (void *)(intptr_t)selected);
    lv_obj_t *text = lv_label_create(row);
    lv_label_set_text(text, label);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
    return row;
}

static void clear_effects(void) {
    save_sync_count = save_async_count = artist_apply_count = 0;
    reload_count = info_toast_count = rotation_invalidations = 0;
    effects[0] = '\0';
    last_toast[0] = '\0';
}

static void fire(lv_obj_t *target, lv_event_cb_t callback,
                 lv_event_code_t code, void *user_data) {
    lv_obj_add_event_cb(target, callback, code, user_data);
    lv_obj_send_event(target, code, NULL);
}

static lv_obj_t *switch_with_value(bool enabled) {
    lv_obj_t *obj = lv_switch_create(lv_screen_active());
    assert(obj);
    if (enabled) lv_obj_add_state(obj, LV_STATE_CHECKED);
    return obj;
}

static lv_obj_t *event_target(void) {
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    assert(obj);
    return obj;
}

static void test_binary_switch_callbacks(void) {
    lv_obj_t *toggle = switch_with_value(false);

    current_settings.screen_upside_down = true;
    lv_display_t *display = lv_display_get_default();
    assert(display);
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180);
    clear_effects();
    lv_obj_clear_state(toggle, LV_STATE_CHECKED);
    fire(toggle, upside_down_screen_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!current_settings.screen_upside_down);
    assert(save_async_count == 1 && rotation_invalidations == 1);
    assert(lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_0);
    assert(strcmp(effects, "save-async;invalidate;") == 0);

    clear_effects();
    lv_obj_t *toggle_on = switch_with_value(true);
    fire(toggle_on, upside_down_screen_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.screen_upside_down);
    assert(save_async_count == 1 && rotation_invalidations == 1);
    assert(lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_180);
    assert(strcmp(effects, "save-async;invalidate;") == 0);

    clear_effects();
    lv_obj_t *toggle_wrong_event = switch_with_value(false);
    fire(toggle_wrong_event, upside_down_screen_switch_event_cb, LV_EVENT_CLICKED, NULL);
    assert(save_async_count == 0 && rotation_invalidations == 0);

    lv_obj_t *artist_toggle = switch_with_value(false);
    clear_effects();
    fire(artist_toggle, artist_images_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!current_settings.show_artist_images && !last_artist_value);
    assert(save_async_count == 1 && artist_apply_count == 1);
    assert(strcmp(effects, "save-async;artist;") == 0);
    clear_effects();
    lv_obj_t *artist_toggle_on = switch_with_value(true);
    fire(artist_toggle_on, artist_images_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.show_artist_images && last_artist_value);
    assert(save_async_count == 1 && artist_apply_count == 1);
    assert(strcmp(effects, "save-async;artist;") == 0);
    clear_effects();
    lv_obj_t *artist_wrong_event = switch_with_value(false);
    fire(artist_wrong_event, artist_images_switch_event_cb, LV_EVENT_CLICKED, NULL);
    assert(save_async_count == 0 && artist_apply_count == 0);

    lv_obj_t *lyrics_toggle = switch_with_value(false);
    clear_effects();
    fire(lyrics_toggle, lyrics_keep_screen_on_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!current_settings.lyrics_keep_screen_on && save_sync_count == 1);
    assert(strcmp(effects, "save;") == 0);
    clear_effects();
    lv_obj_t *lyrics_toggle_on = switch_with_value(true);
    fire(lyrics_toggle_on, lyrics_keep_screen_on_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(current_settings.lyrics_keep_screen_on && save_sync_count == 1);
    assert(strcmp(effects, "save;") == 0);
    clear_effects();
    lv_obj_t *lyrics_wrong_event = switch_with_value(false);
    fire(lyrics_wrong_event, lyrics_keep_screen_on_switch_event_cb, LV_EVENT_CLICKED, NULL);
    assert(save_sync_count == 0);

    lv_obj_delete(toggle);
    lv_obj_delete(toggle_on);
    lv_obj_delete(toggle_wrong_event);
    lv_obj_delete(artist_toggle);
    lv_obj_delete(artist_toggle_on);
    lv_obj_delete(artist_wrong_event);
    lv_obj_delete(lyrics_toggle);
    lv_obj_delete(lyrics_toggle_on);
    lv_obj_delete(lyrics_wrong_event);
}

static void test_animation_and_keyboard_choices(void) {
    static const int animation_scales[] = { 0, 25, 50, 75, 100 };
    current_settings.animation_scale = 50;
    animation_speed_screen = build_animation_speed_screen();
    assert(animation_speed_screen && animation_speed_list);
    populate_animation_speed_screen();
    assert(lv_obj_get_child_count(animation_speed_list) == 5);
    assert((intptr_t)lv_obj_get_user_data(lv_obj_get_child(animation_speed_list, 2)) == 1);
    for (size_t i = 0; i < sizeof(animation_scales) / sizeof(animation_scales[0]); ++i) {
        lv_obj_t *target = lv_obj_get_child(animation_speed_list, (int32_t)i);
        clear_effects();
        lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
        assert(current_settings.animation_scale == animation_scales[i]);
        assert(save_sync_count == 1 && strcmp(effects, "save;") == 0);
        assert(lv_obj_get_child_count(animation_speed_list) == 5);
        for (size_t row = 0; row < 5; ++row)
            assert((intptr_t)lv_obj_get_user_data(lv_obj_get_child(animation_speed_list, (int32_t)row)) ==
                   (row == i));
    }
    lv_obj_t *target = event_target();
    clear_effects();
    fire(target, animation_speed_option_row_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(save_sync_count == 0);
    lv_obj_delete(target);

    lv_obj_delete(animation_speed_screen);
    animation_speed_screen = animation_speed_list = NULL;

    const size_t keyboard_count = gui_settings_keyboard_layout_count();
    assert(keyboard_count == 6);
    /* Private row callbacks receive these indices only from the option-row
     * builders above. The public accessor's invalid-index behavior is tested;
     * forging an out-of-contract private callback index would test UB. */
    current_settings.keyboard_layout = gui_settings_keyboard_layout_at(0, NULL);
    keyboard_layout_screen = build_keyboard_layout_screen();
    assert(keyboard_layout_screen && keyboard_layout_list);
    populate_keyboard_layout_screen();
    assert(lv_obj_get_child_count(keyboard_layout_list) == keyboard_count);
    assert((intptr_t)lv_obj_get_user_data(lv_obj_get_child(keyboard_layout_list, 0)) == 1);
    for (size_t i = 0; i < keyboard_count; ++i) {
        const char *label = NULL;
        int layout = gui_settings_keyboard_layout_at(i, &label);
        assert(layout >= 0 && label && label[0]);
        clear_effects();
        lv_obj_send_event(lv_obj_get_child(keyboard_layout_list, (int32_t)i), LV_EVENT_CLICKED, NULL);
        assert(current_settings.keyboard_layout == layout);
        assert(save_sync_count == 1 && strcmp(effects, "save;") == 0);
        assert(lv_obj_get_child_count(keyboard_layout_list) == keyboard_count);
        for (size_t row = 0; row < keyboard_count; ++row)
            assert((intptr_t)lv_obj_get_user_data(lv_obj_get_child(keyboard_layout_list, (int32_t)row)) ==
                   (row == i));
    }
    const char *invalid_label = NULL;
    assert(gui_settings_keyboard_layout_at(keyboard_count, &invalid_label) == -1);
    assert(gui_settings_keyboard_layout_at(keyboard_count + 1, NULL) == -1);
    target = event_target();
    clear_effects();
    fire(target, keyboard_layout_option_row_cb, LV_EVENT_VALUE_CHANGED, NULL);
    assert(save_sync_count == 0);
    lv_obj_delete(target);
    lv_obj_delete(keyboard_layout_screen);
    keyboard_layout_screen = keyboard_layout_list = NULL;
}

static void test_language_selection_and_reload_order(void) {
    assert(i18n_set_language("en"));
    current_settings.language[0] = '\0';
    size_t count = i18n_language_count();
    assert(count > 1);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *target = event_target();
        const char *code = i18n_language_code(i);
        assert(code && code[0]);
        assert(i18n_set_language("en"));
        snprintf(current_settings.language, sizeof(current_settings.language), "en");
        clear_effects();
        fire(target, language_choice_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (strcmp(code, "en") == 0) {
            assert(strcmp(i18n_get_language(), "en") == 0);
            assert(strcmp(current_settings.language, "en") == 0);
            assert(save_sync_count == 0 && info_toast_count == 0 && reload_count == 0);
            assert(effects[0] == '\0');
        } else {
            assert(strcmp(i18n_get_language(), code) == 0);
            assert(strcmp(current_settings.language, code) == 0);
            assert(save_sync_count == 1 && info_toast_count == 1 && reload_count == 1);
            assert(strcmp(last_toast, i18n_tr("Applying language, this may take a while")) == 0);
            assert(strcmp(effects, "save;toast;reload;") == 0);
        }
        lv_obj_delete(target);
    }
    assert(i18n_set_language("en"));
    snprintf(current_settings.language, sizeof(current_settings.language), "en");
    lv_obj_t *target = event_target();
    clear_effects();
    fire(target, language_choice_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)count);
    assert(save_sync_count == 0 && info_toast_count == 0 && reload_count == 0);
    assert(strcmp(i18n_get_language(), "en") == 0);
    assert(!i18n_set_language("zz-test-invalid"));
    assert(strcmp(i18n_get_language(), "en") == 0);
    lv_obj_delete(target);
    target = event_target();
    clear_effects();
    fire(target, language_choice_row_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)1);
    assert(save_sync_count == 0 && reload_count == 0);
    assert(i18n_set_language("en"));
    lv_obj_delete(target);
}

int main(void) {
    lv_init();
    lv_display_t *display = lv_display_create(240, 320);
    assert(display);
    static uint8_t buffer[240 * 20 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    test_binary_switch_callbacks();
    test_animation_and_keyboard_choices();
    test_language_selection_and_reload_order();
    lv_display_delete(display);
    lv_deinit();
    puts("display-settings-callbacks: toggles, rotation order, animation/keyboard choices, language reload");
    return 0;
}
