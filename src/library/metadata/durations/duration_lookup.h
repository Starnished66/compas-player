#ifndef DURATION_LOOKUP_H
#define DURATION_LOOKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "duration_cache.h"
#include "../../../audio/engine/audio.h"

/* Background lookup of a list of tracks in the duration cache. One worker
 * resolves each path's identity (the library row's mtime/size, else stat),
 * then asks the cache. The UI thread only polls `done` and reads results. */
typedef struct {
    int32_t mtime, size;
    bool identified; /* false: the file could not be identified, never cached */
    bool hit;
    duration_info_t info;
} duration_lookup_result_t;

typedef struct duration_lookup duration_lookup_t;

/* Copies the paths. NULL on invalid input or when no thread can start. */
duration_lookup_t * duration_lookup_start(const char * const * paths, size_t count);
/* The cache's storage epoch when the lookup started: pass it to duration_cache_put()
 * for anything learned about these paths, so a card swap discards it. */
uint32_t duration_lookup_epoch(const duration_lookup_t * job);
bool duration_lookup_done(const duration_lookup_t * job);
/* Valid once done; index order matches the paths given to start. */
const duration_lookup_result_t * duration_lookup_result(const duration_lookup_t * job, size_t index);
/* The copy of the path given to start. */
const char * duration_lookup_path(const duration_lookup_t * job, size_t index);
void duration_lookup_cancel(duration_lookup_t * job);
/* Joins and frees only when done; otherwise a no-op the caller retries. */
void duration_lookup_destroy(duration_lookup_t * job);
/* Final shutdown only: cancel, join and free. */
void duration_lookup_cancel_and_destroy(duration_lookup_t * job);

/* Probe result <-> cache value. */
bool duration_info_from_probe(const audio_current_format_info_t * probe, duration_info_t * out);
/* DSD, above 48 kHz or above 16 bit: the Hi-Res chip's rule. */
bool duration_info_is_hires(const duration_info_t * info);

#endif
