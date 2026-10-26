#include "../gui_list_reorder.c"
#include <assert.h>

uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
lv_font_t lv_font_montserrat_16;

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void assert_children(lv_obj_t * list, lv_obj_t ** expected, uint32_t count) {
    assert(lv_obj_get_child_count(list) == count);
    for (uint32_t i = 0; i < count; ++i) assert(lv_obj_get_child(list, (int32_t)i) == expected[i]);
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    static uint8_t display_buffer[240 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);

    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_t * list = lv_obj_create(screen);
    lv_obj_set_size(list, 220, 300);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_t * previous_page = lv_obj_create(list);
    lv_obj_set_size(previous_page, 220, 20);
    lv_obj_t * rows[3];
    for (int i = 0; i < 3; ++i) {
        rows[i] = lv_obj_create(list);
        lv_obj_set_size(rows[i], 220, 40);
    }
    lv_obj_t * next_page = lv_obj_create(list);
    lv_obj_set_size(next_page, 220, 20);
    lv_screen_load(screen);
    lv_obj_update_layout(list);

    gui_list_reorder_state_t state = {
        .list = list,
        .rows = rows,
        .row_capacity = 3,
        .ids = { 0, 1, 2 },
        .count = 3,
        .row_base_index = 1,
    };
    lv_area_t area;
    lv_obj_get_coords(rows[2], &area);
    assert(gui_list_reorder_rows(&state, 0, area.y2 + 1));
    lv_obj_t * expected_forward[] = { previous_page, rows[1], rows[2], rows[0], next_page };
    assert_children(list, expected_forward, 5);
    assert(state.ids[0] == 1 && state.ids[1] == 2 && state.ids[2] == 0);

    lv_obj_get_coords(rows[1], &area);
    assert(gui_list_reorder_rows(&state, 0, area.y1 - 1));
    lv_obj_t * expected_backward[] = { previous_page, rows[0], rows[1], rows[2], next_page };
    assert_children(list, expected_backward, 5);
    assert(state.ids[0] == 0 && state.ids[1] == 1 && state.ids[2] == 2);

    lv_obj_set_style_translate_y(rows[0], 11, 0);
    gui_list_reorder_row_stop_translate(rows[0]);
    assert(lv_obj_get_style_translate_y(rows[0], LV_PART_MAIN) == 0);

    state.rows = NULL;
    state.row_capacity = GUI_LIST_REORDER_MAX_ROWS;
    gui_list_reorder_finish(&state, false);
    lv_obj_delete(screen);
    return 0;
}
