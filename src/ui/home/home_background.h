#ifndef HOME_BACKGROUND_H
#define HOME_BACKGROUND_H

#include <lvgl/lvgl.h>

/* Apply once during screen construction on the LVGL UI thread. The screen and
 * path must be non-NULL. Any decoded descriptor and pixels are released when
 * the screen is deleted. */
void home_background_apply(lv_obj_t * screen, const char * relative_path);

#endif
