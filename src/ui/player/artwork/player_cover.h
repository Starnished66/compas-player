#ifndef PLAYER_COVER_H
#define PLAYER_COVER_H

/* Decoded cover state and the decode/reload pipeline. Read-only views of the
 * shown cover for the backdrop. Public entry points stay in gui_player.h. */

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "player_artwork_cache.h"
#include "../lyrics/player_inline_lyrics.h"

const uint8_t * player_cover_bytes(void);
int player_cover_edge(void);
uint64_t player_cover_generation(void);
const cover_file_id_t * player_cover_file(void);

void fit_cover_img_to_card(void);
void player_cover_size_changed_cb(lv_event_t * e);
void player_cover_resync_raster(void);
bool player_inline_query_cover(player_inline_cover_info_t * info);
void player_cover_cache_timer_pause(void);
void player_cover_cache_timer_delete(void);
void player_cover_show_retained(void);

#endif /* PLAYER_COVER_H */
