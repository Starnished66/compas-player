#ifndef GUI_EXTERNAL_CONTROLS_H
#define GUI_EXTERNAL_CONTROLS_H

#include <stdbool.h>

/* UI thread only */
void gui_external_controls_poll(void);

/* UI thread only */
void gui_external_controls_publish_status(bool favorite);

#endif /* GUI_EXTERNAL_CONTROLS_H */
