#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "button_mapping.h"
#include "gui_settings.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"
#include "settings.h"

player_settings_t current_settings;
static lv_style_t accent_style;
static lv_obj_t * navigation[8];
static size_t navigation_depth;
static lv_obj_t * popup_confirm;
static lv_obj_t * popup_cancel;
static lv_event_cb_t popup_confirm_cb;
static lv_event_cb_t popup_cancel_cb;
static bool popup_visible;
static int popup_teardown_count;
static int toast_count;
static int save_count;
static int apply_count;

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

const char * i18n_tr(const char * text) { return text; }

#if defined(BUTTON_MAPPING_SPEC_BASELINE)
static lv_obj_t * settings_add_summary(lv_obj_t * row);
#else
lv_obj_t * add_pill_summary_label(lv_obj_t * row);
#endif
static lv_obj_t * settings_button_mapping_summary __attribute__((unused));
void nav_push(lv_obj_t * screen);
void nav_pop(void);
void finalize_screen_navigation(lv_obj_t * screen);
void generic_back_cb(lv_event_t * e);
bool gui_button_bindings_allow_wake(const button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT]);
void gui_apply_button_bindings(void);
void show_error_toast(const char * text);

#if defined(BUTTON_MAPPING_SPEC_BASELINE)
#include "button_mapping_baseline_fixture.c"
static void test_row_cb(lv_event_t * e) { button_mapping_open_menu_cb(e); }
static void test_set_summary(lv_obj_t * label) { settings_button_mapping_summary = label; }
static void test_refresh(void) { refresh_button_mapping_summaries(); }
static void test_build_popup(void) { build_button_mapping_reset_popup(); }
static void test_teardown_popup(void) { gui_popup_teardown(&button_mapping_reset_popup); }
#include "button_mapping_baseline_teardown.c"
#else
#include "gui_button_mapping.c"
static void test_row_cb(lv_event_t * e) { gui_button_mapping_row_cb(e); }
static void test_set_summary(lv_obj_t * label) { gui_button_mapping_set_category_summary(label); }
static void test_refresh(void) { gui_button_mapping_refresh_summaries(); }
static void test_build_popup(void) { gui_button_mapping_build_reset_popup(); }
static void test_teardown_popup(void) { gui_button_mapping_teardown_reset_popup(); }
#endif

static lv_obj_t * make_row(lv_obj_t * parent, const char * text, lv_event_cb_t cb, void * data) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 240, 44);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, data);
    return row;
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_cb,
                                  const pill_list_item_t * items, int count,
                                  lv_style_t * style, int32_t gap, int32_t icon_pct) {
    (void)back_cb; (void)style; (void)gap; (void)icon_pct;
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 240, 320);
    lv_obj_t * title_label = lv_label_create(screen);
    lv_label_set_text(title_label, title);
    lv_obj_t * list = lv_obj_create(screen);
    for (int i = 0; i < count; ++i) {
        lv_obj_t * row = make_row(list, items[i].label, items[i].on_click, items[i].user_data);
        if (items[i].out_row) *items[i].out_row = row;
    }
    return screen;
}

lv_obj_t * build_subsonic_list_screen(const char * title, lv_obj_t ** title_out,
                                      lv_obj_t ** list_out) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_t * title_label = lv_label_create(screen);
    lv_label_set_text(title_label, title);
    lv_obj_t * list = lv_obj_create(screen);
    if (title_out) *title_out = title_label;
    if (list_out) *list_out = list;
    return screen;
}

lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * text, bool selected,
                               lv_event_cb_t cb, void * data) {
    (void)selected;
    return make_row(parent, text, cb, data);
}

#if defined(BUTTON_MAPPING_SPEC_BASELINE)
static lv_obj_t * settings_add_summary(lv_obj_t * row) {
#else
lv_obj_t * add_pill_summary_label(lv_obj_t * row) {
#endif
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, "");
    return label;
}

