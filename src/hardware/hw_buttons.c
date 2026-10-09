#include "hw_buttons.h"
#include "button_mapping.h"
#include "debug_log.h"
#include "input_device_utils.h"
#include "backlight.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#endif

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include "ui_wake.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Typematic repeat for volume keys: initial delay followed by periodic repeats. */
#define VOLUME_REPEAT_INITIAL_DELAY_MS 350
#define VOLUME_REPEAT_INTERVAL_MS 60

/* Threshold to trigger power long-press (power-off menu) instead of a short tap (screen toggle). */
#define POWER_LONG_PRESS_MS 700

/* Hold-to-seek threshold/repeat for the Next button, matching LVGL's own
 * default long-press timing (LV_INDEV_DEF_LONG_PRESS_TIME/_REP_TIME) so
 * holding the physical button feels the same as holding the touch one. */
#define NEXT_SEEK_LONG_PRESS_MS 400
#define NEXT_SEEK_REPEAT_MS 100

#define BUTTON_EVENT_QUEUE_CAPACITY 64
#define BUTTON_EVENT_MAX_REPEAT_COUNT 255u
#define MAPPED_CUSTOM_HOLD_MS 500
#define MAPPED_CUSTOM_REPEAT_MS 100
#define SCREENSHOT_CHORD_WINDOW_MS 250

typedef struct {
    button_mapping_button_t button;
    button_mapping_gesture_t gesture;
    unsigned count;
    uint32_t timestamp_ms;
} queued_button_event_t;

typedef struct {
    bool held;
    bool tap_emitted;
    bool hold_fired;
    bool screenshot_chorded;
    bool tap_waiting_for_chord;
    bool hold_enabled;
    bool repeat_enabled;
    bool volume_hold_blocked;
    uint32_t pressed_ms;
    uint32_t hold_due_ms;
    uint32_t repeat_due_ms;
    uint32_t repeat_interval_ms;
} mapped_button_state_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static queued_button_event_t button_event_queue[BUTTON_EVENT_QUEUE_CAPACITY];
static unsigned button_event_queue_count = 0;
static button_mapping_binding_t mapped_bindings[BUTTON_MAPPING_BUTTON_COUNT];
static mapped_button_state_t mapped_button_states[BUTTON_MAPPING_BUTTON_COUNT];
static bool screenshot_requested = false;
/* Developer Options arms the chord; written from the UI thread, read here
 * under state_mutex like the rest of the key state. */
static bool screenshot_combo_enabled = false;
static bool screenshot_chord_used_for_power_hold = false;
static bool volume_lock_screen_off = false;
static bool physical_key_held[KEY_MAX + 1];
static uint32_t physical_key_pressed_ms[KEY_MAX + 1];

static uint32_t monotonic_ms(void);
static void queue_button_event_locked(button_mapping_button_t button,
                                      button_mapping_gesture_t gesture,
                                      unsigned count, uint32_t timestamp_ms);

/* The R3 II knob is reported as arrow-key pairs. Track each physical key per
 * evdev fd so a repeated KEY_DOWN without a release cannot become multiple
 * detents; unlike ordinary volume keys, these inputs never enter the hold
 * repeat path below. */
static void handle_knob_key_event(unsigned short code, int value, bool * left_held, bool * right_held) {
    bool * held = code == KEY_LEFT ? left_held : code == KEY_RIGHT ? right_held : NULL;
    if (!held) return;

    pthread_mutex_lock(&state_mutex);
    if (value == 1 && !*held) {
        /* Reverse the stock arrow mapping to match the reported wheel direction. */
        bool up = code == KEY_RIGHT;
        queue_button_event_locked(up ? BUTTON_MAPPING_VOLUME_UP : BUTTON_MAPPING_VOLUME_DOWN,
                                  BUTTON_MAPPING_GESTURE_TAP, 1, monotonic_ms());
        *held = true;
    } else if (value == 0) {
        *held = false;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* SYN_DROPPED invalidates the remainder of the current evdev packet. Discard
 * through SYN_REPORT and clear held guards there so a lost release cannot
 * suppress future knob impulses. */
static bool knob_consume_sync_event(unsigned short type, unsigned short code,
                                    bool * sync_dropped, bool * left_held, bool * right_held) {
    if (*sync_dropped) {
        if (type == EV_SYN && code == SYN_REPORT) {
            *sync_dropped = false;
            *left_held = false;
            *right_held = false;
        }
        return true;
    }
    if (type == EV_SYN && code == SYN_DROPPED) {
        *sync_dropped = true;
        return true;
    }
    return false;
}

/* The screenshot chord's second key. The R3II 2025 turns volume with a knob,
 * which cannot be held, so it chords Power with Previous instead. */
#if defined(BOARD_R3II_2025)
  #define SCREENSHOT_CHORD_PREV 1
#else
  #define SCREENSHOT_CHORD_PREV 0
#endif
#define SCREENSHOT_CHORD_VOLUME_DOWN (!SCREENSHOT_CHORD_PREV)

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000u +
                       (uint64_t) ts.tv_nsec / 1000000u);
}

