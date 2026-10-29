#ifndef UI_BLIT_VERIFY_H
#define UI_BLIT_VERIFY_H

#include "lvgl.h"
#include <stdint.h>

#ifdef UI_PERF_TRACE
/* Self-check for LVGL's scroll blit (UI_PERF_TRACE builds only). While /usr/data/mnt/sd_0/
 * compas_scroll_blit.verify exists, every Nth frame produced by a pixel copy (N = the integer
 * in the file, default 1) is compared with a fresh stock render of the screen: the list's rows
 * on the panel's front page against lv_snapshot_take() of the same screen, skipping overlays
 * and the scrollbar column. The comparison runs right after the render of the pass that produced the frame. Areas invalidated since
 * the frame (marquee steps, landing labels) are left out of it; it is skipped when the
 * list itself scrolled, the screen changed or nothing of the rectangle was left to compare. Results are in the capture header. */
void ui_blit_verify_frame(void);   /* a frame was produced by a scroll blit */
void ui_blit_verify_plain_frame(void); /* a frame that was not a blit: self-test of the check */
void ui_blit_verify_after_render(void); /* main loop: right after the pass's render; runs a due check */
void ui_blit_verify_reset(void);   /* capture start */
typedef struct {
    uint32_t checked, skipped, bad_frames, selftest_checked, selftest_bad;
    uint64_t bad_pixels;
    uint64_t compared_px, excluded_px; /* pixels of the checked rectangles compared / left out (overlays, areas dirtied since) */
    uint32_t skip_reason[5]; /* gone, no front page, list scrolled, snapshot failed, fully excluded */
    int32_t bad_x1, bad_y1, bad_x2, bad_y2; /* bounding box of the first bad frame; x1 > x2: none */
} ui_blit_verify_stats_t;
void ui_blit_verify_stats(ui_blit_verify_stats_t * out);
#endif

#endif
