#include "../../src/ui/player/transport/player_seek_input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../../src/audio/engine/audio.h"

uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }

static double test_position;
static double test_duration;
static double test_last_seek;
static double test_last_seek_percent;
static uint64_t test_generation;
static unsigned test_seek_count;
static unsigned test_seek_percent_count;
static int32_t displayed_percent;

uint64_t audio_get_playback_generation(void) { return test_generation; }
double audio_get_position_seconds(void) { return test_position; }
double audio_get_duration_seconds(void) { return test_duration; }
void audio_seek(double seconds) { test_last_seek = seconds; ++test_seek_count; }
void audio_seek_percent(double percent) {
    test_last_seek_percent = percent;
    ++test_seek_percent_count;
}

static void test_slider_event_cb(lv_event_t * event) {
    player_seek_input_slider_event(event, &displayed_percent);
}

static void flush_cb(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void) area;
    (void) pixels;
    lv_display_flush_ready(display);
}

static void run_ms(uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static void test_touch_and_hardware_holds(lv_obj_t * parent) {
    lv_obj_t * next = lv_obj_create(parent);
    lv_obj_t * previous = lv_obj_create(parent);
    player_seek_input_bind_transport_target(previous, -1);
    player_seek_input_bind_transport_target(next, 1);

    test_position = 10.0;
    test_duration = 60.0;
    test_generation = 1;
    test_seek_count = 0;
    lv_obj_send_event(next, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_send_event(next, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    assert(test_seek_count == 2 && test_last_seek == 16.0);

    test_generation++;
    lv_obj_send_event(next, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    assert(test_seek_count == 2);
    test_position = 20.0;
    lv_obj_send_event(previous, LV_EVENT_LONG_PRESSED, NULL);
    assert(test_last_seek == 17.0); /* Touch directions share the same accumulator. */

    assert(player_seek_input_consume_transport_click(1));
    assert(!player_seek_input_consume_transport_click(1));
    assert(player_seek_input_consume_transport_click(-1));
    assert(!player_seek_input_consume_transport_click(-1));
    lv_obj_send_event(previous, LV_EVENT_PRESS_LOST, NULL);
    assert(!player_seek_input_consume_transport_click(-1));

    test_position = 5.0;
    player_seek_input_hw_steps(1, 2, true);
    assert(test_last_seek == 11.0);
    player_seek_input_hw_steps(-1, 1, true);
    assert(test_last_seek == 2.0);
    player_seek_input_hw_steps(1, 3, false);
    assert(test_last_seek == 20.0);
    player_seek_input_progress_reset(); /* Slider reset preserves a held hardware target. */
    player_seek_input_hw_steps(1, 1, false);
    assert(test_last_seek == 23.0);
    test_generation++;
    player_seek_input_hw_steps(1, 1, false);
    assert(test_last_seek == 23.0);

    test_duration = 18.2;
    test_position = 20.0;
    player_seek_input_hw_steps(1, 1, true);
    assert(fabs(test_last_seek - 17.7) < 1e-9);
    test_duration = 0.2;
    test_position = 20.0;
    player_seek_input_hw_steps(1, 1, true);
    assert(test_last_seek == 0.0);
}

static void test_progress_slider_input(lv_obj_t * parent) {
    lv_obj_t * slider = lv_slider_create(parent);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_add_event_cb(slider, test_slider_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(slider, test_slider_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(slider, test_slider_event_cb, LV_EVENT_PRESS_LOST, NULL);

    player_seek_input_progress_reset();
    test_duration = 100.0;
    test_position = 0.0;
    test_seek_percent_count = 0;

    lv_slider_set_value(slider, 20, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
    assert(user_seeking && player_seek_input_progress_busy());
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    assert(!user_seeking && displayed_percent == 20);
    assert(player_seek_input_progress_busy());

    lv_slider_set_value(slider, 35, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(slider, LV_EVENT_PRESS_LOST, NULL);
    assert(displayed_percent == 35 && !user_seeking);
    run_ms(140);
    assert(test_seek_percent_count == 0);
    run_ms(20);
    assert(test_seek_percent_count == 1 && test_last_seek_percent == 35.0);

    assert(player_seek_input_waiting_for_position(0.0));
    assert(!player_seek_input_waiting_for_position(34.8));
    assert(!player_seek_input_progress_busy());

    lv_slider_set_value(slider, 15, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
    run_ms(160);
    assert(test_seek_percent_count == 2 && test_last_seek_percent == 15.0);
    assert(user_seeking); /* Press does not cancel an already armed release timer. */
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    run_ms(160);
    assert(test_seek_percent_count == 3 && test_last_seek_percent == 40.0);

    lv_slider_set_value(slider, 80, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    assert(player_seek_input_waiting_for_position(0.0));
    lv_tick_inc(4000);
    assert(!player_seek_input_waiting_for_position(0.0));

    lv_slider_set_value(slider, 60, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    assert(player_seek_input_progress_busy());
    unsigned seeks_before_reset = test_seek_percent_count;
    player_seek_input_progress_reset();
    assert(!user_seeking && !player_seek_input_progress_busy());
    run_ms(200);
    assert(test_seek_percent_count == seeks_before_reset);

    test_duration = 0.0;
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    assert(!player_seek_input_waiting_for_position(0.0));
    assert(player_seek_input_progress_busy()); /* Debounce timer still owns the pending input. */
    player_seek_input_progress_reset();
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(240, 320);
    static uint8_t draw_buffer[240 * 32 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_screen_load(screen);

    test_touch_and_hardware_holds(screen);
    test_progress_slider_input(screen);
    puts("Player seek input: hold, click suppression, debounce and settling passed");
    return 0;
}
