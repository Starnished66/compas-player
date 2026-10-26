#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "../../../library/browse/gui_list_reorder.h"
#include "../../../shell/gui_navigation.h"
#include "../../../shell/gui_reload.h"
#include "../../../shell/gui_shell.h"
#include "../../../appearance/gui_theme.h"
#include "../../../text/i18n/i18n.h"
#include "../../../widgets/builders/screen_builders.h"
#include "../../../../core/persistence/settings.h"
#include "../../../../core/themes/ui_customization.h"

#define TRACE_CAPACITY 16384
#define NAV_CAPACITY 32
#define ZONE_CAPACITY UI_DRAWER_COUNT
#define TIMER_CAPACITY 64

static char trace_log[TRACE_CAPACITY];
static size_t trace_length;
static bool trace_moves;
static bool trace_switch_state;
static bool trace_clock_options;
static int traced_move_count;
static lv_obj_t * nav_stack[NAV_CAPACITY];
static size_t nav_depth;
static lv_obj_t * zone_objects[ZONE_CAPACITY];
static int zone_ids[ZONE_CAPACITY];
static int zone_count;
static lv_obj_t * active_order_screen;
static lv_obj_t * active_quick_screen;
static lv_obj_t * active_topbar_screen;
static lv_obj_t * active_clock_screen;
static lv_timer_t * drag_timer;
static lv_obj_t * drag_handle_under_test;
static lv_obj_t * category_screens[NAV_CAPACITY];
static size_t category_screen_count;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static lv_style_t accent_style;
static lv_style_t accent_outline_style;
lv_style_t list_row_pressed_style;
player_settings_t current_settings;

static void tracef(const char * format, ...);
static void pointer_read(lv_indev_t * indev, lv_indev_data_t * data);
static lv_obj_t * shell_test_top_screen(void);
static lv_obj_t * shell_test_top_list(void);
static bool timer_registered(lv_timer_t * target);
static void shell_test_move_to_index(lv_obj_t * obj, int32_t index);
static void shell_test_add_state(lv_obj_t * obj, lv_state_t state);
static void shell_test_remove_state(lv_obj_t * obj, lv_state_t state);
static void shell_test_obj_delete(lv_obj_t * object);
static void shell_test_reorder_finish(gui_list_reorder_state_t * state, bool commit);

#define lv_obj_move_to_index shell_test_move_to_index
#define lv_obj_add_state shell_test_add_state
#define lv_obj_remove_state shell_test_remove_state
#define lv_obj_delete shell_test_obj_delete
#define gui_list_reorder_finish shell_test_reorder_finish
#if defined(SHELL_SETTINGS_SPEC_FINAL)
#include "../gui_shell_settings.c"
#define shell_settings_test_quick_drawer_row_cb gui_shell_settings_quick_drawer_row_cb
#define shell_settings_test_music_order_row_cb gui_shell_settings_music_order_row_cb
#define shell_settings_test_topbar_row_cb gui_shell_settings_topbar_row_cb
#else
#include "shell_settings_baseline_fixture.c"
#endif
#undef gui_list_reorder_finish
#undef lv_obj_delete
#undef lv_obj_remove_state
#undef lv_obj_add_state
#undef lv_obj_move_to_index

static void tracef(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(trace_log + trace_length, TRACE_CAPACITY - trace_length,
                            format, args);
    va_end(args);
    assert(written >= 0 && (size_t)written < TRACE_CAPACITY - trace_length);
    trace_length += (size_t)written;
}

static void trace_clear(void) {
    trace_length = 0;
    trace_log[0] = '\0';
}

static void pointer_read(lv_indev_t * indev, lv_indev_data_t * data) {
    (void)indev;
    data->point = pointer_point;
    data->state = pointer_state;
}

static bool timer_registered(lv_timer_t * target) {
    if (!target) return false;
    for (lv_timer_t * timer = lv_timer_get_next(NULL); timer;
         timer = lv_timer_get_next(timer))
        if (timer == target) return true;
    return false;
}

static void timer_snapshot(lv_timer_t ** out, int * count) {
    *count = 0;
    for (lv_timer_t * timer = lv_timer_get_next(NULL); timer && *count < TIMER_CAPACITY;
         timer = lv_timer_get_next(timer)) out[(*count)++] = timer;
}

