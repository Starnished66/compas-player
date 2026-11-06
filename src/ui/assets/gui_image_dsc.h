#ifndef GUI_IMAGE_DSC_H
#define GUI_IMAGE_DSC_H

#include "lvgl.h"

/* Borrow a tightly packed square RGB565 buffer; px must be positive. */
void gui_image_dsc_init_rgb565(lv_image_dsc_t * dsc, const void * pixels, int px);

#endif
