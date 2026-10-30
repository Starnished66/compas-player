#ifndef GUI_SCREEN_OFF_MEMORY_H
#define GUI_SCREEN_OFF_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

/* Screen-off memory release. Once the screen has stayed off for a while with
 * the player running (playing or paused), the rebuildable decoded-image caches
 * are freed once per screen-off period. Static transition snapshots and the
 * quick-drawer buffers stay for instant response. Everything freed is
 * reloaded on demand, as after an emulator session. */

/* Called every poll tick while the screen is off. `screen_just_went_off`
 * starts a new period (re-arms the one-shot). UI thread only. */
void gui_screen_off_memory_tick(bool screen_just_went_off, bool player_running, uint32_t now_ms);
/* Called while the screen is off but a wake is pending: the 60 s wait starts over. */
void gui_screen_off_memory_restart_wait(uint32_t now_ms);
/* Called on the tick the screen turns on again. Logs the memory after wake. */
void gui_screen_off_memory_wake(void);
#define GUI_SCREEN_OFF_MEMORY_DELAY_MS 60000u

/* Pure policy (host-tested): has the release point been reached? */
static inline bool gui_screen_off_memory_due(bool released, bool player_running, uint32_t off_elapsed_ms) {
    return !released && player_running && off_elapsed_ms >= GUI_SCREEN_OFF_MEMORY_DELAY_MS;
}

#endif
