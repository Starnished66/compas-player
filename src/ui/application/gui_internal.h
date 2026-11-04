#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

/* Shared between gui.c, gui_screen_power.c and gui_control_poll.c only. */

#include "gui.h"
#include "../../audio/engine/audio.h"

typedef struct {
    bool on;
    bool just_woke;
} screen_poll_result_t;

/* The audio engine's volume as a capped percent. */
static inline int gui_capped_volume_percent(void) {
    return audio_clamp_volume_percent((int)(audio_get_volume() * 100.0f + 0.5f));
}

/* gui_screen_power.c: screen on/off, wake reveal, idle actions, physical buttons. */
void gui_screen_power_init(void);
void gui_screen_power_deinit(void);
/* Creates the power and media input timers. */
void gui_screen_power_start_timers(void);
/* Restarts the dimming and idle-shutdown state after an external session. */
void gui_screen_power_reset_idle_state(void);
void gui_buttons_init(void);
void media_button_timer_cb(lv_timer_t * timer);
bool shutdown_background_work_active(void);
screen_poll_result_t poll_screen_power(void);

/* gui.c: whether the current track is a favorite, published to external controls. */
extern bool favorite_is_set;

/* gui_control_poll.c: the update and player timers and the per-tick polls. */
void gui_control_poll_start_timers(void);
void gui_control_poll_deinit(void);
/* Makes the player timer due now, if it exists. */
void gui_control_poll_kick_player_ui(void);

#endif /* GUI_INTERNAL_H */
