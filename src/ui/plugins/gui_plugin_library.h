#ifndef GUI_PLUGIN_LIBRARY_H
#define GUI_PLUGIN_LIBRARY_H

#include "../../library/metadata/db/metadata_db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Plugin library lookups are implemented in gui_plugin_library.c using
 * metadata_db queries. Returned arrays and strings belong to the caller;
 * release them with gui_plugin_free_string_array(). Missing matches return
 * NULL/0. get_album_tracks() also rejects ambiguous album names within an
 * artist's albums. Results follow database ordering, except
 * get_next_album_tracks(), which follows the artist page's album sort and
 * stops at the last album without wrapping. */
char ** gui_plugin_get_artist_albums(const char * artist, int * out_count);
char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count);
char ** gui_plugin_get_next_album_tracks(const char * artist, const char * current_album, int * out_count);
void gui_plugin_free_string_array(char ** array, int count);

/* Paged plugin queries read metadata_db directly without loading UI library
 * arrays. Limits outside 1..GUI_PLUGIN_LIBRARY_MAX_PAGE use the maximum.
 * Callers provide room for that clamped number of rows. Song rows carry IDs
 * for later lookup; group rows carry first_song_id for cover lookup. */
#define GUI_PLUGIN_LIBRARY_MAX_PAGE 200

int64_t gui_plugin_library_song_count(void);

/* query/artist/album_artist/album each optional (NULL or "" skips that
 * filter) -- query is a title/artist substring match, the other three are
 * exact (case-insensitive). out_total, if non-NULL, receives the total
 * match count (for a plugin's own "page N of M" UI) via one extra count
 * query -- pass NULL to skip it if the caller doesn't need it. Returns the
 * number of rows written into out_rows (a caller-owned buffer sized for
 * the clamped limit described above); fewer than requested
 * (including 0) means this was the last page. */
int gui_plugin_library_get_songs(const char * query, const char * artist, const char * album_artist,
                                  const char * album, int offset, int limit, song_row_t * out_rows,
                                  int64_t * out_total);

/* Title/artist substring search, capped at GUI_PLUGIN_LIBRARY_MAX_PAGE --
 * no offset (see metadata_db_search_songs()'s own comment: always replace-
 * not-append the caller's previous result set, same convention this app's
 * own on-device search already follows). */
int gui_plugin_library_search(const char * query, int limit, song_row_t * out_rows);

bool gui_plugin_library_get_song(int64_t id, song_row_t * out_row);

bool gui_plugin_library_album_cover(int64_t song_id, int size_px, char * out, size_t out_size);

/* Artist and album pages use nonnegative offsets and the limit clamp above.
 * artist_filter (albums only) optionally restricts the returned albums. */
int gui_plugin_library_get_artists(int offset, int limit, group_row_t * out_rows);
int gui_plugin_library_get_albums(int offset, int limit, const char * artist_filter, group_row_t * out_rows);

/* Bridges to the same start_library_rescan() Settings > Update Music
 * Database's own row calls -- background-thread rescan of MUSIC_ROOT_DIR,
 * showing the same native "Updating music database..." progress screen.
 * A no-op if a rescan is already in flight (start_library_rescan()'s own
 * library_rescan_active guard). Lets a plugin that just wrote new files
 * under plugin.sd_root() make them show up in library_* queries without
 * the user finding the native menu item themselves. */
bool gui_plugin_refresh_library(void);

#endif /* GUI_PLUGIN_LIBRARY_H */
