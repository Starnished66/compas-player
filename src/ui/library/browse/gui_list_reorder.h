#ifndef GUI_LIST_REORDER_H
#define GUI_LIST_REORDER_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define GUI_LIST_REORDER_MAX_ROWS 200

typedef struct gui_list_reorder_state gui_list_reorder_state_t;
struct gui_list_reorder_state {
    lv_obj_t * list;
    lv_obj_t ** rows;
    int row_capacity;
    int ids[GUI_LIST_REORDER_MAX_ROWS];
    int count;
    int drag_id;
    int drag_count;
    int32_t pointer_y;
    lv_indev_t * indev;
    lv_timer_t * timer;
    bool active;
    bool list_was_scrollable;
    bool changed;
    int row_base_index;
    void (*prepare)(gui_list_reorder_state_t * state, int id);
    void (*commit)(gui_list_reorder_state_t * state);
    void (*restore)(gui_list_reorder_state_t * state);
};

void gui_list_reorder_event(gui_list_reorder_state_t * state, int id, lv_event_t * e);
void gui_list_reorder_finish(gui_list_reorder_state_t * state, bool commit);
void gui_list_reorder_row_stop_translate(lv_obj_t * row);

#endif
