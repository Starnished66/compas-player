#include "gui_list_reorder.h"
#include "src/core/board_config.h"
#include <string.h>

static void gui_list_reorder_row_translate_anim_cb(void * var, int32_t value) {
    lv_obj_t * row = var;
    if (row) lv_obj_set_style_translate_y(row, value, 0);
}

void gui_list_reorder_row_stop_translate(lv_obj_t * row) {
    if (!row) return;
    lv_anim_delete(row, gui_list_reorder_row_translate_anim_cb);
    lv_obj_set_style_translate_y(row, 0, 0);
}

static void gui_list_reorder_animate_rows(lv_obj_t ** rows, const int * ids, int count) {
    for (int i = 0; i < count; ++i) {
        lv_obj_t * row = rows[ids[i]];
        if (!row) continue;
        lv_anim_delete(row, gui_list_reorder_row_translate_anim_cb);
        int32_t offset = lv_obj_get_style_translate_y(row, LV_PART_MAIN);
        if (!offset) continue;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, row);
        lv_anim_set_values(&anim, offset, 0);
        lv_anim_set_duration(&anim, 150);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, gui_list_reorder_row_translate_anim_cb);
        lv_anim_start(&anim);
    }
}

static bool gui_list_reorder_rows(gui_list_reorder_state_t * state, int id, int32_t pointer_y) {
    if (!state || !state->list || !state->rows || state->row_capacity < 1 ||
        state->row_capacity > GUI_LIST_REORDER_MAX_ROWS || state->count <= 1 ||
        state->count > state->row_capacity || state->count > GUI_LIST_REORDER_MAX_ROWS)
        return false;
    int * order = state->ids;
    int count = state->count;
    int from = -1;
    bool seen[GUI_LIST_REORDER_MAX_ROWS] = { false };
    for (int i = 0; i < count; ++i) if (order[i] == id) { from = i; break; }
    if (from < 0) return false;
    lv_obj_update_layout(state->list);
    int32_t visual_y[GUI_LIST_REORDER_MAX_ROWS];
    for (int i = 0; i < count; ++i) {
        int row_id = order[i];
        if (row_id < 0 || row_id >= state->row_capacity || row_id >= GUI_LIST_REORDER_MAX_ROWS ||
            seen[row_id] || !state->rows[row_id] ||
            lv_obj_get_parent(state->rows[row_id]) != state->list) return false;
        seen[row_id] = true;
        lv_area_t row_area;
        lv_obj_get_coords(state->rows[row_id], &row_area);
        /* Flex layout includes translate_y in the current rendered coordinates. */
        visual_y[row_id] = row_area.y1;
    }
    int to = from;
    while (to + 1 < count) {
        lv_obj_t * next = state->rows[order[to + 1]];
        lv_area_t next_area;
        lv_obj_get_coords(next, &next_area);
        int32_t next_center = (next_area.y1 + next_area.y2) / 2 -
            lv_obj_get_style_translate_y(next, LV_PART_MAIN);
        if (pointer_y <= next_center) break;
        ++to;
    }
    while (to > 0) {
        lv_obj_t * previous = state->rows[order[to - 1]];
        lv_area_t previous_area;
        lv_obj_get_coords(previous, &previous_area);
        int32_t previous_center = (previous_area.y1 + previous_area.y2) / 2 -
            lv_obj_get_style_translate_y(previous, LV_PART_MAIN);
        if (pointer_y >= previous_center) break;
        --to;
    }
    if (to == from) return false;
    int moved_id = order[from];
    if (to > from) memmove(&order[from], &order[from + 1], (size_t)(to - from) * sizeof(int));
    else memmove(&order[to + 1], &order[to], (size_t)(from - to) * sizeof(int));
    order[to] = moved_id;
    for (int i = 0; i < count; ++i)
        lv_obj_move_to_index(state->rows[order[i]], state->row_base_index + i);
    lv_obj_update_layout(state->list);
    for (int i = 0; i < count; ++i) {
        int row_id = order[i];
        lv_obj_t * row = state->rows[row_id];
        lv_anim_delete(row, gui_list_reorder_row_translate_anim_cb);
        lv_area_t row_area;
        lv_obj_get_coords(row, &row_area);
        int32_t offset = visual_y[row_id] - row_area.y1 +
            lv_obj_get_style_translate_y(row, LV_PART_MAIN);
        lv_obj_set_style_translate_y(row, offset, 0);
    }
    gui_list_reorder_animate_rows(state->rows, order, count);
    return true;
}

