#ifndef GUI_EMULATOR_STORE_H
#define GUI_EMULATOR_STORE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/* More > Emulators > Download emulators: the emulator catalog screen. The
 * list, detail popup and busy-screen flow follow the Plugin Store
 * (gui_plugin_store.c) and drive emulator_store.c the same way. */
void gui_emulator_store_init(void);
void gui_emulator_store_teardown(void);
/* Checks Wi-Fi, loads the catalog behind the busy screen and opens the list. */
void gui_emulator_store_open(void);
bool gui_emulator_store_operation_active(void);
void poll_emulator_store(void);

#endif /* GUI_EMULATOR_STORE_H */
