#include "player_seek_input.h"
#include "../../shell/gui_shell.h"

#include <math.h>
#include <stddef.h>

#include "../../../audio/engine/audio.h"
#include "player_seekbar.h"

#define TRANSPORT_SEEK_STEP_SECONDS 3.0
#define WHEEL_SEEK_SETTLE_MS 500

/* Step of hardware-button seeking; touch long-press keeps the fixed step. */
static int hw_seek_step_seconds = (int) TRANSPORT_SEEK_STEP_SECONDS;

/* Long-press seeking on next/prev buttons (fast-forward/rewind).
 *
 * The target accumulates seek offsets persistently so rapid repeat events
 * advance smoothly without re-reading intermediate positions before earlier
 * seeks complete. The single touch triple is shared by both directions.
 *
 * Playback generation changes during a hold cancel further seeks until the
 * next LONG_PRESSED event. Targets are clamped short of EOF to prevent
 * auto-advancing into the next track while holding. */
static double transport_seek_target_seconds;
static uint64_t transport_seek_playback_generation;
static bool transport_seek_hold_cancelled;

/* Separate held targets for each hardware direction retain repeat
 * accumulation while sharing touch seeking's generation and EOF guards. */
static double hw_seek_target_seconds[2];
static uint64_t hw_seek_playback_generation[2];
static bool hw_seek_hold_cancelled[2];

static bool prev_btn_long_press_fired;
static bool next_btn_long_press_fired;

static lv_timer_t * pending_progress_seek_timer;
/* Audio consumes the percent so it can bind conversion to this decoder's
 * generation, even before the decoder has published its duration. */
static double pending_progress_seek_percent;
/* Estimate used only by the arrival guard; never passed to audio seeking. */
static double pending_progress_seek_seconds;
bool user_seeking;
static bool progress_awaiting_seek;
static uint32_t progress_awaiting_seek_started_ms;

/* Shared math for touch repeat and physical button seeking. */
static double clamp_seek_target(double target) {
    if (target < 0.0) target = 0.0;
    double duration = audio_get_duration_seconds();
    double max_target = duration > TRANSPORT_SEEK_EOF_GUARD_SECONDS ? duration - TRANSPORT_SEEK_EOF_GUARD_SECONDS : 0.0;
    return target > max_target ? max_target : target;
}

static void apply_transport_seek_step(double dir, int step_count, bool is_first, double step_seconds,
                                      double * target_seconds, uint64_t * playback_generation,
                                      bool * hold_cancelled) {
    if (gui_shell_bt_dac_owns_output()) return;
    if (step_count <= 0) return;
    uint64_t gen = audio_get_playback_generation();
    if (is_first) {
        *target_seconds = audio_get_position_seconds();
        *playback_generation = gen;
        *hold_cancelled = false;
    } else if (gen != *playback_generation) {
        *hold_cancelled = true;
    }
    if (*hold_cancelled) return;

    *target_seconds = clamp_seek_target(*target_seconds + dir * step_seconds * (double) step_count);
    audio_seek(*target_seconds);
}

static void transport_seek_repeat_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    double dir = (double) (intptr_t) lv_event_get_user_data(e);
    apply_transport_seek_step(dir, 1, code == LV_EVENT_LONG_PRESSED, TRANSPORT_SEEK_STEP_SECONDS,
                              &transport_seek_target_seconds, &transport_seek_playback_generation,
                              &transport_seek_hold_cancelled);
}

/* LVGL can emit CLICKED after LONG_PRESSED. Clear a fired marker on
 * PRESS_LOST too, because that path has no following CLICKED to consume it. */
static bool * long_press_marker(int direction) {
    if (direction == -1) return &prev_btn_long_press_fired;
    if (direction == 1) return &next_btn_long_press_fired;
    return NULL;
}

static void transport_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    if (fired) *fired = true;
}

static void transport_long_press_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESS_LOST) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    if (fired) *fired = false;
}

