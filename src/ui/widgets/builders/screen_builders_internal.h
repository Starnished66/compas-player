#ifndef SCREEN_BUILDERS_INTERNAL_H
#define SCREEN_BUILDERS_INTERNAL_H

#include "lvgl/lvgl.h"
#include <stdint.h>

/* Private bridges shared by the screen-builder and compact-list modules. */
int32_t screen_builders_active_display_width(void);
void screen_builders_configure_content_scrollbar(lv_obj_t * list);
void compact_list_refresh_font_layout(lv_obj_t * list);

#endif /* SCREEN_BUILDERS_INTERNAL_H */
