#ifndef GUI_REMOTE_PLUGIN_IMAGES_H
#define GUI_REMOTE_PLUGIN_IMAGES_H

#include <stdbool.h>

/* Returns malloc-owned bounded JSON {mime,data}; never includes the path. */
char * gui_remote_plugin_preview_json(const char * path, bool png_only);

#endif
