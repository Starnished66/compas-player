#ifndef PLAYER_BACKDROP_H
#define PLAYER_BACKDROP_H

/* Now Playing backdrop: the frosted reflection of the cover, the flat-color
 * alternative and the text/icon contrast styles that follow them. Public
 * entry points (gui_player_refresh_frosted_background() and friends) stay in
 * gui_player.h; this header is for the cover pipeline and the role binder.
 * The overlay and background widgets stay owned by gui_player.c and are read
 * live through gui_player_internal.h, so nothing here goes stale when the
 * widget tree is rebuilt. Reflection storage is owned by player_backdrop.c
 * alone: the cover poll hands a worker reflection over through
 * player_backdrop_adopt_reflection(). */

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "player_artwork_cache.h"

player_frost_params_t resolve_player_frost_params(void);
void drop_current_reflection(void);
void apply_player_flat_background(bool has_bg_color, uint32_t bg_color);
void player_panel_show_reflection(void);
void player_backdrop_adopt_reflection(uint8_t * reflection, const player_frost_params_t * params);
void backdrop_styles_init(void);
lv_style_t * player_backdrop_progress_style(bool arc);
lv_style_t * player_backdrop_surface_style(void);

#endif /* PLAYER_BACKDROP_H */
