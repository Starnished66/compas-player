#ifndef PLAYER_LYRICS_BACKDROP_H
#define PLAYER_LYRICS_BACKDROP_H

#include "lvgl.h"
#include "player_layouts.h"

/* Optional cache for the background objects of a player lyrics view.
 * The wrapper contains only background objects; metadata and controls stay
 * outside and keep their own motion. With dim and area both NULL, cache a
 * static wrapper, preserving transparency. Otherwise cache the matched dim
 * and lyrics-area opacity transitions. Unsupported geometry falls back to
 * the live widgets. For a static wrapper, rebinding the same wrapper with
 * both timelines NULL detaches the old timeline pair while retaining a
 * still-valid snapshot; a later valid pair can reuse it. */
void player_lyrics_backdrop_bind(lv_obj_t * wrapper, lv_obj_t * dim, lv_obj_t * area,
                                lv_anim_timeline_t * open_timeline,
                                lv_anim_timeline_t * close_timeline);
/* Call before changing/freeing cached source pixels, or changing source
 * visibility/styles. In-place pixel writes cannot be detected from a source
 * pointer or descriptor key, so callers own this invalidation contract. */
void player_lyrics_backdrop_invalidate(void);
void player_lyrics_backdrop_set_busy(bool busy);
bool player_lyrics_backdrop_begin(bool opening);
/* Finish the current transition. Pass true only for a completed settled
 * transition; it retains a validated narrow static image binding for the next
 * one. Pass false for interruption, teardown, or before source/style changes. */
void player_lyrics_backdrop_end(bool keep_static);
void player_lyrics_backdrop_teardown(void);

#endif
