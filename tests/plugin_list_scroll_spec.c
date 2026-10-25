#include "ui_fonts.h"
#include "gui_theme.h"
#include "screen_builders.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the production implementation so this fixture calls its actual
 * gui_plugin_show_list body and can initialize the private screen pool. */
#ifndef GUI_PLUGINS_IMPLEMENTATION
#define GUI_PLUGINS_IMPLEMENTATION "src/ui/gui_plugins.c"
#endif
#include GUI_PLUGINS_IMPLEMENTATION

static lv_obj_t *stack[PLUGIN_LIST_SCREEN_POOL_SIZE];
static int stack_count;
static lv_obj_t *top_screen;
static int nav_push_count;

void __wrap_nav_push(lv_obj_t *screen) {
    if (stack_count < PLUGIN_LIST_SCREEN_POOL_SIZE) stack[stack_count++] = screen;
    top_screen = screen;
    nav_push_count++;
}
bool __wrap_gui_navigation_contains(lv_obj_t *screen) {
    for (int i = 0; i < stack_count; ++i) if (stack[i] == screen) return true;
    return false;
}
bool __wrap_gui_navigation_is_top(lv_obj_t *screen) { return screen && screen == top_screen; }

static lv_display_t * display;
static void flush_ready(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    LV_UNUSED(a); LV_UNUSED(p); lv_display_flush_ready(d);
}

static void init_slot(int slot) {
    plugin_list_screens[slot] = lv_obj_create(NULL);
    plugin_list_title_labels[slot] = lv_label_create(plugin_list_screens[slot]);
    lv_obj_set_size(plugin_list_title_labels[slot], 200, 32);
    plugin_list_lists[slot] = lv_obj_create(plugin_list_screens[slot]);
    lv_obj_set_size(plugin_list_lists[slot], 240, 180);
    lv_obj_align(plugin_list_lists[slot], LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void assert_selected(int slot, int index) {
    assert(plugin_list_selected_indices[slot] == index);
    lv_obj_t * row = lv_obj_get_child(plugin_list_lists[slot], index);
    assert(row);
    assert(lv_obj_get_style_outline_width(row, LV_PART_MAIN) > 0);
}

static void exercise_replacement(int slot, bool start_grid, bool short_grid) {
    const int initial_count = 35;
    const int long_count = 55;
    const int short_count = 8;
    static const char *labels[64];
    static char values[64][24];
    for (int i = 0; i < 64; ++i) {
        snprintf(values[i], sizeof(values[i]), "Entry %02d", i);
        labels[i] = values[i];
    }
    lv_obj_t *list = plugin_list_lists[slot];
    int columns = start_grid ? 3 : 0;
    assert(gui_plugin_show_list("initial", labels, NULL, NULL, NULL, 0, 0,
                                2, initial_count, columns, -1) == slot);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, 100000, LV_ANIM_OFF);
    int32_t original_y = lv_obj_get_scroll_y(list);
    assert(original_y > 0);
    assert_selected(slot, 2);

    int next_columns = short_grid ? 3 : 0;
    assert(gui_plugin_show_list("long replacement", labels, NULL, NULL, NULL, 0, 0,
                                4, long_count, next_columns, slot) == slot);
    lv_obj_update_layout(list);
    int32_t after_long = lv_obj_get_scroll_y(list);
#ifdef EXPECT_BASELINE
    assert(after_long == 0);
#else
    assert(after_long == original_y);
#endif
    assert_selected(slot, 4);

    int pushes_before_reset = nav_push_count;
    lv_obj_scroll_to_y(list, 100000, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(list) > 0);
    assert(!gui_plugin_list_reset_scroll(-1));
    assert(gui_plugin_list_reset_scroll(slot));
    assert(lv_obj_get_scroll_y(list) == 0);
    assert(nav_push_count == pushes_before_reset);

    lv_obj_scroll_to_y(list, 100000, LV_ANIM_OFF);
    int32_t saved = lv_obj_get_scroll_y(list);
    assert(saved > 0);
#ifdef EXPECT_BASELINE
    (void)saved;
#endif
    assert(gui_plugin_show_list("short replacement", labels, NULL, NULL, NULL, 0, 0,
                                3, short_count, next_columns, slot) == slot);
    lv_obj_update_layout(list);
    int32_t after_short = lv_obj_get_scroll_y(list);
#ifdef EXPECT_BASELINE
    assert(after_short == 0);
#else
    assert(after_short >= 0 && after_short < saved);
#endif
    assert_selected(slot, 3);
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    display = lv_display_create(480, 800);
    assert(display);
    static uint8_t draw_buffer[480 * 12 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    gui_theme_init();
    screen_builders_init_list_row_style();
    init_slot(0);
    init_slot(1);
    exercise_replacement(0, false, false);
    exercise_replacement(1, true, true);

    /* A replacement request for a stacked but covered screen is rejected
     * before the existing rows or scroll offset can be touched. */
    assert(top_screen == plugin_list_screens[1]);
    uint32_t child_count = lv_obj_get_child_count(plugin_list_lists[0]);
    int32_t saved_scroll = lv_obj_get_scroll_y(plugin_list_lists[0]);
    assert(!gui_plugin_list_reset_scroll(0));
    assert(lv_obj_get_scroll_y(plugin_list_lists[0]) == saved_scroll);
    static const char *one[] = { "new" };
    assert(gui_plugin_show_list("not top", one, NULL, NULL, NULL, 0, 0,
                                0, 1, 0, 0) == -1);
    assert(lv_obj_get_child_count(plugin_list_lists[0]) == child_count);
    assert(lv_obj_get_scroll_y(plugin_list_lists[0]) == saved_scroll);

    int before_pushes = nav_push_count;
    init_slot(2);
    assert(gui_plugin_show_list("fresh", one, NULL, NULL, NULL, 0, 0,
                                0, 1, 0, -1) == 2);
    assert(nav_push_count == before_pushes + 1);
    assert(lv_obj_get_scroll_y(plugin_list_lists[2]) == 0);
    puts("production gui_plugin_show_list scroll behavior: PASS");
    return 0;
}
