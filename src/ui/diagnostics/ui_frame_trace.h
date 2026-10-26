#ifndef UI_FRAME_TRACE_H
#define UI_FRAME_TRACE_H

#include "lvgl.h"
#include <stdint.h>

#ifdef UI_PERF_TRACE
typedef enum {
    UI_FRAME_TRACE_EVENT_SYNC_BEGIN = 1,
    UI_FRAME_TRACE_EVENT_SYNC_END,
    UI_FRAME_TRACE_EVENT_DRAWFLUSH,
    UI_FRAME_TRACE_EVENT_PAN_BEGIN,
    UI_FRAME_TRACE_EVENT_PAN_END,
    UI_FRAME_TRACE_EVENT_HANDLER_BEGIN,
    UI_FRAME_TRACE_EVENT_HANDLER_END,
    UI_FRAME_TRACE_EVENT_SLEEP_BEGIN,
    UI_FRAME_TRACE_EVENT_SLEEP_END,
    UI_FRAME_TRACE_EVENT_DRAIN_BEGIN,
    UI_FRAME_TRACE_EVENT_DRAIN_END,
    UI_FRAME_TRACE_EVENT_PREPARE_BEGIN,
    UI_FRAME_TRACE_EVENT_PREPARE_END,
    UI_FRAME_TRACE_EVENT_CACHE,
    UI_FRAME_TRACE_EVENT_COMPOSITOR_BEGIN,
    UI_FRAME_TRACE_EVENT_COMPOSITOR_PRESENT,
    UI_FRAME_TRACE_EVENT_CONTROL_POLL,
    UI_FRAME_TRACE_EVENT_COMPOSITOR_END,
    UI_FRAME_TRACE_EVENT_SPAN,
    UI_FRAME_TRACE_EVENT_HANDLER_FAULTS,
    UI_FRAME_TRACE_EVENT_POINTER_SAMPLE,
    UI_FRAME_TRACE_EVENT_GESTURE_ORIGIN,
    UI_FRAME_TRACE_EVENT_DRAWER_CLAIM
} ui_frame_trace_event_kind_t;

/* POINTER_SAMPLE uses a=(uint16_t)x<<16|(uint16_t)y, b=pressed (0/1).
 * Records accepted changed raw coordinates/state before gesture processing.
 * GESTURE_ORIGIN uses the same packed point, b=candidate bits:
 * Player=1, Back=2, Home=4, Drawer=8. DRAWER_CLAIM marks drag acceptance.
 * SPAN uses a=ui_trace_span_t, b=elapsed microseconds. HANDLER_FAULTS
 * follows a long HANDLER_END with a=major, b=minor UI-thread page-fault
 * deltas (RUSAGE_THREAD), excluding concurrent worker faults. */
typedef enum {
    UI_TRACE_SPAN_PLAYER_CACHE = 1,
    UI_TRACE_SPAN_BACK_TARGET,
    UI_TRACE_SPAN_STATIC_SNAPSHOT,
    UI_TRACE_SPAN_DRAWER_SNAPSHOT,
    UI_TRACE_SPAN_BLEND_STATUS_BAR,
    UI_TRACE_SPAN_BLEND_HOME_BAR,
    UI_TRACE_SPAN_NAV_PUSH,
    UI_TRACE_SPAN_STATIC_LOOKUP,
    UI_TRACE_SPAN_TARGET_DUP,
    UI_TRACE_SPAN_FILE_ROWS
} ui_trace_span_t;

uint64_t ui_perf_now_us(void);
#define UI_TRACE_SPAN_BEGIN(var) uint64_t var = ui_perf_now_us()
#define UI_TRACE_SPAN_END(id, var) \
    ui_frame_trace_event(UI_FRAME_TRACE_EVENT_SPAN, (id), (uint32_t) (ui_perf_now_us() - (var)))

/* UI-thread only. Timestamps are monotonic microseconds. PAN_BEGIN uses a=0
 * for normal and a=1 for external; PAN_END uses a=success, b=y offset.
 * DRAWFLUSH uses a=draw duration (microseconds), b=refreshed pixels.
 * HANDLER_END/SLEEP_END use a=the caller's requested next-wait duration. */
void ui_frame_trace_event(ui_frame_trace_event_kind_t kind, uint32_t a, uint32_t b);
void ui_frame_trace_install(lv_display_t * disp);
void ui_frame_trace_compositor_present(uint64_t work_us);
#else
static inline void ui_frame_trace_event(int kind, uint32_t a, uint32_t b) {
    (void) kind; (void) a; (void) b;
}
static inline void ui_frame_trace_install(lv_display_t * disp) { (void) disp; }
#define UI_TRACE_SPAN_BEGIN(var) do { } while (0)
#define UI_TRACE_SPAN_END(id, var) do { } while (0)
#endif

#endif
