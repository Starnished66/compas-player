#include "artwork_failure_cache.h"
#include "artwork_clock.h"

#include <pthread.h>
#include <string.h>

/* --- Negative / Backoff Failure Cache with mtime Invalidation --- */

#define FAILURE_CACHE_SIZE 256
#define TEMPORARY_BACKOFF_MS 10000ULL /* 10 seconds backoff for temporary failures */
/* Some TEMPORARY causes (a decode that deterministically exceeds a fixed
 * memory-admission budget every single attempt, not just under momentary
 * pressure) never actually resolve on retry. Without a cap, a warmer pass
 * that keeps re-hitting one of these treats it as retryable forever,
 * restarting its whole pass on the same never-going-to-succeed song every
 * backoff window. After this many consecutive TEMPORARY results for the
 * same song+source_mtime, treat it as PERMANENT instead. */
#define ARTWORK_TEMPORARY_PROMOTE_THRESHOLD 3

typedef struct {
    int64_t song_id;
    time_t source_mtime;
    artwork_fail_reason_t reason;
    uint64_t timestamp_ms;
    int consecutive_temporary_count;
} artwork_failure_entry_t;

static artwork_failure_entry_t failure_cache[FAILURE_CACHE_SIZE];
static pthread_mutex_t failure_cache_lock = PTHREAD_MUTEX_INITIALIZER;

bool artwork_failure_cache_is_blocked(int64_t song_id, time_t source_mtime, artwork_fail_reason_t * out_reason) {
    if (song_id <= 0) return true;

    uint64_t now = artwork_monotonic_ms();
    pthread_mutex_lock(&failure_cache_lock);
    size_t idx = (size_t) song_id % FAILURE_CACHE_SIZE;
    if (failure_cache[idx].song_id == song_id && failure_cache[idx].reason != ARTWORK_FAIL_NONE) {
        /* If source file was modified after the failure was recorded, invalidate entry */
        if (source_mtime > 0 && source_mtime > failure_cache[idx].source_mtime) {
            failure_cache[idx].reason = ARTWORK_FAIL_NONE;
            pthread_mutex_unlock(&failure_cache_lock);
            return false;
        }

        if (failure_cache[idx].reason == ARTWORK_FAIL_PERMANENT) {
            if (out_reason) *out_reason = ARTWORK_FAIL_PERMANENT;
            pthread_mutex_unlock(&failure_cache_lock);
            return true;
        } else if (failure_cache[idx].reason == ARTWORK_FAIL_TEMPORARY) {
            if (now - failure_cache[idx].timestamp_ms < TEMPORARY_BACKOFF_MS) {
                if (out_reason) *out_reason = ARTWORK_FAIL_TEMPORARY;
                pthread_mutex_unlock(&failure_cache_lock);
                return true;
            }
            /* Temporary backoff expired -- allow retry */
            failure_cache[idx].reason = ARTWORK_FAIL_NONE;
        }
    }
    pthread_mutex_unlock(&failure_cache_lock);
    return false;
}

void artwork_failure_cache_record(int64_t song_id, time_t source_mtime, artwork_fail_reason_t reason) {
    if (song_id <= 0 || reason == ARTWORK_FAIL_NONE) return;

    uint64_t now = artwork_monotonic_ms();
    pthread_mutex_lock(&failure_cache_lock);
    size_t idx = (size_t) song_id % FAILURE_CACHE_SIZE;
    artwork_failure_entry_t * e = &failure_cache[idx];
    bool same_song = (e->song_id == song_id && e->source_mtime == source_mtime);
    if (reason == ARTWORK_FAIL_TEMPORARY) {
        int consecutive = (same_song ? e->consecutive_temporary_count : 0) + 1;
        if (consecutive >= ARTWORK_TEMPORARY_PROMOTE_THRESHOLD) {
            reason = ARTWORK_FAIL_PERMANENT;
            consecutive = 0;
        }
        e->consecutive_temporary_count = consecutive;
    } else {
        e->consecutive_temporary_count = 0;
    }
    e->song_id = song_id;
    e->source_mtime = source_mtime;
    e->reason = reason;
    e->timestamp_ms = now;
    pthread_mutex_unlock(&failure_cache_lock);
}

void artwork_failure_cache_note_success(int64_t song_id) {
    if (song_id <= 0) return;
    pthread_mutex_lock(&failure_cache_lock);
    size_t idx = (size_t) song_id % FAILURE_CACHE_SIZE;
    if (failure_cache[idx].song_id == song_id) {
        failure_cache[idx].reason = ARTWORK_FAIL_NONE;
        failure_cache[idx].consecutive_temporary_count = 0;
    }
    pthread_mutex_unlock(&failure_cache_lock);
}

void artwork_failure_cache_clear(void) {
    pthread_mutex_lock(&failure_cache_lock);
    memset(failure_cache, 0, sizeof(failure_cache));
    pthread_mutex_unlock(&failure_cache_lock);
}
