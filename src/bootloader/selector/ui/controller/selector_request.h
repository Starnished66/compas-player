#ifndef COMPAS_SELECTOR_REQUEST_H
#define COMPAS_SELECTOR_REQUEST_H
#include <stdbool.h>
#include <stddef.h>
/* One fixed request on verified writable internal storage. Never stores
 * executable paths or commands. A consumed request is removed durably. */
bool selector_request_write(char *error, size_t error_size);
bool selector_request_consume(void);
/* Cancel only the exact inode accepted by this process's last write. */
bool selector_request_cancel(void);
#endif
