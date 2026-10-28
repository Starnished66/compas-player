#ifndef SCROLL_BLIT_H
#define SCROLL_BLIT_H

#include "lvgl.h"

/* Opts a vertically scrolling list into LVGL's scroll blit (lvgl_scroll_blit.patch): while it
 * scrolls, the pixels already on screen are shifted instead of rendering the whole list again.
 * The patch checks the rendering mode, backgrounds and overlays itself; this module only vetoes
 * frames in which something outside LVGL owns the display (transition compositor, external page
 * pans). Trace builds can switch it off at run time with COMPAS_SCROLL_BLIT=0 or by creating
 * /usr/data/mnt/sd_0/compas_scroll_blit.off (re-read twice a second), for A/B comparisons. */
void ui_scroll_blit_enable(lv_obj_t * list);

#endif