static int event_priority(button_mapping_button_t button,
                          button_mapping_gesture_t gesture) {
    if (gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT) return 0;
    if (button == BUTTON_MAPPING_POWER) return 3;
    return 2;
}

static void remove_queued_event_locked(unsigned index) {
    if (index >= button_event_queue_count) return;
    for (unsigned i = index + 1; i < button_event_queue_count; ++i)
        button_event_queue[i - 1] = button_event_queue[i];
    button_event_queue_count--;
}

static void remove_queued_button_events_locked(button_mapping_button_t button) {
    for (unsigned i = 0; i < button_event_queue_count;) {
        if (button_event_queue[i].button == button) remove_queued_event_locked(i);
        else i++;
    }
}

static void remove_queued_volume_hold_events_locked(button_mapping_button_t button) {
    for (unsigned i = 0; i < button_event_queue_count;) {
        const button_mapping_gesture_t gesture = button_event_queue[i].gesture;
        if (button_event_queue[i].button == button &&
            (gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST ||
             gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT))
            remove_queued_event_locked(i);
        else
            i++;
    }
}

/* Caller holds state_mutex. Repeats merge only with the tail so FIFO source
 * ordering remains intact. When full, repeats are the first events evicted;
 * a power/tap edge can therefore never be starved by a held button. */
static void queue_button_event_locked(button_mapping_button_t button,
                                      button_mapping_gesture_t gesture,
                                      unsigned count, uint32_t timestamp_ms) {
    if ((unsigned) button >= BUTTON_MAPPING_BUTTON_COUNT || count == 0) return;
    if (mapped_button_states[button].volume_hold_blocked &&
        (gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST ||
         gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT))
        return;
    if (gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT &&
        count > BUTTON_EVENT_MAX_REPEAT_COUNT)
        count = BUTTON_EVENT_MAX_REPEAT_COUNT;
    if (gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT && button_event_queue_count) {
        queued_button_event_t *tail = &button_event_queue[button_event_queue_count - 1];
        if (tail->button == button && tail->gesture == gesture) {
            ui_wake_notify();
            tail->count = count >= BUTTON_EVENT_MAX_REPEAT_COUNT - tail->count
                              ? BUTTON_EVENT_MAX_REPEAT_COUNT : tail->count + count;
            return;
        }
    }
    if (button_event_queue_count == BUTTON_EVENT_QUEUE_CAPACITY) {
        if (gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT) return;
        unsigned victim = BUTTON_EVENT_QUEUE_CAPACITY;
        int incoming_priority = event_priority(button, gesture);
        for (unsigned i = 0; i < button_event_queue_count; ++i) {
            if (button_event_queue[i].gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT) {
                victim = i;
                break;
            }
            if (event_priority(button_event_queue[i].button,
                               button_event_queue[i].gesture) < incoming_priority &&
                victim == BUTTON_EVENT_QUEUE_CAPACITY)
                victim = i;
        }
        if (victim == BUTTON_EVENT_QUEUE_CAPACITY) {
            /* Preserve power edges under pathological tap floods. */
            if (button != BUTTON_MAPPING_POWER) return;
            for (unsigned i = 0; i < button_event_queue_count; ++i) {
                if (button_event_queue[i].button != BUTTON_MAPPING_POWER) {
                    victim = i;
                    break;
                }
            }
            if (victim == BUTTON_EVENT_QUEUE_CAPACITY) return;
        }
        remove_queued_event_locked(victim);
    }
    unsigned insert_at = button_event_queue_count;
    while (insert_at > 0 &&
           (int32_t) (button_event_queue[insert_at - 1].timestamp_ms - timestamp_ms) > 0) {
        button_event_queue[insert_at] = button_event_queue[insert_at - 1];
        insert_at--;
    }
    button_event_queue_count++;
    ui_wake_notify();
    queued_button_event_t *event = &button_event_queue[insert_at];
    event->button = button;
    event->gesture = gesture;
    event->count = count;
    event->timestamp_ms = timestamp_ms;
}

static bool binding_has_custom_hold_locked(button_mapping_button_t button) {
    return mapped_bindings[button].hold != BUTTON_MAPPING_ACTION_DEFAULT;
}

static bool binding_has_double_locked(button_mapping_button_t button) {
    return mapped_bindings[button].double_tap != BUTTON_MAPPING_ACTION_DEFAULT;
}

static bool hold_resolves_to_volume_locked(button_mapping_button_t button) {
    const button_mapping_action_t hold = mapped_bindings[button].hold;
    if (hold == BUTTON_MAPPING_ACTION_VOLUME_UP ||
        hold == BUTTON_MAPPING_ACTION_VOLUME_DOWN)
        return true;
    return hold == BUTTON_MAPPING_ACTION_DEFAULT &&
           (button == BUTTON_MAPPING_VOLUME_UP || button == BUTTON_MAPPING_VOLUME_DOWN);
}