static bool timer_was_in(lv_timer_t * timer, lv_timer_t ** before, int count) {
    for (int i = 0; i < count; ++i) if (before[i] == timer) return true;
    return false;
}

static void shell_test_move_to_index(lv_obj_t * obj, int32_t index) {
    if (trace_moves && traced_move_count % UI_DRAWER_COUNT == 0)
        tracef("repopulate\n");
    if (trace_moves) ++traced_move_count;
    lv_obj_move_to_index(obj, index);
}

static void shell_test_add_state(lv_obj_t * obj, lv_state_t state) {
    if (trace_switch_state) tracef("row-state:on\n");
    lv_obj_add_state(obj, state);
}

static void shell_test_remove_state(lv_obj_t * obj, lv_state_t state) {
    if (trace_switch_state) tracef("row-state:off\n");
    lv_obj_remove_state(obj, state);
}

static int zone_index(lv_obj_t * object) {
    for (int i = 0; i < zone_count; ++i) if (zone_objects[i] == object) return i;
    return -1;
}

static void shell_test_obj_delete(lv_obj_t * object) {
    if (object == active_order_screen) {
        assert(zone_count == 0);
        assert(drag_timer == NULL || !timer_registered(drag_timer));
        tracef("delete:order\n");
        active_order_screen = NULL;
    } else if (object == active_quick_screen) {
        tracef("delete:quick\n");
        active_quick_screen = NULL;
    } else if (object == active_topbar_screen) {
        tracef("delete:topbar\n");
        active_topbar_screen = NULL;
    } else if (object == active_clock_screen) {
        tracef("delete:clock-position\n");
        active_clock_screen = NULL;
    }
    lv_obj_delete(object);
}

static void shell_test_reorder_finish(gui_list_reorder_state_t * state, bool commit) {
    if (state && state->active) tracef("finish:%s\n", commit ? "commit" : "cancel");
    gui_list_reorder_finish(state, commit);
    if (state && state->row_capacity == UI_ORDER_MAX && !state->active && state->timer == NULL)
        drag_timer = NULL;
}

static lv_obj_t * shell_test_top_screen(void) {
    assert(nav_depth > 0);
    return nav_stack[nav_depth - 1];
}

static lv_obj_t * shell_test_top_list(void) {
    lv_obj_t * screen = shell_test_top_screen();
    return lv_obj_get_child(screen, 1);
}

static lv_obj_t * find_label_object(lv_obj_t * root, const char * text) {
    if (!root) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) &&
        strcmp(lv_label_get_text(root), text) == 0) return root;
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t * found = find_label_object(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t * find_row(lv_obj_t * screen, const char * label) {
    lv_obj_t * text = find_label_object(screen, label);
    return text ? lv_obj_get_parent(text) : NULL;
}

static lv_obj_t * row_switch(lv_obj_t * row) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t * child = lv_obj_get_child(row, i);
        if (lv_obj_check_type(child, &lv_switch_class)) return child;
    }
    return NULL;
}

static void click(lv_obj_t * object) {
    assert(object);
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t * category_row(lv_event_cb_t cb, void * user_data) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 240, 320);
    assert(category_screen_count < NAV_CAPACITY);
    category_screens[category_screen_count++] = screen;
    lv_obj_t * row = add_pill_row_base(screen, "test category entry");
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    nav_push(screen);
    return row;
}

static void test_open_quick_drawer(void) {
    lv_obj_t * row = category_row(shell_settings_test_quick_drawer_row_cb, NULL);
    click(row);
    active_quick_screen = shell_test_top_screen();
}

static void open_music_order(void) {
    lv_obj_t * row = category_row(shell_settings_test_music_order_row_cb,
                                  (void *)(intptr_t)UI_ORDER_MUSIC);
    click(row);
    active_order_screen = shell_test_top_screen();
}

static void open_topbar(void) {
    lv_obj_t * row = category_row(shell_settings_test_topbar_row_cb, NULL);
    click(row);
    active_topbar_screen = shell_test_top_screen();
}

