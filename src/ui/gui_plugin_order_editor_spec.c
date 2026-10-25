#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>

#include "gui_list_reorder.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "i18n.h"
#include "plugin_manager.h"
#include "screen_builders.h"

#define TRACE_CAPACITY 16384
static char trace_log[TRACE_CAPACITY];
static size_t trace_length;
static lv_obj_t * nav_stack[8];
static size_t nav_depth;
static lv_obj_t * zone_objects[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
static int zone_count;
static int change_count;
static int close_count;
static lv_style_t accent_style;
lv_style_t list_row_pressed_style;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;

static void tracef(const char * format, ...);

static void pointer_read(lv_indev_t * indev, lv_indev_data_t * data) {
    (void)indev;
    data->point = pointer_point;
    data->state = pointer_state;
}

void editor_test_lv_obj_clean(lv_obj_t * object);
void editor_test_lv_obj_delete(lv_obj_t * object);
void editor_test_reorder_finish(gui_list_reorder_state_t * state, bool commit);

#define lv_obj_clean editor_test_lv_obj_clean
#define lv_obj_delete editor_test_lv_obj_delete
#define gui_list_reorder_finish editor_test_reorder_finish

#if defined(EDITOR_BASELINE)
#include "baseline_fixture.c"
#else
#include "gui_plugin_order_editor.c"
#endif

#undef gui_list_reorder_finish
#undef lv_obj_delete
#undef lv_obj_clean

static void tracef(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(trace_log + trace_length, TRACE_CAPACITY - trace_length,
                            format, args);
    va_end(args);
    assert(written >= 0 && (size_t)written < TRACE_CAPACITY - trace_length);
    trace_length += (size_t)written;
}

static void assert_all_editor_handles_unregistered(void) {
    assert(zone_count == 0);
    for (int i = 0; i < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++i)
        assert(zone_objects[i] == NULL);
}

void editor_test_lv_obj_clean(lv_obj_t * object) {
    assert(object == plugin_order_editor_list);
    assert(!plugin_order_editor_drag_state.active);
    assert(zone_count == 0);
    tracef("clean\n");
    lv_obj_clean(object);
}

void editor_test_lv_obj_delete(lv_obj_t * object) {
    if (object == plugin_order_editor_screen) {
        assert(!plugin_order_editor_drag_state.active);
        assert_all_editor_handles_unregistered();
        tracef("delete-screen\n");
    }
    lv_obj_delete(object);
}

void editor_test_reorder_finish(gui_list_reorder_state_t * state, bool commit) {
    if (state == &plugin_order_editor_drag_state)
        tracef("finish:%s\n", commit ? "commit" : "cancel");
    gui_list_reorder_finish(state, commit);
}

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

lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text) {
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_set_size(row, 240, 48);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 120);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    return row;
}

void set_reorder_handle_icon(lv_obj_t * grip) {
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x555555), 0);
}

