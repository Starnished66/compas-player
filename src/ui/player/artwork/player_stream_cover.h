#ifndef PLAYER_STREAM_COVER_H
#define PLAYER_STREAM_COVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "player_artwork_jobs.h"

/* Gets the encoded cover of a streamed track on the cover worker: from the
 * Subsonic cover cache (or the server) when the request names a cover id,
 * else from its URL, capped in size. *body is malloc'd: free() it. */
bool player_stream_cover_fetch(const cover_decode_request_t * req, bool (*stale)(void *), void * stale_arg,
                               uint8_t ** body, size_t * n);

/* Aborts the fetch in progress, if any. Safe from any thread. */
void player_stream_cover_cancel(void);

#endif