void gui_list_reorder_finish(gui_list_reorder_state_t * state, bool commit) {
    if (!state) return;
    if (state->timer) { lv_timer_delete(state->timer); state->timer = NULL; }
    if (!state->active) {
        if (state->rows && state->row_capacity > 0 &&
            state->row_capacity <= GUI_LIST_REORDER_MAX_ROWS)
            for (int id = 0; id < state->row_capacity; ++id)
                gui_list_reorder_row_stop_translate(state->rows[id]);
        return;
    }
    bool should_commit = commit && state->changed;
    bool restore_ui = !should_commit;
    if (state->list_was_scrollable && state->list)
        lv_obj_add_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
    if (state->rows && state->drag_id >= 0 && state->drag_id < state->row_capacity &&
        state->rows[state->drag_id])
        lv_obj_clear_state(state->rows[state->drag_id], LV_STATE_USER_1);
    state->active = false;
    state->drag_id = -1;
    state->indev = NULL;
    state->list_was_scrollable = false;
    state->changed = false;
    if (restore_ui) {
        if (state->rows && state->row_capacity > 0 &&
            state->row_capacity <= GUI_LIST_REORDER_MAX_ROWS)
            for (int id = 0; id < state->row_capacity; ++id)
                gui_list_reorder_row_stop_translate(state->rows[id]);
        if (state->restore) state->restore(state);
    } else if (state->commit) {
        /* Detach the drag before adapters can rebuild or delete its UI. */
        state->commit(state);
    }
}

static void gui_list_reorder_timer_cb(lv_timer_t * timer) {
    gui_list_reorder_state_t * state = lv_timer_get_user_data(timer);
    if (!state || !state->active || !state->list) return;
    if (lv_obj_get_screen(state->list) != lv_screen_active() || !state->indev ||
        lv_indev_get_state(state->indev) != LV_INDEV_STATE_PRESSED) {
        gui_list_reorder_finish(state, false);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(state->indev, &p);
    state->pointer_y = p.y;
    lv_area_t list_area;
    lv_obj_get_coords(state->list, &list_area);
    int32_t edge = BOARD_SCALE_PX(36);
    if (p.y < list_area.y1 + edge) {
        int32_t scroll = list_area.y1 + edge - p.y;
        if (scroll > BOARD_SCALE_PX(24)) scroll = BOARD_SCALE_PX(24);
        lv_obj_scroll_by_bounded(state->list, 0, scroll, LV_ANIM_OFF);
    } else if (p.y > list_area.y2 - edge) {
        int32_t scroll = p.y - (list_area.y2 - edge);
        if (scroll > BOARD_SCALE_PX(24)) scroll = BOARD_SCALE_PX(24);
        lv_obj_scroll_by_bounded(state->list, 0, -scroll, LV_ANIM_OFF);
    }
    if (gui_list_reorder_rows(state, state->drag_id, state->pointer_y)) state->changed = true;
}

static void gui_list_reorder_begin(gui_list_reorder_state_t * state, int id) {
    if (!state || state->active || !state->prepare || !state->list || !state->rows) return;
    if (state->row_capacity < 1 || state->row_capacity > GUI_LIST_REORDER_MAX_ROWS) return;
    state->prepare(state, id);
    if (state->count < 1 || state->count > state->row_capacity ||
        state->count > GUI_LIST_REORDER_MAX_ROWS) return;
    bool found = false;
    bool seen[GUI_LIST_REORDER_MAX_ROWS] = { false };
    for (int i = 0; i < state->count; ++i) {
        int row_id = state->ids[i];
        if (row_id < 0 || row_id >= state->row_capacity || row_id >= GUI_LIST_REORDER_MAX_ROWS ||
            seen[row_id] || !state->rows[row_id] ||
            lv_obj_get_parent(state->rows[row_id]) != state->list) return;
        seen[row_id] = true;
        if (row_id == id) found = true;
    }
    if (!found) return;
    state->row_base_index = (int)lv_obj_get_index(state->rows[state->ids[0]]);
    state->active = true;
    state->drag_id = id;
    state->drag_count = state->count;
    state->indev = lv_indev_active();
    if (!state->indev) { gui_list_reorder_finish(state, false); return; }
    lv_point_t p;
    lv_indev_get_point(state->indev, &p);
    state->pointer_y = p.y;
    state->list_was_scrollable = lv_obj_has_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_stop_scroll_anim(state->list);
    lv_obj_remove_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_state(state->rows[id], LV_STATE_USER_1);
    state->timer = lv_timer_create(gui_list_reorder_timer_cb, 50, state);
}

void gui_list_reorder_event(gui_list_reorder_state_t * state, int id, lv_event_t * e) {
    if (!state || !e) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        gui_list_reorder_begin(state, id);
    } else if (code == LV_EVENT_PRESSING && state->active && state->drag_id == id) {
        if (state->indev) {
            lv_point_t p;
            lv_indev_get_point(state->indev, &p);
            state->pointer_y = p.y;
        }
        if (gui_list_reorder_rows(state, state->drag_id, state->pointer_y)) state->changed = true;
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
               state->active && state->drag_id == id) {
        gui_list_reorder_finish(state, code == LV_EVENT_RELEASED);
    }
}
