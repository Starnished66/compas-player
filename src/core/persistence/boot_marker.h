#ifndef BOOT_MARKER_H
#define BOOT_MARKER_H

#include <stdbool.h>
#include <stddef.h>

/* Crash-guard marker files: a small file whose presence at the next boot
 * means the work it guarded never finished. It may carry a short text naming
 * what was being done. Creation and removal are made durable (file and
 * directory fsync); errors are ignored, a missing marker only weakens the
 * guard. */
void boot_marker_set(const char * path);
/* Creates or replaces the marker with the given text. */
void boot_marker_set_text(const char * path, const char * text);
/* Returns whether the marker exists; copies its text (empty if none) into buf. */
bool boot_marker_read(const char * path, char * buf, size_t size);
bool boot_marker_exists(const char * path);
void boot_marker_clear(const char * path);

#endif /* BOOT_MARKER_H */
