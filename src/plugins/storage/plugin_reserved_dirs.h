#ifndef PLUGIN_RESERVED_DIRS_H
#define PLUGIN_RESERVED_DIRS_H

#include <stdbool.h>

/* Folders on the SD card that hold programs the player runs as root: the
 * external apps (.apps, with their data in .apps-data) and the emulators
 * (.emulators). A plugin must never be able to put a file there, so every
 * plugin API that writes, creates, moves or downloads checks its target
 * with these. Reading stays allowed.
 *
 * The match is lexical (case-insensitive, "." and ".." collapsed, relative
 * paths resolved against the working directory) and also by identity: a
 * path whose existing part leads into one of the folders by any other name
 * (vfat short names such as APPS~1, a link, another mount point of the card)
 * is the same folder. */

/* True when path is one of the reserved folders or anything below them. */
bool plugin_path_in_program_dirs(const char * path);

/* The same with an explicit SD card root (tests). */
bool plugin_path_in_program_dirs_of(const char * sd_root, const char * path);

#endif
