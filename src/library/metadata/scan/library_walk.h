#ifndef LIBRARY_WALK_H
#define LIBRARY_WALK_H

#include <stdatomic.h>
#include <stdbool.h>

/* Top-level folder holding audiobooks; the music scan skips it. */
#define AUDIOBOOKS_LIBRARY_DIR_NAME "Audiobooks"

/* Library discovery shared by the player's database scanner and compas-sdtool.
 * Kept free of UI dependencies so the same walk runs on a computer. */

/* Bounded-memory library walk. Each playable file is delivered immediately
 * to cb; the path is valid only for the duration of the callback. Returning
 * false from cb stops the walk. out_count receives the number of songs
 * delivered. Memory use is bounded by the recursion stack + one PATH_MAX
 * buffer per active directory, independent of library size. */
typedef bool (*file_browser_song_visit_cb_t)(const char * path, void * user);

/* Database-oriented variant which excludes one immediate child directory of
 * root, case-insensitively. Descendants with the same name elsewhere are
 * not skipped. A directory containing database.ignore excludes itself and its
 * subtree from the results, database.unignore re-includes one, and a directory
 * containing both or neither inherits from its parent.
 * False means the scan is void; a non-zero *out_skipped means it was
 * incomplete. */
bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress,
                                                     int * out_skipped);

/* True when name has an extension the decoders accept (the Files filter). */
bool file_browser_is_playable_name(const char * name);

#endif /* LIBRARY_WALK_H */
