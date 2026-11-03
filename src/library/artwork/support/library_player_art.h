#ifndef LIBRARY_PLAYER_ART_H
#define LIBRARY_PLAYER_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../coordinator/artwork_coordinator.h"

/* The default (and smallest) player cover raster, matching the built-in
 * layout's 350px cover card. Layouts with a larger card decode at a larger
 * square edge up to ALBUMART_PLAYER_CACHE_SIZE (player_cover_raster_px() in
 * the player artwork module); every decoded buffer carries its own edge.
 * fit_cover_img_to_card() in gui_player.c reads the real decoded image
 * header, so any edge works on screen. */
#define COVER_ART_WIDTH 350
#define COVER_ART_HEIGHT 350

/* On-demand fallback for the Playing Now page when the background warmer
 * has not produced the player-sized (ALBUMART_PLAYER_CACHE_SIZE) cache for
 * this track yet: finds the original art (external sidecar file, then
 * embedded picture via the same bounded/isolated extraction the warmer
 * uses), decodes and persists the cache exactly as the warmer would, and
 * returns edge x edge pixels for immediate display.
 * One-time cost per track -- once the cache file exists, the caller's own
 * fast path never reaches this again for it.
 *
 * *out_no_art_confirmed (may be NULL) is set true only on a false return
 * that means "this track genuinely has no usable art anywhere", safe for
 * the caller to remember and stop retrying; left false on every transient
 * failure (coordinator busy, isolated-helper fork/pipe/timeout,
 * cancellation), which a caller must retry rather than treat as final. */
bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist, int edge,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed);

bool gui_library_album_cover_cached(int64_t song_id, int size_px, char * out, size_t out_size);

#endif
