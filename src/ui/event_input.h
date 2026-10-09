#pragma once
#include "lvgl/lvgl.h"
#include "src/indev/lv_indev_private.h"
#include "src/display/lv_display_private.h"

/* LVGL 9.5 has no public getter for these read preconditions. Keep the
 * dependency in one place, tested with disabled input and screen animation. */
static inline bool ui_event_input_can_read(const lv_indev_t *input) {
    return input && input->enabled && input->disp && !input->disp->prev_scr;
}

/* Used by both ordinary wake and direct event dispatch. Event-mode readers
 * sleep when released, but must keep running for a stationary press. */
static inline void ui_input_set_runtime_timer(lv_indev_t *input, bool screen_on) {
    if (!input) return;
    lv_timer_t *timer = lv_indev_get_read_timer(input);
    if (!timer) return;
    if (screen_on && (lv_indev_get_mode(input) != LV_INDEV_MODE_EVENT ||
                      lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED))
        lv_timer_resume(timer);
    else lv_timer_pause(timer);
}
