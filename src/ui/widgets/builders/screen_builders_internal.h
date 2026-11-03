#ifndef SCREEN_BUILDERS_INTERNAL_H
#define SCREEN_BUILDERS_INTERNAL_H

#include "lvgl/lvgl.h"
#include "../../../core/system/runtime/board_config.h"
#include <stdint.h>

/* Private bridges shared by the screen-builder and compact-list modules. */
int32_t screen_builders_active_display_width(void);
void screen_builders_configure_content_scrollbar(lv_obj_t * list);
void compact_list_refresh_font_layout(lv_obj_t * list);

/* Private bridges between the screen_builders*.c files. */
/* The hairline frame of Breeze cards: tiles, rows and popup surfaces. */
#define ICON_GRID_TILE_BORDER BOARD_SCALE_PX(2)
/* An image scaled to a target size inside a sized wrapper (rows). */
lv_obj_t * scaled_icon_create(lv_obj_t * parent, const char * src, int32_t target_px,
                              int32_t fallback_native_px, int32_t * out_h);
/* Re-places an icon-grid tile's icon and caption after a font change (menus). */
void icon_grid_refresh_tile(lv_obj_t * tile);

#endif /* SCREEN_BUILDERS_INTERNAL_H */
