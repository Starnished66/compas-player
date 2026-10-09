#include "src/ui/event_input.h"
#include <assert.h>
#include <stdio.h>
uint32_t gui_anim_ms(uint32_t milliseconds) { return milliseconds; }
static lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
static unsigned long_presses, repeats;
static void read_input(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev; data->point.x = 30; data->point.y = 30; data->state = state;
}
static void event_cb(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) ++long_presses;
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT) ++repeats;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
int main(void) {
    lv_init(); lv_tick_set_cb(NULL);
    static uint16_t pixels[100 * 100];
    lv_display_t *display = lv_display_create(100, 100);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_set_pos(button, 10, 10); lv_obj_set_size(button, 60, 60);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_ALL, NULL);
    lv_obj_update_layout(button);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(input, read_input);
    lv_indev_set_display(input, display); lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    assert(lv_timer_get_paused(lv_indev_get_read_timer(input)));
    assert(ui_event_input_can_read(input));
    state = LV_INDEV_STATE_PRESSED; lv_tick_inc(1); lv_indev_read(input);
    assert(!lv_timer_get_paused(lv_indev_get_read_timer(input)));
    ui_input_set_runtime_timer(input, false);
    assert(lv_timer_get_paused(lv_indev_get_read_timer(input)));
    ui_input_set_runtime_timer(input, true);
    assert(!lv_timer_get_paused(lv_indev_get_read_timer(input)));
    // No additional controller samples: LVGL's timer must recognize the hold.
    lv_tick_inc(700); lv_timer_handler(); assert(long_presses == 1);
    lv_tick_inc(400); lv_timer_handler(); assert(repeats > 0);
    state = LV_INDEV_STATE_RELEASED; lv_indev_read(input);
    assert(lv_timer_get_paused(lv_indev_get_read_timer(input)));
    lv_indev_enable(input, false); ui_input_set_runtime_timer(input, false); lv_indev_read(input);
    assert(!ui_event_input_can_read(input));
    assert(lv_timer_get_paused(lv_indev_get_read_timer(input)));
    lv_indev_enable(input, true); ui_input_set_runtime_timer(input, true);
    assert(ui_event_input_can_read(input));
    assert(lv_timer_get_paused(lv_indev_get_read_timer(input))); // wake must leave event timer idle
    lv_obj_t *second = lv_obj_create(NULL);
    lv_screen_load_anim(second, LV_SCREEN_LOAD_ANIM_FADE_IN, 500, 0, false);
    lv_tick_inc(1); lv_timer_handler();
    assert(!ui_event_input_can_read(input));
    lv_tick_inc(600); lv_timer_handler();
    assert(ui_event_input_can_read(input));
    lv_indev_delete(input); lv_display_delete(display); lv_deinit();
    puts("Real LVGL event-mode stationary long press, repeat, release and wake tests passed");
}
