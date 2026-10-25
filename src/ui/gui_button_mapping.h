#ifndef GUI_BUTTON_MAPPING_H
#define GUI_BUTTON_MAPPING_H

#include <lvgl.h>

/* Button Mapping screen flow and its borrowed category summary. */
void gui_button_mapping_row_cb(lv_event_t * e);
void gui_button_mapping_set_category_summary(lv_obj_t * label);
void gui_button_mapping_refresh_summaries(void);
void gui_button_mapping_build_reset_popup(void);
void gui_button_mapping_teardown_reset_popup(void);
void gui_button_mapping_teardown_screens(void);

#endif
