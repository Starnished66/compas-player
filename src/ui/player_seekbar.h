#ifndef PLAYER_SEEKBAR_H
#define PLAYER_SEEKBAR_H

#include "lvgl.h"
#include "waveform.h"
#include <stdbool.h>

/* Attach one of the custom waveform renderers to a progress slider. Returns
 * true when style names a supported renderer; unknown names leave the slider
 * unchanged so its native LVGL rendering remains available. */
bool player_seekbar_attach(lv_obj_t * slider, const char * style);

/* Replace the cached waveform and invalidate only when its contents change.
 * NULL, or data with ready == false, selects the native-looking rail/thumb
 * fallback until waveform data is ready. */
void player_seekbar_update(lv_obj_t * slider, const waveform_data_t * data);

/* Generic progress control helpers. Sliders and arcs use the same percentage
 * range and role; full-circle arcs receive direct pointer-to-angle mapping. */
bool player_seekbar_is_supported(lv_obj_t * obj);
int32_t player_seekbar_get_value(lv_obj_t * obj);
void player_seekbar_set_value(lv_obj_t * obj, int32_t value);
bool player_seekbar_configure(lv_obj_t * obj);

#endif