void register_swipe_dead_zone(lv_obj_t * object) {
    assert(object && zone_count < ZONE_CAPACITY);
    assert(lv_obj_check_type(object, &lv_button_class));
    assert(zone_index(object) < 0);
    zone_objects[zone_count] = object;
    zone_ids[zone_count] = zone_count;
    tracef("zone+:handle-%d\n", zone_count);
    ++zone_count;
}

void unregister_swipe_dead_zone(lv_obj_t * object) {
    int at = zone_index(object);
    assert(at >= 0);
    assert(drag_timer == NULL || !timer_registered(drag_timer));
    assert(active_order_screen && lv_obj_is_valid(active_order_screen));
    assert(lv_obj_get_parent(object));
    tracef("zone-:handle-%d\n", zone_ids[at]);
    for (int i = at + 1; i < zone_count; ++i) {
        zone_objects[i - 1] = zone_objects[i];
        zone_ids[i - 1] = zone_ids[i];
    }
    zone_objects[--zone_count] = NULL;
}

void nav_push(lv_obj_t * screen) {
    assert(nav_depth < NAV_CAPACITY);
    nav_stack[nav_depth++] = screen;
    lv_screen_load(screen);
    tracef("push\n");
}

void nav_pop(void) {
    if (nav_depth) --nav_depth;
    tracef("pop\n");
}

void generic_back_cb(lv_event_t * event) { (void)event; }
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
void enable_gesture_bubble_recursive(lv_obj_t * object) { (void)object; }

void settings_save_async(const player_settings_t * settings) {
    assert(settings == &current_settings);
    tracef("save\n");
}

const char * i18n_tr(const char * text) { return text; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }

static bool test_font_get_glyph(const lv_font_t * font, lv_font_glyph_dsc_t * dsc,
                                uint32_t codepoint, uint32_t next_codepoint) {
    (void)next_codepoint;
    if (codepoint == 0) return false;
    memset(dsc, 0, sizeof(*dsc));
    dsc->resolved_font = font;
    dsc->adv_w = 8;
    return true;
}

static const void * test_font_get_bitmap(lv_font_glyph_dsc_t * dsc,
                                         lv_draw_buf_t * draw_buf) {
    (void)dsc;
    (void)draw_buf;
    return NULL;
}

void gui_shell_apply_customization(void) { tracef("apply:drawer\n"); }
void gui_shell_refresh_quick_drawer_volume_visibility(void) { tracef("refresh:drawer-volume\n"); }
void gui_shell_apply_topbar_settings(void) { tracef("apply:topbar\n"); }
void gui_theme_refresh_request(void) { tracef("apply:music-theme\n"); }

lv_obj_t * build_subsonic_list_screen(const char * title, lv_obj_t ** out_title,
                                      lv_obj_t ** out_list) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 240, 320);
    lv_obj_t * title_label = lv_label_create(screen);
    lv_label_set_text(title_label, title);
    lv_obj_set_pos(title_label, 4, 4);
    lv_obj_t * list = lv_obj_create(screen);
    lv_obj_set_pos(list, 0, 40);
    lv_obj_set_size(list, 240, 276);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    *out_title = title_label;
    *out_list = list;
    return screen;
}

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 240, 54);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, 160);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    return row;
}

lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text,
                               bool checked, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    lv_obj_t * toggle = lv_switch_create(row);
    lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    if (checked) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text,
                                lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * label_text,
                               bool selected, lv_event_cb_t on_click, void * user_data) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    lv_obj_set_user_data(row, (void *)(intptr_t)selected);
    if (selected) lv_obj_add_style(row, &accent_outline_style, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, user_data);
    if (trace_clock_options) tracef("clock-option:%s\n", label_text);
    return row;
}

lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 220);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    return label;
}

lv_style_t * gui_theme_accent_style(void) { return &accent_style; }
lv_style_t * gui_theme_accent_outline_style(void) { return &accent_outline_style; }
int32_t ui_list_row_height(void) { return 54; }
int32_t ui_list_row_width(void) { return 240; }
void set_reorder_handle_icon(lv_obj_t * object) { (void)object; }

