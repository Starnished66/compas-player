#ifndef GUI_SETUP_PLUGINS_H
#define GUI_SETUP_PLUGINS_H

#include <stdbool.h>
#include <stddef.h>

bool gui_setup_plugins_is_selected(const char * id);
size_t gui_setup_plugins_selected_count(void);
void gui_setup_plugins_restore_selection(void);
void gui_setup_plugins_clear_selection(void);
void gui_setup_plugins_toggle(const char * id, const char * name);
void gui_setup_plugins_set_layout(const char * id, const char * name);
bool gui_setup_plugins_layout_ready(void);
bool gui_setup_plugins_start(void (*on_complete)(void));
bool gui_setup_plugins_busy(void);
void gui_setup_plugins_teardown(void);
void gui_setup_plugins_resume(void);

#endif
