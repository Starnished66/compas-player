#ifndef GUI_FILE_MANAGER_H
#define GUI_FILE_MANAGER_H

#include "lvgl/lvgl.h"
#include <stdbool.h>

/* More > File Manager: browses the whole SD card (all files, optionally
 * hidden ones) with selection, copy, move, rename, delete and new folder.
 * The screen is built on first open. */
void gui_file_manager_tile_cb(lv_event_t * e);
/* Opens the screen, at folder (an existing folder on the card) when not
 * NULL. False when the screen or folder could not be shown, or the screen
 * is already open under another one. */
bool gui_file_manager_open(const char * folder);
/* Deletes the screen and popups so gui_reload.c can rebuild the UI. */
void gui_file_manager_teardown(void);

#endif /* GUI_FILE_MANAGER_H */
