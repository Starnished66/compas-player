#ifndef PLAYER_TRANSPORT_IMAGES_H
#define PLAYER_TRANSPORT_IMAGES_H

#include "lvgl/lvgl.h"
#include <stdbool.h>

void gui_player_prepare_play_btn_images(void);
const void * gui_player_play_btn_image_src(bool is_playing);
/* UI-thread cache shared with the drawer. Widget-only teardown keeps it alive;
 * reset only when refreshing assets, before rebinding the borrowed sources. */
bool player_transport_images_set_glyph_on_light(bool value);
void player_transport_images_reset(void);

#endif /* PLAYER_TRANSPORT_IMAGES_H */
