/* Host integration spec for the production gesture_note_pointer_sample helper.
 * The runner extracts that static function verbatim from gui_shell.c, then
 * includes it above this harness. LVGL indev/timers are real; only the raw
 * driver callback and gesture state are controlled here. Wake suppression is
 * applied by wrapped_pointer_read_cb before this helper; that wrapper filter
 * is intentionally outside this helper-focused spec. */
#include <assert.h>
#include <stdio.h>

#include "lvgl/src/lvgl.h"
#ifdef UI_PERF_TRACE
#include "ui_frame_trace.h"
void ui_frame_trace_event(ui_frame_trace_event_kind_t kind, uint32_t a, uint32_t b) {
    (void)kind;
    (void)a;
    (void)b;
}
#endif

/* The focused link uses LVGL objects only; production fonts/theme helpers are
 * intentionally outside this scheduling test. */
lv_font_t lv_font_montserrat_16;
uint32_t gui_anim_ms(uint32_t milliseconds) { return milliseconds; }

static lv_indev_state_t s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
static lv_point_t s_last_raw_pointer_point;
static bool s_last_raw_pointer_point_valid;
static lv_timer_t *quick_drawer_drag_timer;
static bool home_swipe_tracking, player_swipe_tracking, back_swipe_tracking;
static bool quick_drawer_drag_claimed, quick_drawer_expansion_dragging;
static bool home_swipe_candidate, player_swipe_candidate, back_swipe_candidate;
static bool quick_drawer_drag_tracking;

#include "gesture_sample_helper.inc"

static lv_indev_t *pointer;
static lv_indev_state_t raw_state;
static lv_point_t raw_point;
static bool in_raw_callback;
static unsigned timer_calls, callback_inside_raw;
static unsigned long_press_events;
static lv_indev_state_t state_seen_by_timer;

static void raw_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->state = raw_state;
    data->point = raw_point;
    in_raw_callback = true;
    gesture_note_pointer_sample(data);
    /* Match wrapped_pointer_read_cb: publish last-raw state only after the
     * helper, while LVGL still has the previous public indev state. */
    s_last_raw_pointer_state = data->state;
    in_raw_callback = false;
}

static void drag_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (in_raw_callback) callback_inside_raw++;
    timer_calls++;
    state_seen_by_timer = lv_indev_get_state(pointer);
}

static void button_event_cb(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) long_press_events++;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void sample(lv_indev_state_t state, int x, int y) {
    raw_state = state;
    raw_point.x = x;
    raw_point.y = y;
    lv_indev_read(pointer);
}

static void pause_indev_timer(void) {
    lv_timer_t *timer = lv_indev_get_read_timer(pointer);
    if (timer && !lv_timer_get_paused(timer)) lv_timer_pause(timer);
}

static uint32_t run_handler(unsigned ticks) {
    lv_tick_inc(ticks);
    return lv_timer_handler();
}

