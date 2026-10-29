#ifndef GUI_ABOUT_INFO_H
#define GUI_ABOUT_INFO_H

#include "lvgl.h"

/* Puts the wordmark and the device info lines (version, build, device,
 * memory, internal storage, SD card) at the top of the About screen's
 * scrolling list. Values refresh about every 2 s while `scr` is the loaded
 * screen; the filesystem reads run on a worker thread. */
void gui_about_info_attach(lv_obj_t * scr, lv_obj_t * list);

#endif
