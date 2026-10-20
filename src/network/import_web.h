#ifndef IMPORT_WEB_H
#define IMPORT_WEB_H

#include <stdbool.h>

/* Existing UI lifecycle hooks; these now enable/cancel the native
 * PIN-authenticated file API and do not launch any external service. */
bool import_web_start(void);
void import_web_stop(void);

#endif