static void latch_volume_hold_blocked_locked(button_mapping_button_t button,
                                             mapped_button_state_t *state) {
    if (state->volume_hold_blocked || !hold_resolves_to_volume_locked(button)) return;
    state->volume_hold_blocked = true;
    remove_queued_volume_hold_events_locked(button);
}

static void mapped_button_press_locked(button_mapping_button_t button,
                                       uint32_t now, bool screen_on,
                                       uint32_t native_hold_ms,
                                       uint32_t repeat_ms, bool defer_tap,
                                       bool wait_for_chord, bool chorded) {
    mapped_button_state_t *state = &mapped_button_states[button];
    memset(state, 0, sizeof(*state));
    state->held = true;
    state->pressed_ms = now;
    state->screenshot_chorded = chorded;
    state->tap_waiting_for_chord = wait_for_chord && !chorded;
    state->volume_hold_blocked = volume_lock_screen_off && !screen_on &&
                                 hold_resolves_to_volume_locked(button);
    bool custom_hold = binding_has_custom_hold_locked(button);
    bool custom_double = binding_has_double_locked(button);
    uint32_t hold_ms = native_hold_ms;
    if (!hold_ms && custom_hold) hold_ms = MAPPED_CUSTOM_HOLD_MS;
    if (hold_ms) {
        state->hold_enabled = true;
        state->hold_due_ms = now + hold_ms;
        state->repeat_enabled = repeat_ms != 0 || custom_hold;
        state->repeat_interval_ms = repeat_ms ? repeat_ms : MAPPED_CUSTOM_REPEAT_MS;
        if (state->repeat_enabled)
            state->repeat_due_ms = state->hold_due_ms + state->repeat_interval_ms;
    }
    bool defer = defer_tap || custom_hold || custom_double;
    if (!defer && !chorded) {
        queue_button_event_locked(button, BUTTON_MAPPING_GESTURE_TAP, 1, now);
        state->tap_emitted = true;
    }
}

static void mapped_button_release_locked(button_mapping_button_t button, uint32_t now) {
    mapped_button_state_t *state = &mapped_button_states[button];
    if (!state->held) return;
    if (!state->screenshot_chorded && !state->hold_fired && !state->tap_emitted) {
        queue_button_event_locked(button, BUTTON_MAPPING_GESTURE_TAP, 1,
                                  now);
        state->tap_emitted = true;
    }
    state->tap_waiting_for_chord = false;
    state->held = false;
}

/* Called from the reader's timed poll path and before each physical event so
 * a delayed release cannot turn an already completed hold into a tap. */
static void apply_due_mapped_holds_locked(uint32_t now, bool screen_on) {
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        mapped_button_state_t *state = &mapped_button_states[i];
        if (!state->held || state->screenshot_chorded) continue;
        if (volume_lock_screen_off && !screen_on)
            latch_volume_hold_blocked_locked((button_mapping_button_t) i, state);
        if (state->tap_waiting_for_chord &&
            (int32_t) (now - (state->pressed_ms + SCREENSHOT_CHORD_WINDOW_MS)) > 0) {
            if (!binding_has_custom_hold_locked((button_mapping_button_t) i) &&
                !binding_has_double_locked((button_mapping_button_t) i)) {
                queue_button_event_locked((button_mapping_button_t) i,
                                      BUTTON_MAPPING_GESTURE_TAP, 1, now);
                state->tap_emitted = true;
            }
            state->tap_waiting_for_chord = false;
        }
        if (!state->hold_enabled) continue;
        if (!state->hold_fired && (int32_t) (now - state->hold_due_ms) >= 0) {
            if (state->volume_hold_blocked) {
                /* The hold still wins tap disambiguation, but its volume
                 * action and repeats stay suppressed until release. */
                state->hold_fired = true;
                state->hold_enabled = false;
                state->repeat_enabled = false;
            } else {
                state->hold_fired = true;
                queue_button_event_locked((button_mapping_button_t) i,
                                          BUTTON_MAPPING_GESTURE_HOLD_FIRST, 1,
                                          state->hold_due_ms);
            }
        }
        if (state->hold_fired && state->repeat_enabled &&
            (int32_t) (now - state->repeat_due_ms) >= 0) {
            uint32_t elapsed = now - state->repeat_due_ms;
            uint32_t interval = state->repeat_interval_ms;
            uint32_t due_repeats = 1 + elapsed / interval;
            unsigned repeats = due_repeats > BUTTON_EVENT_MAX_REPEAT_COUNT
                                   ? BUTTON_EVENT_MAX_REPEAT_COUNT : (unsigned) due_repeats;
            queue_button_event_locked((button_mapping_button_t) i,
                                      BUTTON_MAPPING_GESTURE_HOLD_REPEAT,
                                      repeats, state->repeat_due_ms);
            state->repeat_due_ms += due_repeats * interval;
        }
    }
}

