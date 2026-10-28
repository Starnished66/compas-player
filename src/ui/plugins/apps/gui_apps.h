#ifndef GUI_APPS_H
#define GUI_APPS_H

#include "lvgl/lvgl.h"
#include <stdbool.h>

/* More > External Apps: the installed apps (app_registry.c) and the launcher.
 * An app runs as a separate executable (app_session.c) while the UI thread
 * waits for it; the first run, and the first after its executable changed,
 * asks the user (app_approval.c). The list is rescanned every time it opens.
 * Works on every board. */
void gui_apps_row_cb(lv_event_t * e);
/* Deletes the screen, the approval dialog and the list data so gui_reload.c
 * can rebuild the UI. */
void gui_apps_teardown(void);

/* For plugin.launch_app(): checks that app_id is installed and valid on this
 * board and that file (NULL for none) is an existing regular file on the SD
 * card whose extension the app opens, then schedules the launch (and the
 * approval dialog when needed) with lv_async_call. False with a short English
 * reason (a pointer to a literal) when it cannot. */
bool gui_apps_request_launch(const char * app_id, const char * file, const char ** reason);

#endif /* GUI_APPS_H */
