/* Native Power + Volume Up button-lock chord. Build with:
 * cc -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections \
 *   -Isrc/core -Isrc/hardware -Isrc/network -Isrc/ui \
 *   src/hardware/input/hw_buttons_lock_test.c -Wl,--gc-sections -pthread \
 *   -o build/button-lock-grok/hw_buttons_lock_test
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool test_screen_on = true;

bool backlight_screen_is_on(void) { return test_screen_on; }
void bt_media_player_dispatch_avrcp_key(unsigned short code, int value) {
    (void) code;
    (void) value;
}
void ui_wake_notify(void) {}

#include "hw_buttons.c"

static unsigned event_count(void) {
    unsigned n = 0;
    hw_button_event_t event;
    while (hw_buttons_consume_event(&event)) n++;
    return n;
}

static hw_button_event_t take_event(button_mapping_button_t button,
                                    button_mapping_gesture_t gesture) {
    hw_button_event_t event;
    assert(hw_buttons_consume_event(&event));
    assert(event.button == button && event.gesture == gesture);
    return event;
}

static void expect_empty(void) {
    hw_button_event_t event;
    if (hw_buttons_consume_event(&event)) {
        fprintf(stderr, "unexpected event button=%d gesture=%d count=%u t=%u\n",
                (int) event.button, (int) event.gesture, event.count, event.timestamp_ms);
        abort();
    }
}

static void reset_state(void) {
    pthread_mutex_lock(&state_mutex);
    button_event_queue_count = 0;
    memset(mapped_button_states, 0, sizeof(mapped_button_states));
    memset(physical_key_held, 0, sizeof(physical_key_held));
    memset(physical_key_pressed_ms, 0, sizeof(physical_key_pressed_ms));
    screenshot_requested = false;
    screenshot_combo_enabled = false;
    screenshot_chord_used_for_power_hold = false;
    volume_lock_screen_off = false;
    button_lock_enabled = false;
    button_locked = false;
    button_lock_changed = false;
    lock_chord_active = false;
    lock_chord_used_for_power_hold = false;
    knob_volume_up_held = false;
    pthread_mutex_unlock(&state_mutex);
    test_screen_on = true;
}

static void chord_lock(bool power_first) {
    bool was_locked = hw_buttons_button_lock_is_locked();
    if (power_first) {
        handle_key_event(KEY_POWER, 1);
        handle_key_event(KEY_VOLUMEUP, 1);
    } else {
        handle_key_event(KEY_VOLUMEUP, 1);
        handle_key_event(KEY_POWER, 1);
    }
    expect_empty();
    assert(hw_buttons_button_lock_is_locked() == was_locked);
    handle_key_event(KEY_POWER, 0);
    assert(hw_buttons_button_lock_is_locked() == was_locked);
    handle_key_event(KEY_VOLUMEUP, 0);
    assert(hw_buttons_button_lock_is_locked() != was_locked);
}

static void test_both_chord_orders(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    assert(hw_buttons_button_lock_is_locked());
    assert(hw_buttons_consume_button_lock_changed());
    expect_empty();

    chord_lock(false);
    assert(!hw_buttons_button_lock_is_locked());
    assert(hw_buttons_consume_button_lock_changed());
    expect_empty();
}

static void test_single_keys(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);

    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_POWER, 0);
    take_event(BUTTON_MAPPING_POWER, BUTTON_MAPPING_GESTURE_TAP);
    assert(!hw_buttons_button_lock_is_locked());
    expect_empty();

    handle_key_event(KEY_VOLUMEUP, 1);
    handle_key_event(KEY_VOLUMEUP, 0);
    take_event(BUTTON_MAPPING_VOLUME_UP, BUTTON_MAPPING_GESTURE_TAP);
    assert(!hw_buttons_button_lock_is_locked());
    expect_empty();

    handle_key_event(KEY_PLAYPAUSE, 1);
    take_event(BUTTON_MAPPING_PLAY_PAUSE, BUTTON_MAPPING_GESTURE_TAP);
    handle_key_event(KEY_PLAYPAUSE, 0);
    expect_empty();
}

static void test_locked_actions_and_repeat(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    assert(hw_buttons_button_lock_is_locked());
    (void) hw_buttons_consume_button_lock_changed();

    handle_key_event(KEY_PLAYPAUSE, 1);
    expect_empty();
    handle_key_event(KEY_NEXTSONG, 1);
    expect_empty();
    handle_key_event(KEY_VOLUMEUP, 1);
    pthread_mutex_lock(&state_mutex);
    uint32_t pressed = mapped_button_states[BUTTON_MAPPING_VOLUME_UP].pressed_ms;
    apply_due_mapped_holds_locked(pressed + VOLUME_REPEAT_INITIAL_DELAY_MS, true);
    apply_due_mapped_holds_locked(pressed + VOLUME_REPEAT_INITIAL_DELAY_MS +
                                  VOLUME_REPEAT_INTERVAL_MS, true);
    pthread_mutex_unlock(&state_mutex);
    expect_empty();
    handle_key_event(KEY_VOLUMEUP, 0);
    handle_key_event(KEY_PLAYPAUSE, 0);
    handle_key_event(KEY_NEXTSONG, 0);
    expect_empty();
    assert(hw_buttons_button_lock_is_locked());
}

static void test_unlock_does_not_replay(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    (void) hw_buttons_consume_button_lock_changed();

    handle_key_event(KEY_PLAYPAUSE, 1);
    handle_key_event(KEY_NEXTSONG, 1);
    expect_empty();

    chord_lock(false);
    assert(!hw_buttons_button_lock_is_locked());
    (void) hw_buttons_consume_button_lock_changed();
    expect_empty();
    handle_key_event(KEY_PLAYPAUSE, 0);
    handle_key_event(KEY_NEXTSONG, 0);
    expect_empty();

    handle_key_event(KEY_PLAYPAUSE, 1);
    take_event(BUTTON_MAPPING_PLAY_PAUSE, BUTTON_MAPPING_GESTURE_TAP);
    handle_key_event(KEY_PLAYPAUSE, 0);
}

static void test_disabling_releases_lock(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    assert(hw_buttons_button_lock_is_locked());
    (void) hw_buttons_consume_button_lock_changed();

    handle_key_event(KEY_PLAYPAUSE, 1);
    hw_buttons_set_button_lock_enabled(false);
    assert(!hw_buttons_button_lock_is_enabled());
    assert(!hw_buttons_button_lock_is_locked());
    assert(hw_buttons_consume_button_lock_changed());
    expect_empty();
    handle_key_event(KEY_PLAYPAUSE, 0);
    expect_empty();

    handle_key_event(KEY_VOLUMEUP, 1);
    take_event(BUTTON_MAPPING_VOLUME_UP, BUTTON_MAPPING_GESTURE_TAP);
    handle_key_event(KEY_VOLUMEUP, 0);
}

static void test_screenshot_overlap(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    hw_buttons_set_screenshot_combo_enabled(true);

    handle_key_event(KEY_POWER, 1);
    handle_key_event((SCREENSHOT_CHORD_PREV ? KEY_PREVIOUSSONG : KEY_VOLUMEDOWN), 1);
    assert(hw_buttons_consume_screenshot());
    assert(!hw_buttons_button_lock_is_locked());
    expect_empty();
    handle_key_event((SCREENSHOT_CHORD_PREV ? KEY_PREVIOUSSONG : KEY_VOLUMEDOWN), 0);
    handle_key_event(KEY_POWER, 0);
    expect_empty();
    assert(!hw_buttons_consume_screenshot());

    chord_lock(true);
    assert(hw_buttons_button_lock_is_locked());
    assert(!hw_buttons_consume_screenshot());
    (void) hw_buttons_consume_button_lock_changed();
}

static void test_wrap_timestamps(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);

    handle_key_event(KEY_VOLUMEUP, 1);
    pthread_mutex_lock(&state_mutex);
    uint32_t now = mapped_button_states[BUTTON_MAPPING_VOLUME_UP].pressed_ms;
    physical_key_pressed_ms[KEY_VOLUMEUP] = now + (0xFFFFFFFFu - 39u);
    pthread_mutex_unlock(&state_mutex);
    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_POWER, 0);
    handle_key_event(KEY_VOLUMEUP, 0);
    assert(hw_buttons_button_lock_is_locked());
    (void) hw_buttons_consume_button_lock_changed();
    expect_empty();

    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    handle_key_event(KEY_VOLUMEUP, 1);
    pthread_mutex_lock(&state_mutex);
    now = mapped_button_states[BUTTON_MAPPING_VOLUME_UP].pressed_ms;
    physical_key_pressed_ms[KEY_VOLUMEUP] = now + (0xFFFFFFFFu - 399u);
    pthread_mutex_unlock(&state_mutex);
    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_POWER, 0);
    handle_key_event(KEY_VOLUMEUP, 0);
    assert(!hw_buttons_button_lock_is_locked());
    assert(event_count() >= 1);
}

static void test_emergency_power_while_locked(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    (void) hw_buttons_consume_button_lock_changed();
    assert(hw_buttons_button_lock_is_locked());

    handle_key_event(KEY_POWER, 1);
    pthread_mutex_lock(&state_mutex);
    uint32_t pressed = mapped_button_states[BUTTON_MAPPING_POWER].pressed_ms;
    apply_due_mapped_holds_locked(pressed + POWER_LONG_PRESS_MS, true);
    pthread_mutex_unlock(&state_mutex);
    take_event(BUTTON_MAPPING_POWER, BUTTON_MAPPING_GESTURE_HOLD_FIRST);
    expect_empty();
    handle_key_event(KEY_POWER, 0);
    expect_empty();
    assert(hw_buttons_button_lock_is_locked());

    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_POWER, 0);
    take_event(BUTTON_MAPPING_POWER, BUTTON_MAPPING_GESTURE_TAP);
    expect_empty();
}

static void test_screen_off_and_debounce(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    test_screen_on = false;
    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_VOLUMEUP, 1);
    expect_empty();
    assert(!hw_buttons_button_lock_is_locked());
    handle_key_event(KEY_VOLUMEUP, 0);
    assert(!hw_buttons_button_lock_is_locked());
    handle_key_event(KEY_POWER, 0);
    assert(hw_buttons_button_lock_is_locked());
    (void) hw_buttons_consume_button_lock_changed();
    expect_empty();
}

static void test_queued_action_cleared_on_lock(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    handle_key_event(KEY_PLAYPAUSE, 1);
    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_VOLUMEUP, 1);
    handle_key_event(KEY_POWER, 0);
    handle_key_event(KEY_VOLUMEUP, 0);
    assert(hw_buttons_button_lock_is_locked());
    expect_empty();
    handle_key_event(KEY_PLAYPAUSE, 0);
    expect_empty();
}

static void test_locked_power_ignores_volume_mapping(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    chord_lock(true);
    mapped_bindings[BUTTON_MAPPING_POWER].hold = BUTTON_MAPPING_ACTION_VOLUME_UP;
    volume_lock_screen_off = true;
    test_screen_on = false;
    handle_key_event(KEY_POWER, 1);
    pthread_mutex_lock(&state_mutex);
    apply_due_mapped_holds_locked(mapped_button_states[BUTTON_MAPPING_POWER].hold_due_ms, false);
    pthread_mutex_unlock(&state_mutex);
    hw_buttons_set_volume_lock_screen_off(true);
    take_event(BUTTON_MAPPING_POWER, BUTTON_MAPPING_GESTURE_HOLD_FIRST);
    handle_key_event(KEY_POWER, 0);
    expect_empty();
    mapped_bindings[BUTTON_MAPPING_POWER].hold = BUTTON_MAPPING_ACTION_DEFAULT;
}

static void test_knob_shortcut_and_input_loss(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(true);
    bool left = false, right = false, dropped = false;
    /* Knob-first is an ordinary detent, never retroactively a chord. */
    handle_knob_key_event(KEY_RIGHT, 1, &left, &right);
    take_event(BUTTON_MAPPING_VOLUME_UP, BUTTON_MAPPING_GESTURE_TAP);
    handle_key_event(KEY_POWER, 1);
    handle_key_event(KEY_POWER, 0);
    take_event(BUTTON_MAPPING_POWER, BUTTON_MAPPING_GESTURE_TAP);
    handle_knob_key_event(KEY_RIGHT, 0, &left, &right);
    assert(!button_locked);
    /* Hold Power first, then turn up, with no volume event. */
    handle_key_event(KEY_POWER, 1);
    handle_knob_key_event(KEY_RIGHT, 1, &left, &right);
    handle_knob_key_event(KEY_RIGHT, 0, &left, &right);
    handle_key_event(KEY_POWER, 0);
    expect_empty();
    assert(button_locked);
    /* Lost input cancels a partial chord rather than leaving it latched. */
    handle_key_event(KEY_POWER, 1);
    handle_knob_key_event(KEY_RIGHT, 1, &left, &right);
    assert(knob_consume_sync_event(EV_SYN, SYN_DROPPED, &dropped, &left, &right));
    assert(knob_consume_sync_event(EV_SYN, SYN_REPORT, &dropped, &left, &right));
    assert(!knob_volume_up_held && !lock_chord_active);
    handle_key_event(KEY_POWER, 0);
    assert(button_locked);
    handle_key_event(KEY_POWER, 1);
    handle_knob_key_event(KEY_RIGHT, 1, &left, &right);
    handle_knob_key_event(KEY_RIGHT, 0, &left, &right);
    handle_key_event(KEY_POWER, 0);
    assert(!button_locked);
    expect_empty();
    handle_key_event(KEY_POWER, 1);
    handle_knob_key_event(KEY_RIGHT, 1, &left, &right);
    struct pollfd fds[1] = {{0}};
    char paths[1][64] = {{0}};
    bool knobs[1] = {true}, lefts[1] = {false}, rights[1] = {true}, drops[1] = {false};
    unsigned char media[1] = {0};
    int count = 1;
    compact_button_fd_slot(fds, paths, knobs, lefts, rights, drops, media, &count, 0);
    assert(count == 0 && !knob_volume_up_held && !lock_chord_active);
    handle_key_event(KEY_POWER, 0);
    assert(!button_locked);
    expect_empty();
}