static lv_obj_t * new_event_row(lv_obj_t * parent, const char * label,
                                lv_event_cb_t cb, void * user_data) {
    lv_obj_t * row = add_pill_row_base(parent, label);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void pointer_drag_start(lv_indev_t * indev, lv_obj_t * screen, int physical_row) {
    lv_obj_t * list = lv_obj_get_child(screen, 1);
    lv_obj_t * row = lv_obj_get_child(list, physical_row);
    assert(row);
    drag_handle_under_test = lv_obj_get_child(row, 1);
    assert(drag_handle_under_test && lv_obj_check_type(drag_handle_under_test, &lv_button_class));
    lv_obj_update_layout(screen);
    lv_area_t area;
    lv_obj_get_coords(drag_handle_under_test, &area);
    pointer_point.x = (area.x1 + area.x2) / 2;
    pointer_point.y = (area.y1 + area.y2) / 2;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    lv_tick_inc(20);
    lv_indev_read(indev);
    assert(ui_order_drag_state.active);
    assert(ui_order_drag_state.indev == indev);
    assert(ui_order_drag_state.timer);
    drag_timer = ui_order_drag_state.timer;
    assert(timer_registered(drag_timer));
}

static void pointer_drag_move(lv_indev_t * indev, lv_obj_t * screen, int target_physical_row) {
    lv_obj_t * list = lv_obj_get_child(screen, 1);
    lv_obj_t * target = lv_obj_get_child(list, target_physical_row);
    assert(drag_handle_under_test && target);
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    int32_t offset = 16;
    if (ui_order_drag_state.active && ui_order_drag_state.drag_id >= 0 &&
        ui_order_drag_state.drag_id < ui_order_drag_state.row_capacity &&
        ui_order_drag_state.rows[ui_order_drag_state.drag_id] &&
        lv_obj_get_index(ui_order_drag_state.rows[ui_order_drag_state.drag_id]) >
            lv_obj_get_index(target)) offset = -48;
    pointer_point.y = (area.y1 + area.y2) / 2 + offset;
    lv_indev_read(indev);
    lv_obj_send_event(drag_handle_under_test, LV_EVENT_PRESSING, NULL);
}

static void pointer_drag_release(lv_indev_t * indev) {
    assert(drag_handle_under_test);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    lv_obj_send_event(drag_handle_under_test, LV_EVENT_RELEASED, NULL);
    assert(!timer_registered(drag_timer));
    assert(!ui_order_drag_state.active);
    drag_timer = NULL;
    drag_handle_under_test = NULL;
}

static void settle_order_animation(void) {
    lv_tick_inc(200);
    lv_timer_handler();
}

static void assert_trace(const char * expected) {
    if (strcmp(trace_log, expected) != 0) {
        fprintf(stderr, "expected trace:\n%sactual trace:\n%s", expected, trace_log);
        abort();
    }
}

static void save_order(const ui_item_order_t *order, uint8_t ids[UI_ORDER_MAX],
                       unsigned *count) {
    assert(order && order->count <= UI_ORDER_MAX);
    *count = order->count;
    memcpy(ids, order->ids, UI_ORDER_MAX);
}

static void assert_order_is_swapped_prefix(const ui_item_order_t *order,
                                           const uint8_t before[UI_ORDER_MAX],
                                           unsigned count) {
    assert(order->count == count);
    assert(count >= 2);
    assert(order->ids[0] == before[1] && order->ids[1] == before[0]);
    for (unsigned i = 2; i < count; ++i) assert(order->ids[i] == before[i]);
}

static void assert_order_equals(const ui_item_order_t *order,
                                const uint8_t expected[UI_ORDER_MAX],
                                unsigned count) {
    assert(order->count == count);
    for (unsigned i = 0; i < count; ++i) {
        if (order->ids[i] != expected[i])
            fprintf(stderr, "order mismatch at %u: got %u expected %u\n", i,
                    order->ids[i], expected[i]);
        assert(order->ids[i] == expected[i]);
    }
}

static void assert_clock_selection(lv_obj_t *screen, const char *selected) {
    static const char *const labels[] = { "Left", "Center", "Right" };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        lv_obj_t *row = find_row(screen, labels[i]);
        assert(row);
        assert((intptr_t)lv_obj_get_user_data(row) == (strcmp(labels[i], selected) == 0));
    }
}

