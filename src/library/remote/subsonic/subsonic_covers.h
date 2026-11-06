#ifndef SUBSONIC_COVERS_H
#define SUBSONIC_COVERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "subsonic_cache.h"

/* Cover art for one Subsonic server, kept on the SD card in the server's
 * folder (covers/). No UI code: callers run these on a worker thread, never the
 * UI thread. Nothing is keyed on a URL, because the auth part of a URL changes
 * on every request; files are named by the cover id, and the URL is built
 * inside each network call from the server passed in.
 *
 * A thumbnail is a small header plus raw RGB565 pixels. The large cover is the
 * server's own bytes at SUBSONIC_COVER_LARGE_PX. With no card nothing is
 * written, so a fetch still returns its result but is repeated next time. */

#define SUBSONIC_COVER_LARGE_PX 480
#define SUBSONIC_COVER_LARGE_MAX (1024L * 1024) /* larger covers are neither downloaded nor read */

/* A stable key for a cover in RAM: the same server and cover id always give
 * the same value, and no URL is involved. srvkey comes from subsonic_cache_server_key(). */
uint64_t subsonic_cover_key(const char * srvkey, const char * cover_id);

/* Reads a stored px by px thumbnail. *px_out is a malloc'd buffer of px*px
 * pixels: free() it. False for a missing, short, long or damaged file. */
bool subsonic_cover_thumb_load(const subsonic_server_t * server, const char * cover_id, int px, uint16_t ** px_out);

/* Loads the thumbnail, or else downloads and decodes it and stores it. *no_art
 * is set when the server has no cover for this id (empty id, HTTP 404 or
 * Subsonic error 70), so the caller can stop asking. */
bool subsonic_cover_thumb_fetch(const subsonic_server_t * server, const char * cover_id, int px,
                                http_cancel_token_t * cancel, uint16_t ** px_out, bool * no_art);

/* The stored large cover, or with allow_net the downloaded one (stored for
 * next time). *bytes is malloc'd: free() it. */
bool subsonic_cover_large_bytes(const subsonic_server_t * server, const char * cover_id, bool allow_net,
                                http_cancel_token_t * cancel, uint8_t ** bytes, size_t * n);

/* Decodes large-cover bytes at thumbnail priority and takes ownership of the
 * malloc'd bytes (freed on every result). Returns malloc'd RGB565 pixels to
 * free(), or NULL on failure. Only permanent decode errors evict the stored
 * large cover. The cancellation callback and user are forwarded unchanged. */
uint16_t * subsonic_cover_decode_large(const subsonic_server_t * server, const char * cover_id,
                                       uint8_t * bytes, size_t n, int px, bool (*cancelled)(void *), void * user);

/* Deletes the stored large cover, for a caller that found its bytes undecodable.
 * The next fetch downloads it again. */
void subsonic_cover_large_evict(const subsonic_server_t * server, const char * cover_id);

/* Files kept per server by the background prune, and how many new cover files
 * make a prune worth running. */
#define SUBSONIC_COVER_THUMBS_MAX 3000
#define SUBSONIC_COVER_LARGE_FILES_MAX 300
#define SUBSONIC_COVER_SAVES_PER_PRUNE 100

/* True once per SUBSONIC_COVER_SAVES_PER_PRUNE cover files saved since the last
 * true. Any thread; the UI polls it and starts a SUBSONIC_REFRESH_PRUNE job. */
bool subsonic_covers_prune_due(void);

/* Keeps the newest files of each kind and deletes the rest. */
void subsonic_covers_prune(const subsonic_server_t * server, int keep_thumbs, int keep_large);

#endif /* SUBSONIC_COVERS_H */
