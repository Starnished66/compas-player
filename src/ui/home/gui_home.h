#ifndef GUI_HOME_H
#define GUI_HOME_H

#include <lvgl/lvgl.h>

/* Build on the LVGL UI thread. The borrowed callbacks must remain valid for
 * the screen lifetime; the injected navigation callbacks resolve their
 * destination when clicked. */
lv_obj_t * gui_home_build_screen(lv_event_cb_t settings_cb,
                                 lv_event_cb_t stream_media_cb);

#endif
