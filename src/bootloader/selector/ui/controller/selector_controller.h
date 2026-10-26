#ifndef COMPAS_SELECTOR_CONTROLLER_H
#define COMPAS_SELECTOR_CONTROLLER_H
#include <stdbool.h>
/* Called only after trusted internal storage/SD preparation, before any
 * Rockbox display/input threads. Returns true only for explicit Rockbox. */
bool selector_controller_run(bool rockbox_available);
#endif
