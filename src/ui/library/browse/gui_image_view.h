#ifndef GUI_IMAGE_VIEW_H
#define GUI_IMAGE_VIEW_H

#include <stdbool.h>

/* True for a JPEG, PNG or BMP file name (by extension). */
bool gui_image_view_is_image_name(const char * name);
/* Full-screen viewer for an image file. Swiping left or right steps through
 * the images of its folder in File Manager order. The screen is built on
 * first use. False when it could not be opened. */
bool gui_image_view_open(const char * path);
/* Stops the decode worker and deletes the screen so gui_reload.c can rebuild the UI. */
void gui_image_view_teardown(void);

#endif