lv_style_t * gui_theme_accent_style(void) { return &accent_style; }
lv_color_t accent_lv_color(void) { return lv_color_hex(0x336699); }
void nav_push(lv_obj_t * screen) {
    assert(navigation_depth < 8);
    navigation[navigation_depth++] = screen;
    lv_screen_load(screen);
}
void nav_pop(void) {
    assert(navigation_depth > 0);
    --navigation_depth;
}
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
void generic_back_cb(lv_event_t * e) { (void)e; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
void settings_save_async(const player_settings_t * settings) { assert(settings == &current_settings); ++save_count; }
void gui_apply_button_bindings(void) { ++apply_count; }
void show_error_toast(const char * text) { assert(text && *text); ++toast_count; }

lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t mode,
                               lv_obj_t ** title_out, const char * body_text,
                               const char * confirm_text, lv_color_t confirm_color,
                               lv_event_cb_t confirm_cb, lv_obj_t ** confirm_row_out,
                               const char * cancel_text, lv_color_t cancel_color,
                               lv_event_cb_t cancel_cb, lv_obj_t ** cancel_row_out,
                               lv_event_cb_t backdrop_cb, lv_obj_t ** backdrop_out) {
    (void)mode; (void)body_text; (void)confirm_color; (void)cancel_color; (void)backdrop_cb;
    lv_obj_t * popup = lv_obj_create(NULL);
    lv_obj_t * title = lv_label_create(popup);
    lv_label_set_text(title, title_text);
    lv_obj_t * backdrop = lv_obj_create(NULL);
    popup_confirm = make_row(popup, confirm_text, confirm_cb, NULL);
    popup_cancel = make_row(popup, cancel_text, cancel_cb, NULL);
    popup_confirm_cb = confirm_cb;
    popup_cancel_cb = cancel_cb;
    if (title_out) *title_out = title;
    if (confirm_row_out) *confirm_row_out = popup_confirm;
    if (cancel_row_out) *cancel_row_out = popup_cancel;
    if (backdrop_out) *backdrop_out = backdrop;
    return popup;
}

void gui_popup_show(gui_popup_t * popup) { popup->visible = true; popup_visible = true; }
void gui_popup_hide(gui_popup_t * popup) { popup->visible = false; popup_visible = false; }
void gui_popup_teardown(gui_popup_t * popup) {
    if (popup->popup) lv_obj_delete(popup->popup);
    if (popup->backdrop) lv_obj_delete(popup->backdrop);
    popup->popup = popup->backdrop = NULL;
    popup->visible = false;
    popup_visible = false;
    ++popup_teardown_count;
}
bool gui_popup_is_visible(const gui_popup_t * popup) { return popup && popup->visible; }

static lv_obj_t * find_row(lv_obj_t * root, const char * text) {
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(root, (int32_t)i);
        if (!child) continue;
        if (lv_obj_get_child_count(child) > 0) {
            lv_obj_t * label = lv_obj_get_child(child, 0);
            if (label && lv_obj_check_type(label, &lv_label_class) &&
                strcmp(lv_label_get_text(label), text) == 0) return child;
        }
        lv_obj_t * nested = find_row(child, text);
        if (nested) return nested;
    }
    return NULL;
}

static void click(lv_obj_t * object) {
    assert(object);
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
}

