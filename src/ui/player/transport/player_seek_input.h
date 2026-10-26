#ifndef PLAYER_SEEK_INPUT_H
#define PLAYER_SEEK_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#define TRANSPORT_SEEK_EOF_GUARD_SECONDS 0.5

/* Existing public ABI also declared by gui_player.h. */
extern bool user_seeking;

/* UI-thread-owned controller. Bound targets outlive their event callbacks.
 * The displayed-percent pointer is borrowed for the duration of the event
 * call and is never retained. Progress reset cancels slider seeking only; it
 * intentionally leaves touch and hardware hold accumulators untouched. */
void player_seek_input_bind_transport_target(lv_obj_t * target, int direction);
bool player_seek_input_consume_transport_click(int direction);
void player_seek_input_hw_steps(int direction, int step_count, bool is_first);

void player_seek_input_slider_event(lv_event_t * event, int32_t * displayed_percent);
void player_seek_input_progress_reset(void);
bool player_seek_input_progress_busy(void);
bool player_seek_input_waiting_for_position(double position);

#endif
