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

#endif