static void reset_settings(void) {
    memset(&current_settings, 0, sizeof(current_settings));
    for (int i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        current_settings.button_bindings[i].tap = BUTTON_MAPPING_ACTION_DEFAULT;
        current_settings.button_bindings[i].double_tap = BUTTON_MAPPING_ACTION_DEFAULT;
        current_settings.button_bindings[i].hold = BUTTON_MAPPING_ACTION_DEFAULT;
    }
    current_settings.button_bindings[BUTTON_MAPPING_POWER].tap = BUTTON_MAPPING_ACTION_SCREEN_TOGGLE;
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    static uint8_t display_buffer[240 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_style_init(&accent_style);
    reset_settings();
    test_build_popup();

    lv_obj_t * category = lv_obj_create(NULL);
    lv_obj_t * borrowed_summary = lv_label_create(category);
    test_set_summary(borrowed_summary);
    test_refresh();
    assert(strcmp(lv_label_get_text(borrowed_summary), "Custom") == 0);

    lv_obj_t * trigger = make_row(category, "Button Mapping", test_row_cb, NULL);
    click(trigger);
    lv_obj_t * mapping = navigation[navigation_depth - 1];
    int available = 0;
    for (int i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
#if BOARD_HAS_PREVIOUS_BUTTON
        ++available;
#else
        if (i != BUTTON_MAPPING_PREVIOUS) ++available;
#endif
    }
    assert(available + 1 == (int)(lv_obj_get_child_count(lv_obj_get_child(mapping, 1))));

    lv_obj_t * play_pause_row = find_row(mapping, "Play/Pause");
    assert(play_pause_row);
    click(play_pause_row);
    lv_obj_t * detail = navigation[navigation_depth - 1];
    lv_obj_t * tap_row = find_row(detail, "Single press");
    assert(tap_row);
    click(tap_row);
    lv_obj_t * action_screen = navigation[navigation_depth - 1];
    lv_obj_t * next_action = find_row(action_screen, "Next Track");
    assert(next_action);
    current_settings.play_pause_button_mode = 1;
    click(next_action);
    assert(current_settings.button_bindings[BUTTON_MAPPING_PLAY_PAUSE].tap == BUTTON_MAPPING_ACTION_NEXT);
    assert(current_settings.play_pause_button_mode == 0);
    assert(save_count == 1 && apply_count == 1 && toast_count == 0);
    assert(navigation_depth == 2);
    assert(strcmp(lv_label_get_text(borrowed_summary), "Custom") == 0);
    assert(strstr(lv_label_get_text(lv_obj_get_child(play_pause_row, 1)), "Single: Next Track"));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(tap_row, 1)), "Next Track") == 0);

    /* Removing the last explicit wake binding is rejected and rolled back. */
    for (int i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        current_settings.button_bindings[i].tap = BUTTON_MAPPING_ACTION_NONE;
        current_settings.button_bindings[i].double_tap = BUTTON_MAPPING_ACTION_NONE;
        current_settings.button_bindings[i].hold = BUTTON_MAPPING_ACTION_NONE;
    }
    current_settings.button_bindings[BUTTON_MAPPING_POWER].tap = BUTTON_MAPPING_ACTION_SCREEN_TOGGLE;
    click(find_row(mapping, "Power"));
    detail = navigation[navigation_depth - 1];
    click(find_row(detail, "Single press"));
    action_screen = navigation[navigation_depth - 1];
    click(find_row(action_screen, "Do Nothing"));
    assert(current_settings.button_bindings[BUTTON_MAPPING_POWER].tap == BUTTON_MAPPING_ACTION_SCREEN_TOGGLE);
    assert(save_count == 1 && apply_count == 1 && toast_count == 1);
    assert(navigation_depth == 4);

    /* Reset cancel preserves bindings; confirmation resets and applies. */
    click(find_row(mapping, "Reset All"));
    assert(popup_visible);
    assert(popup_cancel_cb != NULL && popup_confirm_cb != NULL);
    click(popup_cancel);
    assert(!popup_visible);
    assert(current_settings.button_bindings[BUTTON_MAPPING_POWER].tap == BUTTON_MAPPING_ACTION_SCREEN_TOGGLE);
    click(find_row(mapping, "Reset All"));
    current_settings.play_pause_button_mode = 1;
    click(popup_confirm);
    assert(current_settings.button_bindings[BUTTON_MAPPING_POWER].tap == BUTTON_MAPPING_ACTION_DEFAULT);
    assert(current_settings.play_pause_button_mode == 0);
    assert(save_count == 2 && apply_count == 2);
    assert(strcmp(lv_label_get_text(borrowed_summary), "Default") == 0);

    /* Module teardown must clear its borrowed category label before owner deletion. */
    test_teardown_popup();
    assert(popup_teardown_count == 1);
#if defined(BUTTON_MAPPING_SPEC_BASELINE)
    test_button_mapping_teardown_screens();
#else
    gui_button_mapping_teardown_screens();
#endif
    lv_obj_delete(category);
    test_refresh();
    puts("button mapping UI characterization: PASS");
    return 0;
}