static void test_visual_lock_arming(void) {
    reset_state();
    hw_buttons_set_button_lock_enabled(false);
    hw_buttons_set_button_locked(true);
    assert(!hw_buttons_button_lock_is_locked());
    hw_buttons_set_button_lock_enabled(true);
    hw_buttons_set_button_locked(true);
    assert(hw_buttons_button_lock_is_locked());
    assert(hw_buttons_consume_button_lock_changed());
    hw_buttons_set_button_locked(false);
    assert(!hw_buttons_button_lock_is_locked());
    assert(hw_buttons_consume_button_lock_changed());
    hw_buttons_set_button_locked(true);
    hw_buttons_set_button_lock_enabled(false);
    assert(!hw_buttons_button_lock_is_locked());
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "both_chord_orders\n");
    test_visual_lock_arming();
    test_both_chord_orders();
    fprintf(stderr, "single_keys\n");
    test_single_keys();
    fprintf(stderr, "locked_actions_and_repeat\n");
    test_locked_actions_and_repeat();
    fprintf(stderr, "unlock_does_not_replay\n");
    test_unlock_does_not_replay();
    fprintf(stderr, "disabling_releases_lock\n");
    test_disabling_releases_lock();
    fprintf(stderr, "screenshot_overlap\n");
    test_screenshot_overlap();
    fprintf(stderr, "wrap_timestamps\n");
    test_wrap_timestamps();
    fprintf(stderr, "emergency_power\n");
    test_emergency_power_while_locked();
    fprintf(stderr, "screen_off_debounce\n");
    test_screen_off_and_debounce();
    fprintf(stderr, "queued_cleared\n");
    test_queued_action_cleared_on_lock();
    test_locked_power_ignores_volume_mapping();
    test_knob_shortcut_and_input_loss();
    puts("hw_buttons lock chord: PASS");
    return 0;
}
