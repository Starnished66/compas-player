#ifndef GUI_EQUALIZER_H
#define GUI_EQUALIZER_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/* All calls are made on the LVGL/UI thread. Both injected callbacks are
 * required and borrowed for the process lifetime. Screen and popup
 * construction/teardown remain separate phases to preserve the
 * parent settings module's lifecycle order. */
void gui_equalizer_build_screens(lv_event_cb_t profiles_download_cb,
                                 bool (*profiles_download_available)(void));
void gui_equalizer_build_popups(void);
void gui_equalizer_teardown_popups(void);
void gui_equalizer_teardown_screens(void);
void gui_equalizer_update_summary(lv_obj_t * label);
void gui_equalizer_accent_changed(void);
void gui_equalizer_eq_row_event_cb(lv_event_t * e);
void gui_equalizer_crossfeed_row_event_cb(lv_event_t * e);

#endif
