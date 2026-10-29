#pragma once
/* Duration-cache front end of the group-songs page probe (every list that shows
 * "4:32 · FLAC": albums, genres, playlists, artist All Songs, favourites).
 *
 * The page's paths are looked up in the duration cache on a worker; the rows the
 * cache knows are filled at once and only the misses go to the track-probe
 * worker. Probe results are written back to the cache under the identity the
 * lookup found. UI thread only. */
#include <stdbool.h>
#include "../../../audio/engine/audio.h"
#include "../../../library/metadata/durations/duration_cache.h"

/* Forgets the current page. Never waits for the lookup worker. */
void gui_page_durations_reset(void);
/* Starts the cache lookup of `count` paths. False if it cannot start now (too many
 * earlier lookups are still winding down, or no thread): retry on a later poll. */
bool gui_page_durations_begin(const char * const * paths, int count);
/* True once the lookup has finished (also when it failed: then everything is a miss). */
bool gui_page_durations_ready(void);
/* The cache lookup worker is still running. */
bool gui_page_durations_busy(void);
/* After ready: calls hit() for every index the cache knows (not the failed ones) and
 * fills `miss` with the indexes still to probe, in order. Failed-and-remembered
 * indexes are neither. Returns the miss count. */
int gui_page_durations_plan(int * miss, int max, void (*hit)(int index, const duration_info_t * info, void * user),
                            void * user);
/* A probe result for page index `index`: stores it in the cache. */
void gui_page_durations_note_probe(int index, const audio_current_format_info_t * info);
void gui_page_durations_note_failed(int index);
void gui_page_durations_shutdown(void);
