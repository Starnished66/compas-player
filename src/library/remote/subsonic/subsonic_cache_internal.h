#ifndef SUBSONIC_CACHE_INTERNAL_H
#define SUBSONIC_CACHE_INTERNAL_H

#include <stdio.h>

#include "subsonic_cache.h"

/* File helpers shared by the cache and the cover store. Paths are relative to
 * the server's folder. */

bool subsonic_cache_path(const subsonic_server_t * server, const char * file, char * out, size_t size);

/* Writes through a temp file and renames it into place. Fails at once when the
 * SD card is not mounted. fill writes the whole content. */
bool subsonic_cache_write_atomic(const subsonic_server_t * server, const char * file,
                                 bool (*fill)(FILE * f, const void * ctx), const void * ctx);

/* Opens a file for reading unless it is larger than max_bytes; NULL otherwise. */
FILE * subsonic_cache_open(const subsonic_server_t * server, const char * file, long max_bytes);

/* Keeps the newest files ending in suffix across the given folders and deletes
 * the rest. Newest is by mtime, or by the fetched time in a songs file's first
 * line. Never call from the UI thread. */
void subsonic_cache_prune_files(const subsonic_server_t * server, const char * const * dirs, int dir_count,
                                const char * suffix, bool by_mtime, int keep);

#endif /* SUBSONIC_CACHE_INTERNAL_H */
