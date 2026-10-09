#ifndef BUTTON_MAPPING_H
#define BUTTON_MAPPING_H

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_MAPPING_DEFAULT_DOUBLE_WINDOW_MS 300u
#define BUTTON_MAPPING_MIN_DOUBLE_WINDOW_MS 1u
#define BUTTON_MAPPING_MAX_DOUBLE_WINDOW_MS 2000u

/* Allocation-free physical-button gesture policy. The caller owns one context
 * and uses it from a single thread (normally the GUI thread). The input
 * recognizer remains responsible for translating evdev/key events into TAP,
 * HOLD_FIRST, and HOLD_REPEAT events. */
typedef enum {
    BUTTON_MAPPING_POWER = 0,
    BUTTON_MAPPING_PLAY_PAUSE,
    BUTTON_MAPPING_NEXT,
    BUTTON_MAPPING_PREVIOUS,
    BUTTON_MAPPING_VOLUME_UP,
    BUTTON_MAPPING_VOLUME_DOWN,
    BUTTON_MAPPING_BUTTON_COUNT
} button_mapping_button_t;

typedef enum {
    BUTTON_MAPPING_ACTION_DEFAULT = 0,
    BUTTON_MAPPING_ACTION_PLAY_PAUSE,
    BUTTON_MAPPING_ACTION_NEXT,
    BUTTON_MAPPING_ACTION_PREVIOUS,
    BUTTON_MAPPING_ACTION_VOLUME_UP,
    BUTTON_MAPPING_ACTION_VOLUME_DOWN,
    BUTTON_MAPPING_ACTION_SEEK_FORWARD,
    BUTTON_MAPPING_ACTION_SEEK_BACKWARD,
    BUTTON_MAPPING_ACTION_SCREEN_TOGGLE,
    BUTTON_MAPPING_ACTION_POWER_MENU,
    BUTTON_MAPPING_ACTION_NONE,
    BUTTON_MAPPING_ACTION_COUNT
} button_mapping_action_t;

typedef enum {
    BUTTON_MAPPING_GESTURE_TAP = 0,
    BUTTON_MAPPING_GESTURE_DOUBLE,
    BUTTON_MAPPING_GESTURE_HOLD_FIRST,
    BUTTON_MAPPING_GESTURE_HOLD_REPEAT
} button_mapping_gesture_t;

typedef struct {
    button_mapping_action_t tap;
    button_mapping_action_t double_tap;
    button_mapping_action_t hold;
} button_mapping_binding_t;

typedef struct {
    button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT];
    uint16_t double_window_ms[BUTTON_MAPPING_BUTTON_COUNT];
    struct {
        bool pending;
        uint32_t timestamp_ms;
    } pending_taps[BUTTON_MAPPING_BUTTON_COUNT];
} button_mapping_context_t;

/* Output callback. count is 1 for tap/hold-first and 2 for a double gesture
 * (two source taps; the consumer applies one assigned action). For hold-repeat it
 * is the recognizer's accumulated repeat count. first is true only for the
 * first event of a hold. timestamp_ms is the original source event timestamp
 * (for a delayed tap, the timestamp of that tap). DEFAULT is deliberately
 * delivered to the consumer so it can retain the existing native behavior. */
typedef void (*button_mapping_callback_t)(button_mapping_action_t action,
                                          button_mapping_button_t button,
                                          button_mapping_gesture_t gesture,
                                          unsigned count,
                                          bool first,
                                          uint32_t timestamp_ms,
                                          void *userdata);

/* Initialize all bindings to DEFAULT, all double windows to 300ms, and clear
 * pending gesture state. */
void button_mapping_init(button_mapping_context_t *context);

/* Reset all bindings and pending state to defaults. */
void button_mapping_reset(button_mapping_context_t *context);

/* Get or replace one button's complete binding. Invalid inputs return false.
 * Replacing one binding cancels only that button's deferred single tap. */
bool button_mapping_get(const button_mapping_context_t *context,
                        button_mapping_button_t button,
                        button_mapping_binding_t *out_binding);
bool button_mapping_set(button_mapping_context_t *context,
                        button_mapping_button_t button,
                        const button_mapping_binding_t *binding);

/* Set one button's double-tap window. Valid values are 1..2000ms. Invalid
 * context, button, or duration returns false. A valid update cancels only the
 * selected button's pending deferred tap. */
bool button_mapping_set_double_window_ms(button_mapping_context_t *context,
                                        button_mapping_button_t button,
                                        unsigned duration_ms);

/* Feed one normalized event, then emit resulting actions through callback.
 * TAPs are deferred for the selected button's configured window only when
 * double_tap is explicitly non-DEFAULT.
 * Source timestamps are uint32_t monotonic milliseconds; elapsed-time checks
 * are wrap-safe for intervals shorter than 2^31 ms. HOLD events first flush a
 * pending tap for that same button, then apply the hold binding. Non-repeatable
 * custom hold actions ignore HOLD_REPEAT; DEFAULT is passed through on every
 * repeat so native consumers can preserve their current repeat semantics.
 * Returns false for invalid context, button, gesture, or callback. */
bool button_mapping_event(button_mapping_context_t *context,
                          button_mapping_button_t button,
                          button_mapping_gesture_t gesture,
                          unsigned count,
                          uint32_t timestamp_ms,
                          button_mapping_callback_t callback,
                          void *userdata);

/* Flush expired deferred taps. Call on the owner thread, after feeding any
 * queued source events for the current poll. */
void button_mapping_poll(button_mapping_context_t *context,
                         uint32_t now_ms,
                         button_mapping_callback_t callback,
                         void *userdata);

#endif /* BUTTON_MAPPING_H */
