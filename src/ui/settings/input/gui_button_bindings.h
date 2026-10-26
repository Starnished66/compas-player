#ifndef GUI_BUTTON_BINDINGS_H
#define GUI_BUTTON_BINDINGS_H

#include <stdbool.h>

#include "../../../hardware/input/button_mapping.h"

/* Apply saved hardware bindings on the GUI thread and cancel pending gestures. */
void gui_apply_button_bindings(void);
/* A mapping must keep an available physical gesture that can wake the display. */
bool gui_button_bindings_allow_wake(const button_mapping_binding_t bindings[BUTTON_MAPPING_BUTTON_COUNT]);

#endif
