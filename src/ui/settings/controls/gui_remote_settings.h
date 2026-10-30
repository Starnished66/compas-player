#ifndef GUI_REMOTE_SETTINGS_H
#define GUI_REMOTE_SETTINGS_H

#include <stdbool.h>
#include "lvgl.h"

/* UI-thread side of the remote settings API (see remote_settings.h). */

/* Once at boot, after settings are loaded: installs the registry hooks and
 * seeds its snapshot. */
void gui_remote_settings_init(void);
/* From update_timer_cb through gui_external_controls_poll(): applies queued
 * changes and starts a requested rescan. */
void gui_remote_settings_poll(void);

#endif /* GUI_REMOTE_SETTINGS_H */
