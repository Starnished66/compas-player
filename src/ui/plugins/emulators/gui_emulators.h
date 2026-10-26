#ifndef GUI_EMULATORS_H
#define GUI_EMULATORS_H

#include "lvgl/lvgl.h"

/* More > Emulators: the installed emulators (emulator_registry.c), then their
 * systems and games, and the launcher. An emulator runs as a separate
 * executable (compas-emulators contract) while the UI thread waits for it.
 * Installed emulators and games are rescanned every time a screen is shown. */
void gui_emulators_row_cb(lv_event_t * e);
/* Deletes the screens and list data so gui_reload.c can rebuild the UI. */
void gui_emulators_teardown(void);

#endif /* GUI_EMULATORS_H */
