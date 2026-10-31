#ifndef GUI_REMOTE_PLUGINS_H
#define GUI_REMOTE_PLUGINS_H

/* Consume and execute at most one remote plugin/layout request on the UI thread. */
void gui_remote_plugins_poll(void);

#endif