int main(void) {
    lv_init();
    lv_font_montserrat_16.get_glyph_dsc = test_font_get_glyph;
    lv_font_montserrat_16.get_glyph_bitmap = test_font_get_bitmap;
    lv_font_montserrat_16.line_height = 16;
    lv_font_montserrat_16.base_line = 4;
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    static uint8_t display_buffer[240 * 12 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_style_init(&accent_style);
    lv_style_init(&accent_outline_style);
    lv_style_init(&list_row_pressed_style);
    ui_customization_defaults(&current_settings.ui_customization);
    current_settings.quick_drawer_volume_visible = true;
    current_settings.topbar_hidden_elements = 0;
    current_settings.show_battery_percent = false;
    current_settings.topbar_clock_position = TOPBAR_CLOCK_CENTER;

    lv_indev_t * pointer_indev = lv_indev_create();
    assert(pointer_indev);
    lv_indev_set_type(pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_indev, display);
    lv_indev_set_read_cb(pointer_indev, pointer_read);
    lv_indev_set_long_press_time(pointer_indev, 10);

    /* Build Music menu order through its real entry callback and commit a
     * real LVGL long-press reorder. Its side effect routes through themes. */
    open_music_order();
    assert(zone_count == UI_DRAWER_COUNT);
    uint8_t music_order_before[UI_ORDER_MAX];
    unsigned music_order_count;
    save_order(&current_settings.ui_customization.music, music_order_before, &music_order_count);
    trace_clear();
    pointer_drag_start(pointer_indev, active_order_screen, 0);
    pointer_drag_move(pointer_indev, active_order_screen, 1);
    pointer_drag_release(pointer_indev);
    assert(current_settings.ui_customization.music.ids[0] == 1);
    assert_order_is_swapped_prefix(&current_settings.ui_customization.music,
                                   music_order_before, music_order_count);
    assert_trace("save\napply:music-theme\n");
    tracef("music-order:%u,%u\n", current_settings.ui_customization.music.ids[0],
           current_settings.ui_customization.music.ids[1]);
    printf("music-effects:\n%s", trace_log);

    /* Drag the Music order back. Every untouched entry must retain its slot. */
    settle_order_animation();
    trace_clear();
    pointer_drag_start(pointer_indev, active_order_screen, 1);
    pointer_drag_move(pointer_indev, active_order_screen, 0);
    pointer_drag_release(pointer_indev);
    assert_order_equals(&current_settings.ui_customization.music,
                        music_order_before, music_order_count);
    assert_trace("save\napply:music-theme\n");

    /* Open Quick Drawer settings and enter its nested order editor. A
     * visibility toggle during a real active drag cancels first, then saves,
     * repopulates, and applies the drawer update. */
    test_open_quick_drawer();
    lv_obj_t * reorder_row = find_row(active_quick_screen, "Reorder Items");
    assert(reorder_row);
    click(reorder_row);
    active_order_screen = shell_test_top_screen();
    assert(zone_count == UI_DRAWER_COUNT);
    lv_obj_t * order_list = lv_obj_get_child(active_order_screen, 1);
    lv_obj_t * visible_row = lv_obj_get_child(order_list, 2);
    lv_obj_t * visible_switch = lv_obj_get_child(visible_row, 2);
    assert(visible_switch && lv_obj_check_type(visible_switch, &lv_switch_class));
    bool was_visible = (current_settings.ui_customization.drawer.ids[2] == 2);
    assert(was_visible);
    uint8_t drawer_order_before[UI_ORDER_MAX];
    unsigned drawer_order_count;
    save_order(&current_settings.ui_customization.drawer, drawer_order_before, &drawer_order_count);
    pointer_drag_start(pointer_indev, active_order_screen, 0);
    pointer_drag_move(pointer_indev, active_order_screen, 1);
    lv_obj_clear_state(visible_switch, LV_STATE_CHECKED);
    trace_clear();
    trace_moves = true;
    traced_move_count = 0;
    lv_obj_send_event(visible_switch, LV_EVENT_VALUE_CHANGED, NULL);
    trace_moves = false;
    assert(!timer_registered(drag_timer));
    assert(!ui_order_drag_state.active);
    drag_timer = NULL;
    drag_handle_under_test = NULL;
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer_indev);
    assert(current_settings.ui_customization.drawer.ids[2] != 2);
    assert_trace("finish:cancel\nrepopulate\nsave\nrepopulate\napply:drawer\n");
    printf("drawer-toggle-effects:\n%s", trace_log);

    /* Restore the hidden entry. All prior entries keep their relative order;
     * the re-enabled item is appended by the production callback. */
    trace_clear();
    lv_obj_add_state(visible_switch, LV_STATE_CHECKED);
    lv_obj_send_event(visible_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert_trace("save\napply:drawer\n");
    assert(current_settings.ui_customization.drawer.count == drawer_order_count);
    unsigned restore_at = 0;
    for (unsigned i = 0; i < drawer_order_count; ++i) {
        if (drawer_order_before[i] == 2) continue;
        assert(current_settings.ui_customization.drawer.ids[restore_at++] == drawer_order_before[i]);
    }
    assert(current_settings.ui_customization.drawer.ids[restore_at] == 2);
    assert(lv_obj_has_state(visible_switch, LV_STATE_CHECKED));

    /* Commit a changed drawer order and verify save-before-shell-apply. */
    trace_clear();
    uint8_t drawer_before_drag[UI_ORDER_MAX];
    unsigned drawer_before_drag_count;
    save_order(&current_settings.ui_customization.drawer, drawer_before_drag, &drawer_before_drag_count);
    pointer_drag_start(pointer_indev, active_order_screen, 0);
    pointer_drag_move(pointer_indev, active_order_screen, 1);
    pointer_drag_release(pointer_indev);
    assert_trace("save\napply:drawer\n");
    assert_order_is_swapped_prefix(&current_settings.ui_customization.drawer,
                                   drawer_before_drag, drawer_before_drag_count);
    tracef("drawer-order:%u,%u\n", current_settings.ui_customization.drawer.ids[0],
           current_settings.ui_customization.drawer.ids[1]);
    printf("drawer-order-effects:\n%s", trace_log);

    settle_order_animation();
    trace_clear();
    pointer_drag_start(pointer_indev, active_order_screen, 1);
    pointer_drag_move(pointer_indev, active_order_screen, 0);
    pointer_drag_release(pointer_indev);
    assert_order_equals(&current_settings.ui_customization.drawer,
                        drawer_before_drag, drawer_before_drag_count);
    assert_trace("save\napply:drawer\n");

    /* Quick Drawer controls update their row switch before applying shell
     * customization; the Volume Slider uses the dedicated refresh route. */
    test_open_quick_drawer();
    lv_obj_t * enabled_row = find_row(active_quick_screen, "Enabled");
    lv_obj_t * equalizer_row = find_row(active_quick_screen, "Equalizer");
    lv_obj_t * volume_row = find_row(active_quick_screen, "Volume Slider");
    assert(enabled_row && equalizer_row && volume_row);
    trace_clear(); trace_switch_state = true;
    click(enabled_row); trace_switch_state = false;
    assert(!current_settings.ui_customization.drawer_enabled);
    assert(current_settings.ui_customization.drawer_eq_visible);
    assert(current_settings.quick_drawer_volume_visible);
    assert(!lv_obj_has_state(row_switch(enabled_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:off\napply:drawer\n");
    printf("quick-enabled-effects:\n%s", trace_log);
    trace_clear(); trace_switch_state = true;
    click(enabled_row); trace_switch_state = false;
    assert(current_settings.ui_customization.drawer_enabled);
    assert(current_settings.ui_customization.drawer_eq_visible);
    assert(current_settings.quick_drawer_volume_visible);
    assert(lv_obj_has_state(row_switch(enabled_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:on\napply:drawer\n");
    trace_clear(); trace_switch_state = true;
    click(equalizer_row); trace_switch_state = false;
    assert(!current_settings.ui_customization.drawer_eq_visible);
    assert(current_settings.ui_customization.drawer_enabled);
    assert(current_settings.quick_drawer_volume_visible);
    assert(!lv_obj_has_state(row_switch(equalizer_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:off\napply:drawer\n");
    printf("quick-eq-effects:\n%s", trace_log);
    trace_clear(); trace_switch_state = true;
    click(equalizer_row); trace_switch_state = false;
    assert(current_settings.ui_customization.drawer_eq_visible);
    assert(current_settings.ui_customization.drawer_enabled);
    assert(current_settings.quick_drawer_volume_visible);
    assert(lv_obj_has_state(row_switch(equalizer_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:on\napply:drawer\n");
    trace_clear(); trace_switch_state = true;
    click(volume_row); trace_switch_state = false;
    assert(!current_settings.quick_drawer_volume_visible);
    assert(current_settings.ui_customization.drawer_enabled);
    assert(current_settings.ui_customization.drawer_eq_visible);
    assert(!lv_obj_has_state(row_switch(volume_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:off\nrefresh:drawer-volume\n");
    printf("quick-volume-effects:\n%s", trace_log);
    trace_clear(); trace_switch_state = true;
    click(volume_row); trace_switch_state = false;
    assert(current_settings.quick_drawer_volume_visible);
    assert(current_settings.ui_customization.drawer_enabled);
    assert(current_settings.ui_customization.drawer_eq_visible);
    assert(lv_obj_has_state(row_switch(volume_row), LV_STATE_CHECKED));
    assert_trace("save\nrow-state:on\nrefresh:drawer-volume\n");

    /* Top Bar visibility and battery percentage save before apply, then
     * synchronize the non-clickable switch in the clicked row. */
    open_topbar();
    lv_obj_t * wifi_row = find_row(active_topbar_screen, "Wi-Fi");
    lv_obj_t * battery_row = find_row(active_topbar_screen, "Battery Percentage");
    assert(wifi_row && battery_row);
    current_settings.topbar_hidden_elements |= TOPBAR_HIDE_BLUETOOTH;
    trace_clear(); trace_switch_state = true;
    click(wifi_row); trace_switch_state = false;
    assert(current_settings.topbar_hidden_elements & TOPBAR_HIDE_WIFI);
    assert(current_settings.topbar_hidden_elements & TOPBAR_HIDE_BLUETOOTH);
    assert_trace("save\napply:topbar\nrow-state:off\n");
    printf("topbar-wifi-effects:\n%s", trace_log);
    trace_clear(); trace_switch_state = true;
    click(wifi_row); trace_switch_state = false;
    assert(!(current_settings.topbar_hidden_elements & TOPBAR_HIDE_WIFI));
    assert(current_settings.topbar_hidden_elements & TOPBAR_HIDE_BLUETOOTH);
    assert(lv_obj_has_state(row_switch(wifi_row), LV_STATE_CHECKED));
    assert_trace("save\napply:topbar\nrow-state:on\n");
    uint32_t hidden_before_battery = current_settings.topbar_hidden_elements;
    trace_clear(); trace_switch_state = true;
    click(battery_row); trace_switch_state = false;
    assert(current_settings.show_battery_percent);
    assert(current_settings.topbar_hidden_elements == hidden_before_battery);
    assert(lv_obj_has_state(row_switch(battery_row), LV_STATE_CHECKED));
    assert_trace("save\napply:topbar\nrow-state:on\n");
    printf("topbar-battery-effects:\n%s", trace_log);
    trace_clear(); trace_switch_state = true;
    click(battery_row); trace_switch_state = false;
    assert(!current_settings.show_battery_percent);
    assert(current_settings.topbar_hidden_elements == hidden_before_battery);
    assert(!lv_obj_has_state(row_switch(battery_row), LV_STATE_CHECKED));
    assert_trace("save\napply:topbar\nrow-state:off\n");

    /* Clock-position selection applies immediately and leaves list rebuild
     * queued through LVGL async processing. */
    lv_obj_t * position_row = find_row(active_topbar_screen, "Clock Position");
    assert(position_row);
    click(position_row);
    active_clock_screen = shell_test_top_screen();
    lv_obj_t * left_row = find_row(active_clock_screen, "Left");
    assert(left_row);
    trace_clear(); trace_clock_options = true;
    click(left_row);
    assert(current_settings.topbar_clock_position == TOPBAR_CLOCK_LEFT);
    assert(strcmp(trace_log, "save\napply:topbar\n") == 0);
    lv_timer_handler();
    trace_clock_options = false;
    assert(strcmp(trace_log,
        "save\napply:topbar\nclock-option:Left\nclock-option:Center\nclock-option:Right\n") == 0);
    assert_clock_selection(active_clock_screen, "Left");
    printf("clock-position-effects:\n%s", trace_log);
    lv_obj_t * center_row = find_row(active_clock_screen, "Center");
    assert(center_row);
    trace_clear(); trace_clock_options = true;
    click(center_row);
    assert(current_settings.topbar_clock_position == TOPBAR_CLOCK_CENTER);
    assert(strcmp(trace_log, "save\napply:topbar\n") == 0);
    lv_timer_handler();
    trace_clock_options = false;
    assert(strcmp(trace_log,
        "save\napply:topbar\nclock-option:Left\nclock-option:Center\nclock-option:Right\n") == 0);
    assert_clock_selection(active_clock_screen, "Center");

    lv_obj_t *right_row = find_row(active_clock_screen, "Right");
    assert(right_row);
    trace_clear(); trace_clock_options = true;
    click(right_row);
    assert(current_settings.topbar_clock_position == TOPBAR_CLOCK_RIGHT);
    assert(strcmp(trace_log, "save\napply:topbar\n") == 0);
    lv_timer_handler();
    trace_clock_options = false;
    assert(strcmp(trace_log,
        "save\napply:topbar\nclock-option:Left\nclock-option:Center\nclock-option:Right\n") == 0);
    assert_clock_selection(active_clock_screen, "Right");

    center_row = find_row(active_clock_screen, "Center");
    assert(center_row);
    trace_clear(); trace_clock_options = true;
    click(center_row);
    assert(current_settings.topbar_clock_position == TOPBAR_CLOCK_CENTER);
    assert(strcmp(trace_log, "save\napply:topbar\n") == 0);
    lv_timer_handler();
    trace_clock_options = false;
    assert(strcmp(trace_log,
        "save\napply:topbar\nclock-option:Left\nclock-option:Center\nclock-option:Right\n") == 0);
    assert_clock_selection(active_clock_screen, "Center");

    /* Keep module teardown split around the independent plugin editor. Start
     * one more actual pointer-owned drag to prove cancel precedes exact-handle
     * unregister and screen deletion. */
    open_music_order();
    active_order_screen = shell_test_top_screen();
    pointer_drag_start(pointer_indev, active_order_screen, 0);
    pointer_state = LV_INDEV_STATE_RELEASED; /* teardown cancels before LVGL reads another point */
    trace_clear();
    gui_shell_settings_teardown_order();
    assert(zone_count == 0);
    tracef("plugin-order-teardown\n");
    gui_shell_settings_teardown_after_plugin_order();
    assert(active_order_screen == NULL);
    assert(active_quick_screen == NULL);
    assert(active_topbar_screen == NULL);
    assert(active_clock_screen == NULL);
    assert(strcmp(trace_log,
        "finish:cancel\n"
        "zone-:handle-0\nzone-:handle-1\nzone-:handle-2\nzone-:handle-3\n"
        "zone-:handle-4\nzone-:handle-5\nzone-:handle-6\nzone-:handle-7\n"
        "zone-:handle-8\nzone-:handle-9\n"
        "delete:order\nplugin-order-teardown\n"
        "delete:quick\ndelete:topbar\ndelete:clock-position\n") == 0);
    printf("teardown-effects:\n%s", trace_log);

    lv_indev_delete(pointer_indev);
    for (size_t i = 0; i < category_screen_count; ++i)
        if (lv_obj_is_valid(category_screens[i])) lv_obj_delete(category_screens[i]);
    lv_style_reset(&list_row_pressed_style);
    lv_style_reset(&accent_outline_style);
    lv_style_reset(&accent_style);
    lv_display_delete(display);
    puts("GUI Shell Settings characterization passed");
    return 0;
}
