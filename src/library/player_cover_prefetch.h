#ifndef PLAYER_COVER_PREFETCH_H
#define PLAYER_COVER_PREFETCH_H

#include <stdbool.h>

/* Queue a bounded decode of an already-generated local album-art cache for
 * reuse if this track becomes the current player track. Strings are copied;
 * overlong fields are rejected rather than truncated. */
bool player_cover_prefetch_submit(const char *path, const char *artist,
                                  const char *album, const char *album_artist,
                                  int width, int height);

/* Invalidate both queued and active work. This is nonblocking with respect
 * to decode and must be called before clearing the raster cache. */
void player_cover_prefetch_cancel(void);

#endif
