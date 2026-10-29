#ifndef ARTWORK_FAILURE_CACHE_H
#define ARTWORK_FAILURE_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARTWORK_FAIL_NONE = 0,
    ARTWORK_FAIL_PERMANENT,      /* Corrupt, unsupported, oversized, or missing art */
    ARTWORK_FAIL_TEMPORARY,      /* Low memory, coordinator busy, or cancelled */
} artwork_fail_reason_t;

/* Checks if an album (by song_id and source_mtime) is recorded as failed.
 * If source_mtime > cached mtime, returns false (source was updated). */
bool artwork_failure_cache_is_blocked(int64_t song_id, time_t source_mtime,
                                      artwork_fail_reason_t * out_reason);
/* True when a failure (permanent, or temporary and not yet expired) is on record for this song:
 * a cheap look that needs no source mtime, so a caller with nothing on record can skip the
 * file system work that mtime costs. */
bool artwork_failure_cache_has_entry(int64_t song_id);
/* Records an artwork failure for an album with its current source mtime. */
void artwork_failure_cache_record(int64_t song_id, time_t source_mtime,
                                  artwork_fail_reason_t reason);
/* Clears any recorded failure (and its consecutive-TEMPORARY count) for one
 * song after a decode for it actually succeeds -- otherwise a stale count
 * left over from a past, now-resolved TEMPORARY streak could combine with a
 * later, unrelated TEMPORARY failure to wrongly promote it to PERMANENT
 * despite a fully successful decode in between. */
void artwork_failure_cache_note_success(int64_t song_id);
/* Clears/invalidates the failure cache. */
void artwork_failure_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* ARTWORK_FAILURE_CACHE_H */