static void apply_due_mapped_holds(void) {
    uint32_t now = monotonic_ms();
    bool screen_on = backlight_screen_is_on();
    pthread_mutex_lock(&state_mutex);
    apply_due_mapped_holds_locked(now, screen_on);
    pthread_mutex_unlock(&state_mutex);
}

/* value: 1 = key down, 0 = key up (repeat events, value 2, are ignored --
 * this device's own repeat pacing below replaces whatever the kernel might
 * otherwise synthesize, and only actually fires for the two keys that
 * should repeat at all). */
static void handle_key_event(unsigned short code, int value) {
    if (code > KEY_MAX) return;
    /* See handle_knob_key_event() for lock ordering. */
    bool screen_on = backlight_screen_is_on();
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (value == 1) {
        if (physical_key_held[code]) {
            pthread_mutex_unlock(&state_mutex);
            return;
        }
        physical_key_held[code] = true;
        physical_key_pressed_ms[code] = now;
        apply_due_mapped_holds_locked(now, screen_on);
        switch (code) {
            case KEY_POWER: {
#if SCREENSHOT_CHORD_PREV
        bool reverse_chord = screenshot_combo_enabled && !screenshot_chord_used_for_power_hold &&
                    physical_key_held[KEY_PREVIOUSSONG] &&
                    !mapped_button_states[BUTTON_MAPPING_PREVIOUS].screenshot_chorded &&
                    (uint32_t) (now - physical_key_pressed_ms[KEY_PREVIOUSSONG]) <= SCREENSHOT_CHORD_WINDOW_MS;
#else
                bool reverse_chord = screenshot_combo_enabled && !screenshot_chord_used_for_power_hold &&
                    physical_key_held[KEY_VOLUMEDOWN] &&
                    !mapped_button_states[BUTTON_MAPPING_VOLUME_DOWN].screenshot_chorded &&
                    (uint32_t) (now - physical_key_pressed_ms[KEY_VOLUMEDOWN]) <= SCREENSHOT_CHORD_WINDOW_MS;
#endif
                mapped_button_press_locked(BUTTON_MAPPING_POWER, now, screen_on,
                                           POWER_LONG_PRESS_MS, 0, true, false,
                                           reverse_chord);
#if SCREENSHOT_CHORD_PREV
                if (reverse_chord) {
                    screenshot_chord_used_for_power_hold = true;
                    mapped_button_states[BUTTON_MAPPING_POWER].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_PREVIOUS].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_PREVIOUS].tap_waiting_for_chord = false;
                    remove_queued_button_events_locked(BUTTON_MAPPING_POWER);
                    remove_queued_button_events_locked(BUTTON_MAPPING_PREVIOUS);
                    screenshot_requested = true;
                    DBG_LOG("hw_buttons: screenshot chord Prev -> Power at t=%u\n", now);
                } else {
                    DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", now);
                }
#else
                if (reverse_chord) {
                    screenshot_chord_used_for_power_hold = true;
                    mapped_button_states[BUTTON_MAPPING_POWER].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_VOLUME_DOWN].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_VOLUME_DOWN].tap_waiting_for_chord = false;
                    remove_queued_button_events_locked(BUTTON_MAPPING_POWER);
                    remove_queued_button_events_locked(BUTTON_MAPPING_VOLUME_DOWN);
                    screenshot_requested = true;
                    DBG_LOG("hw_buttons: screenshot chord VolDown -> Power at t=%u\n", now);
                } else {
                    DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", now);
                }
#endif
                break;
            }
            /* Accessory remotes do not all speak the same code for the same
             * button: HID consumer-page transport controls arrive as any of
             * these depending on the cable's descriptor. */
            case KEY_PLAYPAUSE:
            case KEY_PLAY:
            case KEY_PLAYCD:
            case KEY_PAUSECD:
                mapped_button_press_locked(BUTTON_MAPPING_PLAY_PAUSE, now, screen_on,
                                           0, 0, false, false, false);
                break;
            case KEY_FASTFORWARD:
            case KEY_NEXTSONG: {
                mapped_button_press_locked(BUTTON_MAPPING_NEXT, now, screen_on,
                                           NEXT_SEEK_LONG_PRESS_MS,
                                           NEXT_SEEK_REPEAT_MS, true, false, false);
                break;
            }
#if SCREENSHOT_CHORD_PREV
            case KEY_REWIND:
                mapped_button_press_locked(BUTTON_MAPPING_PREVIOUS, now, screen_on,
                                           0, 0, false, false, false);
                break;
            case KEY_PREVIOUSSONG: {
                bool chord = screenshot_combo_enabled && physical_key_held[KEY_POWER] &&
                    !mapped_button_states[BUTTON_MAPPING_POWER].hold_fired;
                bool wait_for_chord = screenshot_combo_enabled;
                mapped_button_press_locked(BUTTON_MAPPING_PREVIOUS, now, screen_on, 0, 0,
                                           wait_for_chord, wait_for_chord, chord);
                if (chord) {
                    mapped_button_states[BUTTON_MAPPING_POWER].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_PREVIOUS].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_PREVIOUS].tap_waiting_for_chord = false;
                    remove_queued_button_events_locked(BUTTON_MAPPING_POWER);
                    remove_queued_button_events_locked(BUTTON_MAPPING_PREVIOUS);
                    if (!screenshot_chord_used_for_power_hold) {
                        screenshot_chord_used_for_power_hold = true;
                        screenshot_requested = true;
                        DBG_LOG("hw_buttons: screenshot chord Power -> Prev at t=%u\n", now);
                    }
                }
                break;
            }
