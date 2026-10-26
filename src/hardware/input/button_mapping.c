#include "button_mapping.h"

#include <stddef.h>
#include <string.h>

static bool valid_button(button_mapping_button_t button) {
    return (unsigned) button < BUTTON_MAPPING_BUTTON_COUNT;
}

static bool valid_action(button_mapping_action_t action) {
    return (unsigned) action < BUTTON_MAPPING_ACTION_COUNT;
}

static bool valid_binding(const button_mapping_binding_t *binding) {
    return binding && valid_action(binding->tap) &&
           valid_action(binding->double_tap) && valid_action(binding->hold);
}

static bool action_repeats(button_mapping_action_t action) {
    switch (action) {
        case BUTTON_MAPPING_ACTION_DEFAULT:
        case BUTTON_MAPPING_ACTION_VOLUME_UP:
        case BUTTON_MAPPING_ACTION_VOLUME_DOWN:
        case BUTTON_MAPPING_ACTION_SEEK_FORWARD:
        case BUTTON_MAPPING_ACTION_SEEK_BACKWARD:
            return true;
        default:
            return false;
    }
}

static void emit(button_mapping_callback_t callback, void *userdata,
                 button_mapping_action_t action,
                 button_mapping_button_t button,
                 button_mapping_gesture_t gesture,
                 unsigned count, bool first, uint32_t timestamp_ms) {
    callback(action, button, gesture, count, first, timestamp_ms, userdata);
}

void button_mapping_init(button_mapping_context_t *context) {
    if (!context) return;
    memset(context, 0, sizeof(*context));
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i)
        context->double_window_ms[i] = BUTTON_MAPPING_DEFAULT_DOUBLE_WINDOW_MS;
}

void button_mapping_reset(button_mapping_context_t *context) {
    button_mapping_init(context);
}

bool button_mapping_get(const button_mapping_context_t *context,
                        button_mapping_button_t button,
                        button_mapping_binding_t *out_binding) {
    if (!context || !out_binding || !valid_button(button)) return false;
    *out_binding = context->bindings[button];
    return true;
}

bool button_mapping_set(button_mapping_context_t *context,
                        button_mapping_button_t button,
                        const button_mapping_binding_t *binding) {
    if (!context || !valid_button(button) || !valid_binding(binding)) return false;
    context->bindings[button] = *binding;
    context->pending_taps[button].pending = false;
    context->pending_taps[button].timestamp_ms = 0;
    return true;
}

bool button_mapping_set_double_window_ms(button_mapping_context_t *context,
                                        button_mapping_button_t button,
                                        unsigned duration_ms) {
    if (!context || !valid_button(button) ||
        duration_ms < BUTTON_MAPPING_MIN_DOUBLE_WINDOW_MS ||
        duration_ms > BUTTON_MAPPING_MAX_DOUBLE_WINDOW_MS) return false;
    context->double_window_ms[button] = (uint16_t) duration_ms;
    context->pending_taps[button].pending = false;
    context->pending_taps[button].timestamp_ms = 0;
    return true;
}

static void emit_pending_single(button_mapping_context_t *context,
                                button_mapping_button_t button,
                                button_mapping_callback_t callback,
                                void *userdata) {
    const uint32_t timestamp_ms = context->pending_taps[button].timestamp_ms;
    context->pending_taps[button].pending = false;
    context->pending_taps[button].timestamp_ms = 0;
    emit(callback, userdata, context->bindings[button].tap, button,
         BUTTON_MAPPING_GESTURE_TAP, 1, false, timestamp_ms);
}

bool button_mapping_event(button_mapping_context_t *context,
                          button_mapping_button_t button,
                          button_mapping_gesture_t gesture,
                          unsigned count,
                          uint32_t timestamp_ms,
                          button_mapping_callback_t callback,
                          void *userdata) {
    if (!context || !valid_button(button) || !callback) return false;
    if (gesture != BUTTON_MAPPING_GESTURE_TAP &&
        gesture != BUTTON_MAPPING_GESTURE_HOLD_FIRST &&
        gesture != BUTTON_MAPPING_GESTURE_HOLD_REPEAT) return false;
    if (gesture == BUTTON_MAPPING_GESTURE_HOLD_REPEAT && count == 0) return false;

    button_mapping_binding_t *binding = &context->bindings[button];
    if (gesture == BUTTON_MAPPING_GESTURE_TAP) {
        if (binding->double_tap == BUTTON_MAPPING_ACTION_DEFAULT) {
            /* DEFAULT double behavior means no double gesture is configured;
             * preserve the old immediate single-tap response. */
            emit(callback, userdata, binding->tap, button,
                 BUTTON_MAPPING_GESTURE_TAP, 1, false, timestamp_ms);
            return true;
        }

        if (context->pending_taps[button].pending) {
            uint32_t elapsed = timestamp_ms - context->pending_taps[button].timestamp_ms;
            if (elapsed < context->double_window_ms[button]) {
                context->pending_taps[button].pending = false;
                context->pending_taps[button].timestamp_ms = 0;
                emit(callback, userdata, binding->double_tap, button,
                     BUTTON_MAPPING_GESTURE_DOUBLE, 2, false, timestamp_ms);
                return true;
            }
            /* The source timestamp, rather than when this thread happened to
             * drain its queue, decides whether this is a late second tap. */
            emit_pending_single(context, button, callback, userdata);
        }
        context->pending_taps[button].pending = true;
        context->pending_taps[button].timestamp_ms = timestamp_ms;
        return true;
    }

    /* A hold is a separate physical press, and takes over from tap handling.
     * Flush an earlier completed tap before dispatching this hold. */
    if (context->pending_taps[button].pending)
        emit_pending_single(context, button, callback, userdata);

    if (gesture == BUTTON_MAPPING_GESTURE_HOLD_FIRST) {
        emit(callback, userdata, binding->hold, button, gesture, 1, true,
             timestamp_ms);
        return true;
    }

    if (action_repeats(binding->hold)) {
        emit(callback, userdata, binding->hold, button, gesture, count, false,
             timestamp_ms);
    }
    return true;
}

void button_mapping_poll(button_mapping_context_t *context,
                         uint32_t now_ms,
                         button_mapping_callback_t callback,
                         void *userdata) {
    if (!context || !callback) return;
    for (unsigned i = 0; i < BUTTON_MAPPING_BUTTON_COUNT; ++i) {
        if (!context->pending_taps[i].pending) continue;
        if ((uint32_t) (now_ms - context->pending_taps[i].timestamp_ms) >=
            context->double_window_ms[i]) {
            emit_pending_single(context, (button_mapping_button_t) i,
                                callback, userdata);
        }
    }
}
