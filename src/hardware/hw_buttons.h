#ifndef HW_BUTTONS_H
#define HW_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>
#include "button_mapping.h"

/* Normalized physical-button gesture delivered to the GUI thread. Timestamp
 * is monotonic milliseconds from the evdev reader and count is one except for
 * coalesced/repeated HOLD_REPEAT events. */
typedef struct {
    button_mapping_button_t button;
    button_mapping_gesture_t gesture;
    unsigned count;
    uint32_t timestamp_ms;
} hw_button_event_t;

/* Starts a background thread reading the R1's physical volume/skip/play-pause
 * buttons. The reader normalizes physical input into a bounded event queue;
 * the GUI drains it and applies mappings on its own thread. */
void hw_buttons_init(void);

/* Replace the recognizer policy. A binding update discards queued events and
 * cancels in-progress gestures so an event recognized under the old policy
 * cannot run under the new one. Call from the GUI thread. */
void hw_buttons_set_bindings(const button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT]);

/* Pop one normalized event in source order. The GUI should drain this queue
 * before polling its button_mapping_context_t, then use CLOCK_MONOTONIC for
 * button_mapping_poll(). This is the physical-button event source for mapped
 * GUI actions. */
bool hw_buttons_consume_event(hw_button_event_t *event);

/* True once for a Power + Volume Down screenshot chord (Power + Previous on
 * the R3II 2025, whose volume knob cannot be held), then reset. One
 * screenshot is accepted per Power hold; further presses of the second key
 * during that hold are ignored. */
bool hw_buttons_consume_screenshot(void);
#if defined(BOARD_R3II_2025)
  #define HW_BUTTONS_SCREENSHOT_CHORD_NAME "Power + Previous"
#else
  #define HW_BUTTONS_SCREENSHOT_CHORD_NAME "Power + Vol Down"
#endif

/* Arms the screenshot chord. While disarmed (the default)
 * both keys keep their ordinary meaning and nothing is suppressed. */
void hw_buttons_set_screenshot_combo_enabled(bool enabled);

/* Enable the screen-off volume policy. Tap gestures remain available for
 * mappings to non-volume actions; the reader stops only holds that resolve to
 * volume actions, and never resumes a blocked hold after wake. */
void hw_buttons_set_volume_lock_screen_off(bool enabled);

#endif /* HW_BUTTONS_H */
