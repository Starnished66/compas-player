#ifndef UI_FONTS_H
#define UI_FONTS_H

#include <stdbool.h>

/* Load the UI's external bitmap fonts after lv_init() and before creating
 * displays, themes, or UI objects. Defaults to assets/fonts on host and
 * /usr/resource/fonts on target. COMPAS_FONT_ROOT is an explicit test-only
 * override (for example, a mounted SD directory). Returns false with a
 * path-specific error on stderr if any required font cannot be loaded. */
bool ui_fonts_init(void);

#endif /* UI_FONTS_H */