int32_t ui_list_row_height(void) { return 48; }
int32_t ui_list_row_width(void) { return 240; }
lv_style_t * gui_theme_accent_style(void) { return &accent_style; }
void enable_gesture_bubble_recursive(lv_obj_t * object) { (void)object; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
const char * i18n_tr(const char * text) { return text; }

void register_swipe_dead_zone(lv_obj_t * object) {
    assert(object != NULL);
    for (int i = 0; i < zone_count; ++i) assert(zone_objects[i] != object);
    assert(zone_count < PLUGIN_ORDER_EDITOR_MAX_ITEMS);
    zone_objects[zone_count++] = object;
    tracef("zone+\n");
}

void unregister_swipe_dead_zone(lv_obj_t * object) {
    assert(!plugin_order_editor_drag_state.active);
    int at = -1;
    for (int i = 0; i < zone_count; ++i)
        if (zone_objects[i] == object) { at = i; break; }
    assert(at >= 0);
    memmove(&zone_objects[at], &zone_objects[at + 1],
            (size_t)(zone_count - at - 1) * sizeof(zone_objects[0]));
    zone_objects[--zone_count] = NULL;
    tracef("zone-\n");
}

void nav_push(lv_obj_t * screen) {
    assert(nav_depth < sizeof(nav_stack) / sizeof(nav_stack[0]));
    nav_stack[nav_depth++] = screen;
    lv_screen_load(screen);
    tracef("push\n");
}

bool gui_navigation_contains(lv_obj_t * screen) {
    for (size_t i = 0; i < nav_depth; ++i)
        if (nav_stack[i] == screen) return true;
    return false;
}

static void remove_from_navigation(lv_obj_t * screen) {
    for (size_t i = 0; i < nav_depth; ++i) {
        if (nav_stack[i] == screen) {
            memmove(&nav_stack[i], &nav_stack[i + 1],
                    (nav_depth - i - 1) * sizeof(nav_stack[0]));
            --nav_depth;
            return;
        }
    }
}

void plugin_manager_order_editor_changed(int slot, const char * const * keys,
                                         const char * const * labels,
                                         const bool * visible, int count) {
    assert(slot == 0 && count > 0 && count <= PLUGIN_ORDER_EDITOR_MAX_ITEMS);
    ++change_count;
    if (change_count <= 3) {
        assert(count == 3);
        assert(strcmp(keys[0], "gamma") == 0);
        assert(strlen(keys[1]) == 63 && keys[1][0] == 'k');
        assert(strcmp(keys[2], "beta") == 0);
        assert(strcmp(labels[0], "Gamma") == 0);
        assert(strlen(labels[1]) == 127 && labels[1][0] == 'l');
        assert(strcmp(labels[2], "Beta") == 0);
        assert(visible[0] && visible[1]);
        assert(visible[2] == (change_count == 3));
    }
    tracef("change:%d", count);
    for (int i = 0; i < count; ++i)
        tracef("|%s|%s|%d", keys[i], labels[i], visible[i] ? 1 : 0);
    tracef("\n");
}

void plugin_manager_order_editor_closed(int slot) {
    assert(slot == 0);
    ++close_count;
    tracef("closed\n");
}

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static bool test_font_get_glyph(const lv_font_t * font, lv_font_glyph_dsc_t * descriptor,
                                uint32_t codepoint, uint32_t next_codepoint) {
    (void)next_codepoint;
    if (codepoint == 0) return false;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->resolved_font = font;
    descriptor->adv_w = 8;
    return true;
}

static const void * test_font_get_bitmap(lv_font_glyph_dsc_t * descriptor,
                                         lv_draw_buf_t * draw_buffer) {
    (void)descriptor;
    (void)draw_buffer;
    return NULL;
}

static int live_editor_rows(void) {
    int count = 0;
    for (int i = 0; i < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++i)
        if (plugin_order_editor_rows[i]) ++count;
    return count;
}

static bool timer_is_registered(lv_timer_t * target) {
    for (lv_timer_t * timer = lv_timer_get_next(NULL); timer;
         timer = lv_timer_get_next(timer)) {
        if (timer == target) return true;
    }
    return false;
}

static void check_all_state_cleared(void) {
    assert(plugin_order_editor_screen == NULL);
    assert(plugin_order_editor_title == NULL);
    assert(plugin_order_editor_list == NULL);
    assert(plugin_order_editor_count == 0);
    assert(plugin_order_editor_drag_state.drag_id == -1);
    assert(!plugin_order_editor_drag_state.active);
    for (int i = 0; i < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++i) {
        assert(plugin_order_editor_rows[i] == NULL);
        assert(plugin_order_editor_handles[i] == NULL);
        assert(plugin_order_editor_switches[i] == NULL);
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
    static uint8_t display_buffer[240 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_style_init(&accent_style);
    lv_style_init(&list_row_pressed_style);

    const char * keys[] = { "alpha", "beta", "gamma" };
    const char * labels[] = { "Alpha", "Beta", "Gamma" };
    const bool visible[] = { true, false, true };
    assert(gui_plugin_show_order_editor("Bad", NULL, labels, visible, 3) == -1);
    assert(gui_plugin_show_order_editor("Bad", keys, NULL, visible, 3) == -1);
    assert(gui_plugin_show_order_editor("Bad", keys, labels, NULL, 3) == -1);
    assert(gui_plugin_show_order_editor("Bad", keys, labels, visible, 0) == -1);
    assert(gui_plugin_show_order_editor("Bad", keys, labels, visible, -1) == -1);
    assert(gui_plugin_show_order_editor("Bad", keys, labels, visible,
                                        PLUGIN_ORDER_EDITOR_MAX_ITEMS + 1) == -1);

    char long_key[96], long_label[180];
    memset(long_key, 'k', sizeof(long_key) - 1); long_key[sizeof(long_key) - 1] = '\0';
    memset(long_label, 'l', sizeof(long_label) - 1); long_label[sizeof(long_label) - 1] = '\0';
    const char * copied_keys[] = { long_key, "beta", "gamma" };
    const char * copied_labels[] = { long_label, "Beta", "Gamma" };
    assert(gui_plugin_show_order_editor("", copied_keys, copied_labels, visible, 3) == 0);
    lv_obj_t * original_screen = plugin_order_editor_screen;
    assert(strcmp(lv_label_get_text(plugin_order_editor_title), "Plugin Order") == 0);
    assert(strlen(plugin_order_editor_items[0].key) == sizeof(plugin_order_editor_items[0].key) - 1);
    assert(strlen(plugin_order_editor_items[0].label) == sizeof(plugin_order_editor_items[0].label) - 1);
    memset(long_key, 'x', sizeof(long_key) - 1);
    memset(long_label, 'y', sizeof(long_label) - 1);
    assert(plugin_order_editor_items[0].key[0] == 'k');
    assert(plugin_order_editor_items[0].label[0] == 'l');
    assert(live_editor_rows() == 3 && zone_count == 3);

    unsigned char items_before_failure[sizeof(plugin_order_editor_items)];
    int order_before_failure[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    memcpy(items_before_failure, plugin_order_editor_items, sizeof(items_before_failure));
    memcpy(order_before_failure, plugin_order_editor_order, sizeof(order_before_failure));
    assert(gui_plugin_show_order_editor("Bad", keys, labels, visible, 0) == -1);
    assert(memcmp(items_before_failure, plugin_order_editor_items, sizeof(items_before_failure)) == 0);
    assert(memcmp(order_before_failure, plugin_order_editor_order, sizeof(order_before_failure)) == 0);
    assert(strcmp(lv_label_get_text(plugin_order_editor_title), "Plugin Order") == 0);
    assert(gui_plugin_show_order_editor("Duplicate", keys, labels, visible, 3) == -1);
    assert(memcmp(items_before_failure, plugin_order_editor_items, sizeof(items_before_failure)) == 0);
    assert(memcmp(order_before_failure, plugin_order_editor_order, sizeof(order_before_failure)) == 0);
    assert(strcmp(lv_label_get_text(plugin_order_editor_title), "Plugin Order") == 0);

    /* No-op commits are silent; changed order commits return copied rows. */
    int before_change = change_count;
    memcpy(plugin_order_editor_drag_state.ids, plugin_order_editor_order,
           3 * sizeof(plugin_order_editor_order[0]));
    plugin_order_editor_commit_drag(&plugin_order_editor_drag_state);
    assert(change_count == before_change);
    plugin_order_editor_drag_state.ids[0] = 2;
    plugin_order_editor_drag_state.ids[1] = 0;
    plugin_order_editor_drag_state.ids[2] = 1;
    plugin_order_editor_commit_drag(&plugin_order_editor_drag_state);
    assert(change_count == before_change + 1);

    /* Visibility callback reports the current committed order and values. */
    plugin_order_editor_drag_state.active = true;
    plugin_order_editor_drag_state.drag_id = 1;
    plugin_order_editor_drag_state.list_was_scrollable =
        lv_obj_has_flag(plugin_order_editor_list, LV_OBJ_FLAG_SCROLLABLE);
    plugin_order_editor_drag_state.list = plugin_order_editor_list;
    plugin_order_editor_drag_state.rows = plugin_order_editor_rows;
    plugin_order_editor_drag_state.row_capacity = PLUGIN_ORDER_EDITOR_MAX_ITEMS;
    plugin_order_editor_drag_state.restore = plugin_order_editor_restore_drag;
    lv_obj_add_state(plugin_order_editor_switches[1], LV_STATE_CHECKED);
    lv_obj_send_event(plugin_order_editor_switches[1], LV_EVENT_VALUE_CHANGED, NULL);
    assert(change_count == before_change + 2);
    /* Restoring the active drag refreshes the switch before the toggle handler
     * reads its target, so the captured result remains the stored false. */
    assert(!plugin_order_editor_drag_state.active);
    assert(!plugin_order_editor_items[1].visible);

    /* Without an active drag the same switch change is applied normally. */
    lv_obj_add_state(plugin_order_editor_switches[1], LV_STATE_CHECKED);
    lv_obj_send_event(plugin_order_editor_switches[1], LV_EVENT_VALUE_CHANGED, NULL);
    assert(change_count == before_change + 3);
    assert(plugin_order_editor_items[1].visible);

    /* Both unload events finish without commit; close fires only off-stack. */
    lv_obj_send_event(original_screen, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_send_event(original_screen, LV_EVENT_SCREEN_UNLOADED, NULL);
    assert(close_count == 0);
    remove_from_navigation(original_screen);
    lv_obj_send_event(original_screen, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_send_event(original_screen, LV_EVENT_SCREEN_UNLOADED, NULL);
    assert(close_count == 2);

    /* Rebuilding unregisters old gesture zones before list cleanup. */
    const char * next_keys[] = { "delta", "epsilon" };
    const char * next_labels[] = { "Delta", "Epsilon" };
    const bool next_visible[] = { true, true };
    assert(gui_plugin_show_order_editor("Rebuilt", next_keys, next_labels,
                                        next_visible, 2) == 0);
    assert(plugin_order_editor_screen == original_screen);
    assert(strcmp(lv_label_get_text(plugin_order_editor_title), "Rebuilt") == 0);
    assert(plugin_order_editor_count == 2 && zone_count == 2);

    /* An active drag is cancelled before dead zones are removed and screen deleted. */
    plugin_order_editor_drag_state.active = true;
    plugin_order_editor_drag_state.drag_id = 0;
    plugin_order_editor_drag_state.list_was_scrollable =
        lv_obj_has_flag(plugin_order_editor_list, LV_OBJ_FLAG_SCROLLABLE);
    plugin_order_editor_drag_state.list = plugin_order_editor_list;
    plugin_order_editor_drag_state.rows = plugin_order_editor_rows;
    plugin_order_editor_drag_state.row_capacity = PLUGIN_ORDER_EDITOR_MAX_ITEMS;
    plugin_order_editor_drag_state.restore = plugin_order_editor_restore_drag;
    lv_obj_t * deleted_screen = plugin_order_editor_screen;
    unsigned char items_before_teardown[sizeof(plugin_order_editor_items)];
    int order_before_teardown[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    int closes_before_teardown = close_count;
    memcpy(items_before_teardown, plugin_order_editor_items, sizeof(items_before_teardown));
    memcpy(order_before_teardown, plugin_order_editor_order, sizeof(order_before_teardown));
    gui_plugin_order_editor_teardown();
    assert(!lv_obj_is_valid(deleted_screen));
    assert(close_count == closes_before_teardown);
    check_all_state_cleared();
    assert(zone_count == 0);
    assert(memcmp(items_before_teardown, plugin_order_editor_items, sizeof(items_before_teardown)) == 0);
    assert(memcmp(order_before_teardown, plugin_order_editor_order, sizeof(order_before_teardown)) == 0);
    gui_plugin_order_editor_teardown();
    check_all_state_cleared();
    assert(close_count == closes_before_teardown);
    assert(memcmp(items_before_teardown, plugin_order_editor_items, sizeof(items_before_teardown)) == 0);
    assert(memcmp(order_before_teardown, plugin_order_editor_order, sizeof(order_before_teardown)) == 0);

    /* A one-row reopen overwrites only index zero in the retained arrays. */
    unsigned char items_before_reopen[sizeof(plugin_order_editor_items)];
    int order_before_reopen[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    memcpy(items_before_reopen, plugin_order_editor_items, sizeof(items_before_reopen));
    memcpy(order_before_reopen, plugin_order_editor_order, sizeof(order_before_reopen));
    assert(gui_plugin_show_order_editor(NULL, keys, labels, visible, 1) == 0);
    assert(strcmp(lv_label_get_text(plugin_order_editor_title), "Plugin Order") == 0);
    assert(memcmp(&items_before_reopen[sizeof(plugin_order_editor_items[0])],
                  &plugin_order_editor_items[1], sizeof(plugin_order_editor_items) -
                  sizeof(plugin_order_editor_items[0])) == 0);
    assert(memcmp(&order_before_reopen[1], &plugin_order_editor_order[1],
                  sizeof(order_before_reopen) - sizeof(order_before_reopen[0])) == 0);
    gui_plugin_order_editor_teardown();
    check_all_state_cleared();

    /* The configured 24-row capacity is a supported successful input. */
    char max_keys[PLUGIN_ORDER_EDITOR_MAX_ITEMS][16];
    char max_labels[PLUGIN_ORDER_EDITOR_MAX_ITEMS][16];
    const char * max_key_ptrs[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    const char * max_label_ptrs[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    bool max_visible[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    for (int i = 0; i < PLUGIN_ORDER_EDITOR_MAX_ITEMS; ++i) {
        snprintf(max_keys[i], sizeof(max_keys[i]), "key-%02d", i);
        snprintf(max_labels[i], sizeof(max_labels[i]), "Label %02d", i);
        max_key_ptrs[i] = max_keys[i];
        max_label_ptrs[i] = max_labels[i];
        max_visible[i] = (i % 2) == 0;
    }
    assert(gui_plugin_show_order_editor("Maximum", max_key_ptrs, max_label_ptrs,
                                        max_visible, PLUGIN_ORDER_EDITOR_MAX_ITEMS) == 0);
    assert(plugin_order_editor_count == PLUGIN_ORDER_EDITOR_MAX_ITEMS);
    assert(live_editor_rows() == PLUGIN_ORDER_EDITOR_MAX_ITEMS);
    assert(zone_count == PLUGIN_ORDER_EDITOR_MAX_ITEMS);
    closes_before_teardown = close_count;
    gui_plugin_order_editor_teardown();
    check_all_state_cleared();
    assert(zone_count == 0);
    assert(close_count == closes_before_teardown);

    /* A real pointer long-press starts the registered handle drag and timer. */
    assert(gui_plugin_show_order_editor("Pointer", keys, labels, visible, 3) == 0);
    lv_obj_update_layout(plugin_order_editor_screen);
    lv_area_t handle_area;
    lv_obj_get_coords(plugin_order_editor_handles[0], &handle_area);
    pointer_point.x = (handle_area.x1 + handle_area.x2) / 2;
    pointer_point.y = (handle_area.y1 + handle_area.y2) / 2;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_t * pointer_indev = lv_indev_create();
    assert(pointer_indev);
    lv_indev_set_type(pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_indev, display);
    lv_indev_set_read_cb(pointer_indev, pointer_read);
    lv_indev_set_long_press_time(pointer_indev, 10);
    lv_indev_read(pointer_indev);
    lv_tick_inc(20);
    lv_indev_read(pointer_indev);
    assert(plugin_order_editor_drag_state.active);
    assert(plugin_order_editor_drag_state.drag_id == 0);
    assert(plugin_order_editor_drag_state.indev == pointer_indev);
    assert(plugin_order_editor_drag_state.timer);
    lv_timer_t * drag_timer = plugin_order_editor_drag_state.timer;
    assert(timer_is_registered(drag_timer));
    int closes_before_pointer_teardown = close_count;
    lv_obj_t * pointer_screen = plugin_order_editor_screen;
    gui_plugin_order_editor_teardown();
    assert(!lv_obj_is_valid(pointer_screen));
    assert(!plugin_order_editor_drag_state.active);
    assert(plugin_order_editor_drag_state.timer == NULL);
    assert(!timer_is_registered(drag_timer));
    assert(zone_count == 0);
    assert(close_count == closes_before_pointer_teardown);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_delete(pointer_indev);

    assert(gui_plugin_show_order_editor(NULL, keys, labels, visible, 1) == 0);
    gui_plugin_order_editor_teardown();
    check_all_state_cleared();

    fputs(trace_log, stdout);
    lv_style_reset(&list_row_pressed_style);
    lv_style_reset(&accent_style);
    lv_display_delete(display);
    return 0;
}