int main(void) {
    assert(!player_swipe_candidate && !back_swipe_candidate && !quick_drawer_drag_tracking);
    lv_init();
    lv_tick_set_cb(NULL);
    static uint16_t pixels[64 * 64];
    lv_display_t *display = lv_display_create(64, 64);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_cb);

    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, raw_read);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_display(pointer, display);
    quick_drawer_drag_timer = lv_timer_create(drag_timer_cb, 16, NULL);
    assert(quick_drawer_drag_timer);

    /* A candidate press read marks the timer ready, but never executes its
     * callback inline. LVGL publishes the new state after read_cb returns. */
    home_swipe_candidate = true;
    sample(LV_INDEV_STATE_PRESSED, 12, 30);
    assert(timer_calls == 0);
    assert(lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED);
    assert(!callback_inside_raw);
    pause_indev_timer(); /* isolate the drag timer from event-mode long-press reads */
    run_handler(1);
    assert(timer_calls == 1);
    assert(state_seen_by_timer == LV_INDEV_STATE_PRESSED);
    assert(!callback_inside_raw);

    /* A repeated held coordinate does not continually ready the timer. */
    sample(LV_INDEV_STATE_PRESSED, 12, 30);
    assert(timer_calls == 1);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 1);

    /* Candidate tracking changes are ready immediately; once a gesture owns
     * tracking, movement is left to the existing 16 ms presentation cadence. */
    sample(LV_INDEV_STATE_PRESSED, 15, 30);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 2);
    home_swipe_candidate = false;
    home_swipe_tracking = true;
    sample(LV_INDEV_STATE_PRESSED, 25, 30);
    assert(timer_calls == 2);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 2);
    run_handler(14);
    assert(timer_calls == 2);
    run_handler(1);
    assert(timer_calls == 3);
    assert(lv_timer_get_time_until_next() <= 16);

    /* Release ends tracking and is always made ready for snap/commit logic. */
    sample(LV_INDEV_STATE_RELEASED, 25, 30);
    assert(timer_calls == 3);
    assert(lv_indev_get_state(pointer) == LV_INDEV_STATE_RELEASED);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 4);
    assert(state_seen_by_timer == LV_INDEV_STATE_RELEASED);
    assert(!callback_inside_raw);

    /* An ordinary press followed by movement, with no gesture candidate or
     * owner, must not ready the polling timer. */
    home_swipe_candidate = false;
    home_swipe_tracking = false;
    sample(LV_INDEV_STATE_PRESSED, 70, 70);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 4);
    sample(LV_INDEV_STATE_PRESSED, 80, 70);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 4);

    /* One candidate may coexist with a different active gesture. The active
     * tracking state keeps presentation on the normal timer cadence. */
    home_swipe_candidate = true;
    player_swipe_tracking = true;
    sample(LV_INDEV_STATE_PRESSED, 90, 70);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 4);
    player_swipe_tracking = false;
    sample(LV_INDEV_STATE_RELEASED, 90, 70);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 5);
    home_swipe_candidate = false;

    /* Idle, unpressed coordinate changes are not gesture samples. */
    sample(LV_INDEV_STATE_RELEASED, 40, 40);
    pause_indev_timer();
    run_handler(1);
    assert(timer_calls == 5);

    /* Input runtime policy may pause the gesture timer while idle. Sampling
     * does not resume a paused timer or create a new cadence. */
    lv_timer_pause(quick_drawer_drag_timer);
    home_swipe_candidate = true;
    sample(LV_INDEV_STATE_PRESSED, 45, 45);
    assert(lv_timer_get_paused(quick_drawer_drag_timer));
    assert(timer_calls == 5);
    sample(LV_INDEV_STATE_RELEASED, 45, 45);
    pause_indev_timer();
    home_swipe_candidate = false;

    /* Real LVGL's event-mode read timer still services long presses while a
     * stationary finger holds. The drag timer is created after the indev, so
     * it runs first in this handler; the later indev read readies an already
     * visited timer. LVGL returns 0 and runs that ready callback next pass. */
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_set_pos(button, 5, 5);
    lv_obj_set_size(button, 30, 30);
    lv_obj_add_event_cb(button, button_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_update_layout(button);
    lv_indev_set_long_press_time(pointer, 100);
    home_swipe_candidate = true;
    home_swipe_tracking = false;
    player_swipe_tracking = false;
    back_swipe_tracking = false;
    quick_drawer_drag_claimed = false;
    quick_drawer_expansion_dragging = false;
    lv_timer_resume(quick_drawer_drag_timer);
    lv_timer_resume(lv_indev_get_read_timer(pointer));
    raw_state = LV_INDEV_STATE_PRESSED;
    raw_point.x = 15;
    raw_point.y = 15;
    unsigned before_ordering = timer_calls;
    uint32_t handler_wait = run_handler(16);
    assert(handler_wait == 0);
    assert(timer_calls == before_ordering + 1);
    assert(lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED);
    assert(run_handler(0) <= 16);
    assert(timer_calls == before_ordering + 2);
    assert(state_seen_by_timer == LV_INDEV_STATE_PRESSED);
    assert(long_press_events == 0);

    /* Repeated stationary samples from LVGL's own timer never keep the drag
     * timer artificially ready, and still produce the standard long press. */
    unsigned before_hold = timer_calls;
    for (unsigned elapsed = 0; elapsed < 120; elapsed++) {
        uint32_t wait = run_handler(1);
        assert(wait > 0); /* unchanged samples never force a zero-delay loop */
    }
    assert(long_press_events == 1);
    assert(timer_calls - before_hold <= 8); /* at most the existing 16 ms cadence */

    lv_timer_delete(quick_drawer_drag_timer);
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lv_deinit();
    puts("Gesture pointer sample scheduling and LVGL long-press spec passed");
    return 0;
}