#else
            case KEY_REWIND:
            case KEY_PREVIOUSSONG:
                mapped_button_press_locked(BUTTON_MAPPING_PREVIOUS, now, screen_on,
                                           0, 0, false, false, false);
                break;
#endif
            case KEY_VOLUMEUP:
                mapped_button_press_locked(BUTTON_MAPPING_VOLUME_UP, now, screen_on,
                                           VOLUME_REPEAT_INITIAL_DELAY_MS,
                                           VOLUME_REPEAT_INTERVAL_MS, false, false, false);
                break;
            case KEY_VOLUMEDOWN: {
                bool chord = SCREENSHOT_CHORD_VOLUME_DOWN && screenshot_combo_enabled &&
                    physical_key_held[KEY_POWER] && !mapped_button_states[BUTTON_MAPPING_POWER].hold_fired;
                bool wait_for_chord = SCREENSHOT_CHORD_VOLUME_DOWN && screenshot_combo_enabled;
                if (chord) {
                    mapped_button_press_locked(BUTTON_MAPPING_VOLUME_DOWN, now, screen_on,
                                               VOLUME_REPEAT_INITIAL_DELAY_MS,
                                               VOLUME_REPEAT_INTERVAL_MS, true, true, true);
                    mapped_button_states[BUTTON_MAPPING_POWER].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_VOLUME_DOWN].screenshot_chorded = true;
                    mapped_button_states[BUTTON_MAPPING_VOLUME_DOWN].tap_waiting_for_chord = false;
                    remove_queued_button_events_locked(BUTTON_MAPPING_POWER);
                    remove_queued_button_events_locked(BUTTON_MAPPING_VOLUME_DOWN);
                    if (!screenshot_chord_used_for_power_hold) {
                        screenshot_chord_used_for_power_hold = true;
                        screenshot_requested = true;
                        DBG_LOG("hw_buttons: screenshot chord Power -> VolDown at t=%u\n", now);
                    }
                } else {
                    if (wait_for_chord)
                        mapped_button_press_locked(BUTTON_MAPPING_VOLUME_DOWN, now, screen_on,
                                                   VOLUME_REPEAT_INITIAL_DELAY_MS,
                                                   VOLUME_REPEAT_INTERVAL_MS, true, true, false);
                    else
                        mapped_button_press_locked(BUTTON_MAPPING_VOLUME_DOWN, now, screen_on,
                                                   VOLUME_REPEAT_INITIAL_DELAY_MS,
                                                   VOLUME_REPEAT_INTERVAL_MS, false, false, false);
                }
                break;
            }
            default: break;
        }
    } else if (value == 0) {
        if (!physical_key_held[code]) {
            pthread_mutex_unlock(&state_mutex);
            return;
        }
        apply_due_mapped_holds_locked(now, screen_on);
        physical_key_held[code] = false;
        switch (code) {
            case KEY_POWER:
                mapped_button_release_locked(BUTTON_MAPPING_POWER, now);
                screenshot_chord_used_for_power_hold = false;
                break;
            case KEY_VOLUMEUP:
                mapped_button_release_locked(BUTTON_MAPPING_VOLUME_UP, now);
                break;
            case KEY_VOLUMEDOWN:
                mapped_button_release_locked(BUTTON_MAPPING_VOLUME_DOWN, now);
                break;
#if SCREENSHOT_CHORD_PREV
            case KEY_PREVIOUSSONG:
                mapped_button_release_locked(BUTTON_MAPPING_PREVIOUS, now);
                break;
            case KEY_REWIND:
                mapped_button_release_locked(BUTTON_MAPPING_PREVIOUS, now);
                break;
#else
            case KEY_PREVIOUSSONG:
            case KEY_REWIND:
                mapped_button_release_locked(BUTTON_MAPPING_PREVIOUS, now);
                break;
#endif
            case KEY_FASTFORWARD:
            case KEY_NEXTSONG:
                mapped_button_release_locked(BUTTON_MAPPING_NEXT, now);
                break;
            case KEY_PLAYPAUSE:
            case KEY_PLAY:
            case KEY_PLAYCD:
            case KEY_PAUSECD:
                mapped_button_release_locked(BUTTON_MAPPING_PLAY_PAUSE, now);
                break;
            default: break;
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Per open fd. UNNAMED means EVIOCGNAME failed and the next idle rescan
 * tries again. OTHER and AVRCP are kept, so a named USB remote is not
 * reclassified and a named headset is not dropped back onto the physical path. */
enum {
    HW_BUTTONS_MEDIA_OTHER = 0,
    HW_BUTTONS_MEDIA_AVRCP = 1,
    HW_BUTTONS_MEDIA_UNNAMED = 2,
};

/* BlueZ names its uinput node "<accessory> (AVRCP)". These four codes are the
 * transport buttons that node emits. Volume, fast-forward, and rewind are
 * not in that set and stay on the physical handler. */
static bool avrcp_transport_code(unsigned short code) {
    switch (code) {
        case KEY_PLAYCD:
        case KEY_PAUSECD:
        case KEY_NEXTSONG:
        case KEY_PREVIOUSSONG:
            return true;
        default:
            return false;
    }
}

/* One evdev event from a polled button fd. is_knob selects the detent path.
 * An AVRCP transport code, including release and repeat, goes to
 * bt_media_player and must not touch physical hold state. The host build
 * has no media-player object, so those codes keep the physical handler. */
static void handle_input_event(unsigned short type, unsigned short code, int value,
                               bool is_knob, unsigned char media_class,
                               bool * left_held, bool * right_held, bool * sync_dropped) {
    if (is_knob && knob_consume_sync_event(type, code, sync_dropped, left_held, right_held))
        return;
    if (type != EV_KEY) return;
    if (is_knob) {
        if (value == 2) return;
        handle_knob_key_event(code, value, left_held, right_held);
        return;
    }
    if (media_class == HW_BUTTONS_MEDIA_AVRCP && avrcp_transport_code(code)) {
#ifndef HOST_BUILD
        bt_media_player_dispatch_avrcp_key(code, value);
        return;
#else
        /* No bt_media_player in the host link. Keep the physical handler. */
        (void) 0;
#endif
    }
    if (value == 2) return;
    handle_key_event(code, value);
}

/* Media keys are what an accessory remote sends: a USB DSP cable or dongle
 * carries its own transport buttons, and its device name is the vendor's, so
 * these are found by what they can send rather than by what they are called. */
static bool device_sends_media_keys(int fd) {
    unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
#define HW_BUTTONS_HAS_KEY(code) \
    ((bits[(code) / (8 * sizeof(unsigned long))] >> ((code) % (8 * sizeof(unsigned long)))) & 1UL)
    return HW_BUTTONS_HAS_KEY(KEY_PLAYPAUSE) || HW_BUTTONS_HAS_KEY(KEY_NEXTSONG) ||
           HW_BUTTONS_HAS_KEY(KEY_PREVIOUSSONG) || HW_BUTTONS_HAS_KEY(KEY_PLAY) ||
           HW_BUTTONS_HAS_KEY(KEY_PLAYCD) || HW_BUTTONS_HAS_KEY(KEY_PAUSECD) ||
           HW_BUTTONS_HAS_KEY(KEY_FASTFORWARD) || HW_BUTTONS_HAS_KEY(KEY_REWIND);
#undef HW_BUTTONS_HAS_KEY
}

static bool evdev_name_has_avrcp_marker(const char * name) {
    return name && strstr(name, "(AVRCP)") != NULL;
}

/* 1 when the fd is a BlueZ AVRCP node, 0 when it has some other name,
 * -1 when the name ioctl fails. Callers map -1 to HW_BUTTONS_MEDIA_UNNAMED. */
static int evdev_fd_avrcp_classification(int fd) {
    char name[256];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return -1;
    name[sizeof(name) - 1] = '\0';
    return evdev_name_has_avrcp_marker(name) ? 1 : 0;
}

static unsigned char media_class_from_fd(int fd) {
    int classified = evdev_fd_avrcp_classification(fd);
    if (classified < 0) return HW_BUTTONS_MEDIA_UNNAMED;
    return classified ? HW_BUTTONS_MEDIA_AVRCP : HW_BUTTONS_MEDIA_OTHER;
}

static void retry_unnamed_media_fds(const struct pollfd * fds, unsigned char * media_class, int nfds) {
    for (int i = 0; i < nfds; i++) {
        if (media_class[i] != HW_BUTTONS_MEDIA_UNNAMED) continue;
        unsigned char next = media_class_from_fd(fds[i].fd);
        if (next == HW_BUTTONS_MEDIA_UNNAMED) continue;
        media_class[i] = next;
        DBG_LOG("hw_buttons: fd_index=%d classified avrcp=%d\n",
                i, next == HW_BUTTONS_MEDIA_AVRCP);
    }
}

#define HW_BUTTONS_MAX_FDS 12
/* Cheap: a readdir plus one ioctl per unopened event node, and only when no
 * key was pressed in that window. */
#define MEDIA_KEY_RESCAN_INTERVAL_MS 2000

/* Adds any event device with transport keys that is not already open. Called
 * again on a cadence because an accessory can be plugged in long after boot,
 * which the original one-shot enumeration could never see. An "(AVRCP)" node
 * stays open here: its transport codes are dispatched to bt_media_player and
 * its volume and fast-forward/rewind keys still use the physical handler.
 * fds whose name ioctl failed last time are classified again on this pass. */
static int scan_media_key_devices(struct pollfd * fds, int nfds, char paths[][64],
                                 unsigned char * media_class) {
    retry_unnamed_media_fds(fds, media_class, nfds);

    DIR * dir = opendir("/dev/input");
    if (!dir) return nfds;

    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL && nfds < HW_BUTTONS_MAX_FDS) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        /* d_name can be longer than this slot. Skip it rather than opening
         * a truncated path. Event nodes are "event" plus a short index. */
        size_t name_len = strlen(entry->d_name);
        if (name_len == 0 || name_len >= 64 - 11) continue;
        char path[64];
        memcpy(path, "/dev/input/", 11);
        memcpy(path + 11, entry->d_name, name_len + 1);

        bool already_open = false;
        for (int i = 0; i < nfds; i++)
            if (strcmp(paths[i], path) == 0) { already_open = true; break; }
        if (already_open) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!device_sends_media_keys(fd)) {
            close(fd);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        media_class[nfds] = media_class_from_fd(fd);
        memcpy(paths[nfds], path, name_len + 12);
        DBG_LOG("hw_buttons: media-key device %s opened at fd_index=%d avrcp=%d\n",
                path, nfds, media_class[nfds] == HW_BUTTONS_MEDIA_AVRCP);
        nfds++;
    }
    closedir(dir);
    return nfds;
}