void player_seek_input_bind_transport_target(lv_obj_t * target, int direction) {
    bool * fired = long_press_marker(direction);
    if (!target || !fired) return;
    lv_obj_add_event_cb(target, transport_long_press_cb, LV_EVENT_LONG_PRESSED, fired);
    lv_obj_add_event_cb(target, transport_long_press_cancel_cb, LV_EVENT_PRESS_LOST, fired);
    lv_obj_add_event_cb(target, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED,
                        (void *) (intptr_t) direction);
    lv_obj_add_event_cb(target, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *) (intptr_t) direction);
}

bool player_seek_input_consume_transport_click(int direction) {
    bool * fired = long_press_marker(direction);
    if (!fired || !*fired) return false;
    *fired = false;
    return true;
}

void player_seek_input_hw_steps(int direction, int step_count, bool is_first) {
    if ((direction != 1 && direction != -1) || step_count <= 0) return;
    unsigned slot = direction < 0;
    apply_transport_seek_step((double)direction, step_count, is_first, hw_seek_step_seconds,
                              &hw_seek_target_seconds[slot], &hw_seek_playback_generation[slot],
                              &hw_seek_hold_cancelled[slot]);
}

void player_seek_input_set_hw_step_seconds(int seconds) {
    if (seconds >= 1) hw_seek_step_seconds = seconds;
}

/* Wheel clicks add up: each moves a pending target and one seek runs after
 * the wheel settles. A playback change in between drops the pending seek. */
static lv_timer_t * wheel_seek_timer;
static double wheel_seek_target_seconds;
static uint64_t wheel_seek_playback_generation;

static void wheel_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    wheel_seek_timer = NULL;
    if (audio_get_playback_generation() == wheel_seek_playback_generation)
        audio_seek(wheel_seek_target_seconds);
}

void player_seek_input_hw_click(int direction) {
    if (direction != 1 && direction != -1) return;
    uint64_t gen = audio_get_playback_generation();
    if (!wheel_seek_timer || gen != wheel_seek_playback_generation) {
        wheel_seek_target_seconds = audio_get_position_seconds();
        wheel_seek_playback_generation = gen;
    }
    wheel_seek_target_seconds =
        clamp_seek_target(wheel_seek_target_seconds + direction * (double) hw_seek_step_seconds);
    if (wheel_seek_timer) {
        lv_timer_reset(wheel_seek_timer);
        return;
    }
    wheel_seek_timer = lv_timer_create(wheel_seek_timer_cb, WHEEL_SEEK_SETTLE_MS, NULL);
    lv_timer_set_repeat_count(wheel_seek_timer, 1);
}

static void cancel_pending_progress_seek(void) {
    if (!pending_progress_seek_timer) return;
    lv_timer_delete(pending_progress_seek_timer);
    pending_progress_seek_timer = NULL;
}

static void pending_progress_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    pending_progress_seek_timer = NULL;
    audio_seek_percent(pending_progress_seek_percent);
}

void player_seek_input_slider_event(lv_event_t * e, int32_t * displayed_percent) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        double duration = audio_get_duration_seconds();
        int32_t percent = player_seekbar_get_value(slider);
        pending_progress_seek_percent = (double) percent;
        pending_progress_seek_seconds = duration * ((double) percent / 100.0);
        *displayed_percent = percent;
        progress_awaiting_seek = (duration > 0.0);
        progress_awaiting_seek_started_ms = lv_tick_get();
        cancel_pending_progress_seek();
        pending_progress_seek_timer = lv_timer_create(pending_progress_seek_timer_cb, 150, NULL);
        lv_timer_set_repeat_count(pending_progress_seek_timer, 1);
        user_seeking = false;
    }
}

void player_seek_input_progress_reset(void) {
    cancel_pending_progress_seek();
    user_seeking = false;
    progress_awaiting_seek = false;
}

bool player_seek_input_progress_busy(void) {
    return user_seeking || progress_awaiting_seek || pending_progress_seek_timer != NULL;
}

bool player_seek_input_waiting_for_position(double position) {
    if (!progress_awaiting_seek) return false;
    if (fabs(position - pending_progress_seek_seconds) < 0.4 ||
        lv_tick_elaps(progress_awaiting_seek_started_ms) >= 4000) {
        progress_awaiting_seek = false;
        return false;
    }
    return true;
}
