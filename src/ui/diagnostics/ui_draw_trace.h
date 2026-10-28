#ifndef UI_DRAW_TRACE_H
#define UI_DRAW_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "lvgl.h"

#ifdef UI_PERF_TRACE
/* Per-frame software draw cost by LVGL draw-task type, for UI_PERF_TRACE builds
 * only. Wraps the SW draw unit's dispatch callback, times the one task it
 * executes per call and sums nanoseconds, task count and clipped pixels per
 * class (see ui_draw_trace.c for the class list). Costs two clock reads per
 * task while a capture is armed and nothing otherwise. */
void ui_draw_trace_install(lv_display_t * disp);
/* Scroll blit outcome for the next committed frame: a=code (0 = shifted pixels, see lv_refr.c SB_*),
 * b=(int16 dy)<<16 | copy microseconds. */
void ui_draw_trace_note_blit(uint32_t code, uint32_t dy_us);
/* Times a tiny_ttf face's glyph callbacks; call once right after creating it. */
void ui_draw_trace_wrap_font(lv_font_t * font);
/* Frees the face's slot; call just before destroying it. */
void ui_draw_trace_unwrap_font(const lv_font_t * font);
void ui_draw_trace_arm(bool armed);   /* capture start/stop; start also clears the ring */
void ui_draw_trace_render_start(void); /* drops tasks counted outside a render */
void ui_draw_trace_commit(uint32_t frame_sequence, uint64_t timestamp_us); /* closes an LVGL render frame */
void ui_draw_trace_export_begin(void); /* freezes the ring for export */
bool ui_draw_trace_export_pending(void);
/* Writes the next "D," line; false only on a write error (the record is then retried). */
bool ui_draw_trace_export_next(FILE * f);
void ui_draw_trace_export_discard(void);
/* Times copying one framebuffer page out to RAM, back (content unchanged, inactive page
 * only) and RAM to RAM, to price a scroll blit; results go to the capture header. */
void ui_draw_trace_fb_bench(void);
void ui_draw_trace_fb_bench_results(uint32_t * read_us, uint32_t * write_us, uint32_t * ram_us);
uint32_t ui_draw_trace_record_size(void);
uint32_t ui_draw_trace_capacity(void);
#endif

#endif