/* Drop fds[index] from the live prefix and slide the last live slot down.
 * Per-fd flags move with that slot; the vacated index is cleared so the
 * next open cannot inherit a stale knob bit or media classification. */
static void compact_button_fd_slot(struct pollfd * fds, char paths[][64],
                                  bool * knob, bool * knob_left_held,
                                  bool * knob_right_held, bool * knob_sync_dropped,
                                  unsigned char * media_class, int * nfds, int index) {
    (*nfds)--;
    if (index != *nfds) {
        fds[index] = fds[*nfds];
        knob[index] = knob[*nfds];
        knob_left_held[index] = knob_left_held[*nfds];
        knob_right_held[index] = knob_right_held[*nfds];
        knob_sync_dropped[index] = knob_sync_dropped[*nfds];
        media_class[index] = media_class[*nfds];
        memcpy(paths[index], paths[*nfds], sizeof(paths[index]));
    }
    knob[*nfds] = false;
    knob_left_held[*nfds] = false;
    knob_right_held[*nfds] = false;
    knob_sync_dropped[*nfds] = false;
    media_class[*nfds] = HW_BUTTONS_MEDIA_OTHER;
    paths[*nfds][0] = '\0';
}

static void * hw_buttons_thread_func(void * arg) {
    (void) arg;

    static const struct { const char * name; const char * label; bool knob; } BUTTON_DEVICES[] = {
        { "md-gpio-keys", "md-gpio-keys", false },
        { "jz adc keyboard", "jz adc keyboard", false },
        /* Wired headphone inline remote (earpods_adc). */
        { "earpods_adc", "earpods_adc", false },
#if defined(BOARD_R3II_2025)
        /* Volume knob: arrow events are handled as single detent impulses. */
        { "sa-ring-keys", "sa-ring-keys", true },
#endif
    };

    /* O_NONBLOCK prevents empty read queues on one device from blocking poll
     * and starving inputs from the other button devices. */
    struct pollfd fds[HW_BUTTONS_MAX_FDS];
    char paths[HW_BUTTONS_MAX_FDS][64];
    /* Arrow keys mean volume only on the knob, never on an accessory.
     * media_class selects AVRCP transport dispatch per fd. Named built-in
     * nodes are OTHER without an ioctl. */
    bool knob[HW_BUTTONS_MAX_FDS] = { false };
    bool knob_left_held[HW_BUTTONS_MAX_FDS] = { false };
    bool knob_right_held[HW_BUTTONS_MAX_FDS] = { false };
    bool knob_sync_dropped[HW_BUTTONS_MAX_FDS] = { false };
    unsigned char media_class[HW_BUTTONS_MAX_FDS] = { HW_BUTTONS_MEDIA_OTHER };
    int nfds = 0;
    int found = 0;
    for (size_t i = 0; i < sizeof(BUTTON_DEVICES) / sizeof(BUTTON_DEVICES[0]); i++) {
        char path[64];
        if (!find_input_device_by_name(BUTTON_DEVICES[i].name, path, sizeof(path))) continue;
        found++;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "hw_buttons: failed to open %s\n", path);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        knob[nfds] = BUTTON_DEVICES[i].knob;
        media_class[nfds] = HW_BUTTONS_MEDIA_OTHER;
        snprintf(paths[nfds], sizeof(paths[nfds]), "%s", path);
        DBG_LOG("hw_buttons: fd_index=%d -> %s (%s)\n", nfds, path, BUTTON_DEVICES[i].label);
        nfds++;
    }

    /* The built-in keys are named and known; an accessory's remote is not, so
     * it is picked up by capability here and on every rescan below. */
    nfds = scan_media_key_devices(fds, nfds, paths, media_class);

    if (found == 0 && nfds == 0) {
        fprintf(stderr, "hw_buttons: no physical button input devices found, hardware keys disabled\n");
        return NULL;
    }

    if (nfds == 0) return NULL;

    printf("hw_buttons: listening for physical button presses\n");

    while (1) {
        pthread_mutex_lock(&state_mutex);
        bool mapped_timer_pending = false;
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
            const mapped_button_state_t *state = &mapped_button_states[i];
            if (state->held && (state->tap_waiting_for_chord ||
                (state->hold_enabled && (!state->hold_fired || state->repeat_enabled)))) {
                mapped_timer_pending = true;
                break;
            }
        }
        pthread_mutex_unlock(&state_mutex);

        /* The timeout services active gestures and rescans for accessories
         * that may have been plugged in after startup. */
        int ret = poll(fds, (nfds_t) nfds,
                        mapped_timer_pending
                            ? VOLUME_REPEAT_INTERVAL_MS : MEDIA_KEY_RESCAN_INTERVAL_MS);
        if (ret == 0) {
            apply_due_mapped_holds();
            nfds = scan_media_key_devices(fds, nfds, paths, media_class);
            continue;
        }
        if (ret < 0) continue;

        for (int i = 0; i < nfds; i++) {
            /* An unplugged accessory reports an error rather than data. Its
             * slot is closed and the last one moved down, so a cable can come
             * and go without leaking descriptors or wedging the poll set. */
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                DBG_LOG("hw_buttons: %s went away, closing fd_index=%d\n", paths[i], i);
                close(fds[i].fd);
                compact_button_fd_slot(fds, paths, knob, knob_left_held, knob_right_held,
                                       knob_sync_dropped, media_class, &nfds, i);
                i--;
                continue;
            }
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
                DBG_LOG("hw_buttons: raw event fd_index=%d type=%u code=%u value=%d\n", i, ev.type, ev.code, ev.value);
                handle_input_event(ev.type, ev.code, ev.value, knob[i], media_class[i],
                                   &knob_left_held[i], &knob_right_held[i], &knob_sync_dropped[i]);
                apply_due_mapped_holds();
            }
        }
    }

    return NULL;
}

