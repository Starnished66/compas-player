#ifndef COMPAS_SELECTOR_INPUT_H
#define COMPAS_SELECTOR_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct selector_input selector_input_t;

typedef enum {
    SELECTOR_INPUT_TOUCH_DOWN = 0,
    SELECTOR_INPUT_TOUCH_MOVE,
    SELECTOR_INPUT_TOUCH_UP,
    SELECTOR_INPUT_CANCEL,
    SELECTOR_INPUT_NAV_UP,
    SELECTOR_INPUT_NAV_DOWN,
    SELECTOR_INPUT_ACTIVATE
} selector_input_event_type_t;

typedef struct {
    selector_input_event_type_t type;
    int x;                  /* Screen pixels for touch events; otherwise zero. */
    int y;
    int value;              /* Navigation repeat: 0 press, 1 repeated step. */
    uint64_t monotonic_ms;  /* Timestamp at normalization. */
} selector_input_event_t;

typedef void (*selector_input_event_cb)(const selector_input_event_t * event,
                                        void * userdata);

typedef enum {
    SELECTOR_INPUT_TEST_KEYS = 0,
    SELECTOR_INPUT_TEST_TOUCH
} selector_input_test_device_t;

/* Create an input context for the selector's physical screen dimensions.
 * Open accepts only the board's named, built-in evdev devices. At least one
 * physical touchscreen or key device must open successfully. */
selector_input_t * selector_input_create(int screen_width, int screen_height);
/* Creates the same state machine without opening evdev nodes. */
selector_input_t * selector_input_create_for_test(int screen_width, int screen_height);
bool selector_input_open(selector_input_t * input);
void selector_input_close(selector_input_t * input);
void selector_input_destroy(selector_input_t * input);

/* Wait no longer than timeout_ms, then deliver every currently readable
 * normalized event to callback before returning. Return 0 when at least one
 * device remains open, 1 when the bounded read limit was reached and more
 * input may be pending (deadline must not expire yet), or -1 when all input
 * devices have been lost. */
int selector_input_poll(selector_input_t * input, int timeout_ms,
                        selector_input_event_cb callback, void * userdata);

/* Normalized injection seam for external host tests. It shares the production
 * state machine without constructing the target's 16-byte MIPS input_event.
 * `key` is KEY_*'s Linux numeric code; value is 0 release, 1 press, 2 repeat.
 * Pass startup_held=true only for initial key-state seeding from EVIOCGKEY.
 * Touch coordinates are raw values in the configured ABS range, and contacts
 * reports active contacts in the current SYN_REPORT frame. The default test
 * range is the screen pixel range; set a different range before injection to
 * exercise axis scaling. */
bool selector_input_inject_key(selector_input_t * input, unsigned int key,
                               int value, uint64_t monotonic_ms,
                               bool startup_held);
/* Feed one normalized evdev record field-by-field through the production
 * parser. Type and code use Linux EV, KEY, and ABS numeric constants. The API
 * deliberately does not expose or construct struct input_event. */
bool selector_input_inject_record(selector_input_t * input,
                                  selector_input_test_device_t device,
                                  uint16_t type, uint16_t code, int32_t value,
                                  uint64_t monotonic_ms);
bool selector_input_inject_touch(selector_input_t * input, int raw_x, int raw_y,
                                 unsigned int contacts, bool changed,
                                 uint64_t monotonic_ms, bool startup_held);
bool selector_input_set_test_touch_range(selector_input_t * input,
                                         int x_min, int x_max,
                                         int y_min, int y_max);
void selector_input_inject_syn_dropped(selector_input_t * input,
                                       uint64_t monotonic_ms);

#ifdef __cplusplus
}
#endif

#endif /* COMPAS_SELECTOR_INPUT_H */
