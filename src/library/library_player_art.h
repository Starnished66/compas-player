#ifndef LIBRARY_PLAYER_ART_H
#define LIBRARY_PLAYER_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "artwork_coordinator.h"

/* The real decoded-cover render target. No longer tied to board screen
 * dimensions -- the Player screen shows this at a fixed square card size
 * (fit_cover_img_to_card() in gui_player.c reads the real decoded image
 * header and scales it to fit, so any target size here works), and
 * compute_reflection_bytes() safely center-crops/edge-extends whichever of
 * COVER_ART_WIDTH/REFLECTION_WIDTH and COVER_ART_HEIGHT/REFLECTION_HEIGHT
 * is larger against the other. Matches the on-screen card's own decode-
 * target size (see cover_card's size in build_player_screen()) so the
 * crisp card needs no runtime upscale/downscale on the reference board. */
#define COVER_ART_WIDTH 350
#define COVER_ART_HEIGHT 350

/* On-demand fallback for the Playing Now page when the background warmer
 * has not produced the player-sized (ALBUMART_PLAYER_CACHE_SIZE) cache for
 * this track yet: finds the original art (external sidecar file, then
 * embedded picture via the same bounded/isolated extraction the warmer
 * uses), decodes and persists the cache exactly as the warmer would, and
 * returns COVER_ART_WIDTH x COVER_ART_HEIGHT pixels for immediate display.
 * One-time cost per track -- once the cache file exists, the caller's own
 * fast path never reaches this again for it.
 *
 * *out_no_art_confirmed (may be NULL) is set true only on a false return
 * that means "this track genuinely has no usable art anywhere", safe for
 * the caller to remember and stop retrying; left false on every transient
 * failure (coordinator busy, isolated-helper fork/pipe/timeout,
 * cancellation), which a caller must retry rather than treat as final. */
bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed);

bool gui_library_album_cover_cached(int64_t song_id, int size_px, char * out, size_t out_size);

#endif
