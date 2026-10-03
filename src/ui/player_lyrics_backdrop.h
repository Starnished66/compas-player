#ifndef PLAYER_LYRICS_BACKDROP_H
#define PLAYER_LYRICS_BACKDROP_H

#include "lvgl.h"
#include "player_layouts.h"

/* Optional cache for the background objects of an XML player lyrics view.
 * The wrapper contains only the cover, its dim overlay, and lyrics-area
 * background. Metadata and controls stay outside and keep their XML motion. */
void player_lyrics_backdrop_bind(lv_obj_t * wrapper, lv_obj_t * dim, lv_obj_t * area,
                                lv_anim_timeline_t * open_timeline,
                                lv_anim_timeline_t * close_timeline);
void player_lyrics_backdrop_invalidate(void);
void player_lyrics_backdrop_set_busy(bool busy);
bool player_lyrics_backdrop_begin(bool opening);
void player_lyrics_backdrop_end(void);
void player_lyrics_backdrop_teardown(void);

#endif
