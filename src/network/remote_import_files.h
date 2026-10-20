#ifndef REMOTE_IMPORT_FILES_H
#define REMOTE_IMPORT_FILES_H

#include <stdbool.h>
#include <stdint.h>

/* The native Remote Control file API is enabled only for an Import session.
 * Stop cancels active request bodies and waits for their file cleanup. */
bool remote_import_files_start(void);
void remote_import_files_stop(void);
bool remote_import_files_is_ready(void);

/* Called only after Remote Control has authenticated the request. Returns
 * true if the request path belongs to this API and has been handled. */
bool remote_import_files_handle(int fd, const char *method, const char *path,
                                long long content_length, bool wifi_transport);

#endif
