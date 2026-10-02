#ifndef GUI_SETUP_PLUGINS_H
#define GUI_SETUP_PLUGINS_H

#include <stdbool.h>

bool gui_setup_plugins_is_selected(const char * id);
void gui_setup_plugins_toggle(const char * id, const char * name);
bool gui_setup_plugins_confirm(void (*on_complete)(void));
bool gui_setup_plugins_busy(void);
void gui_setup_plugins_teardown(void);
void gui_setup_plugins_resume(void);

#endif
