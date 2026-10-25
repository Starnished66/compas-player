#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <stdbool.h>
#include <stddef.h>

/* <SD>/.compas/device.json tells tools running on a computer which player a
 * card belongs to, so they build a database and covers it will accept:
 *
 *   {"schema":1,"model":"r1","player_version":"...","plugin_api":17,
 *    "database_format":5,"art_cache":"v4"}
 *
 * Every value comes from the constant the player itself uses. See
 * docs/SDTOOL.md. */

/* Formats the file's content (one line, newline-terminated) into out. Returns
 * the length, or 0 if it does not fit. */
size_t device_info_format(char *out, size_t size);

/* Writes compas_dir/device.json atomically when its content differs from the
 * file already there. Nothing is written, and false returned, when the folder
 * is missing or cannot be written (no card, read-only card). True means the
 * file holds the current content. */
bool device_info_publish(const char *compas_dir);

#endif
