#ifndef GUI_REMOTE_SETTINGS_H
#define GUI_REMOTE_SETTINGS_H

#include <stdbool.h>
#include "lvgl.h"

/* UI-thread side of the remote settings API (see remote_settings.h). */

/* Once at boot, after settings are loaded: installs the registry hooks and
 * seeds its snapshot. */
void gui_remote_settings_init(void);
/* From update_timer_cb through gui_external_controls_poll(): applies queued
 * changes, starts a requested rescan, and switches the permission back off
 * when it expires. */
void gui_remote_settings_poll(void);

/* "Allow remote settings changes" on the Remote Control screen. Session-only. */
bool gui_remote_settings_allowed(void);
void gui_remote_settings_toggle_cb(lv_event_t * e);
/* Lets the poll flip the switch back when the permission expires. */
void gui_remote_settings_bind_switch(lv_obj_t * sw);

#endif /* GUI_REMOTE_SETTINGS_H */