void hw_buttons_init(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, hw_buttons_thread_func, NULL);
    pthread_detach(thread);
}

void hw_buttons_set_bindings(const button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT]) {
    if (!bindings) return;
    pthread_mutex_lock(&state_mutex);
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        const button_mapping_binding_t *binding = &bindings[i];
        mapped_bindings[i].tap = (unsigned) binding->tap < BUTTON_MAPPING_ACTION_COUNT
                                      ? binding->tap : BUTTON_MAPPING_ACTION_DEFAULT;
        mapped_bindings[i].double_tap = (unsigned) binding->double_tap < BUTTON_MAPPING_ACTION_COUNT
                                            ? binding->double_tap : BUTTON_MAPPING_ACTION_DEFAULT;
        mapped_bindings[i].hold = (unsigned) binding->hold < BUTTON_MAPPING_ACTION_COUNT
                                      ? binding->hold : BUTTON_MAPPING_ACTION_DEFAULT;
    }
    /* Events have already been recognized against the previous policy. Do
     * not let them execute after settings install a new binding set. */
    button_event_queue_count = 0;
    memset(mapped_button_states, 0, sizeof(mapped_button_states));
    pthread_mutex_unlock(&state_mutex);
}

bool hw_buttons_consume_event(hw_button_event_t *event) {
    if (!event) return false;
    pthread_mutex_lock(&state_mutex);
    if (!button_event_queue_count) {
        pthread_mutex_unlock(&state_mutex);
        return false;
    }
    queued_button_event_t queued = button_event_queue[0];
    remove_queued_event_locked(0);
    event->button = queued.button;
    event->gesture = queued.gesture;
    event->count = queued.count;
    event->timestamp_ms = queued.timestamp_ms;
    pthread_mutex_unlock(&state_mutex);
    return true;
}

void hw_buttons_set_screenshot_combo_enabled(bool enabled) {
    pthread_mutex_lock(&state_mutex);
    screenshot_combo_enabled = enabled;
    pthread_mutex_unlock(&state_mutex);
}

void hw_buttons_set_volume_lock_screen_off(bool enabled) {
    bool screen_on = backlight_screen_is_on();
    pthread_mutex_lock(&state_mutex);
    volume_lock_screen_off = enabled;
    if (enabled && !screen_on) {
        for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
            mapped_button_state_t *state = &mapped_button_states[i];
            if (state->held) latch_volume_hold_blocked_locked((button_mapping_button_t) i, state);
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

bool hw_buttons_consume_screenshot(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = screenshot_requested;
    screenshot_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
