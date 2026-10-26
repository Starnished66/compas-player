#define _POSIX_C_SOURCE 200809L
#include "artwork_failure_cache.h"

#include <assert.h>
#include <stdint.h>

static uint64_t fake_monotonic_ms = 1;

int __wrap_clock_gettime(clockid_t clock_id, struct timespec * ts) {
    assert(clock_id == CLOCK_MONOTONIC);
    ts->tv_sec = (time_t) (fake_monotonic_ms / 1000U);
    ts->tv_nsec = (long) (fake_monotonic_ms % 1000U) * 1000000L;
    return 0;
}

static artwork_fail_reason_t blocked_reason(int64_t song_id, time_t mtime) {
    artwork_fail_reason_t reason = ARTWORK_FAIL_NONE;
    assert(artwork_failure_cache_is_blocked(song_id, mtime, &reason));
    return reason;
}

int main(void) {
    artwork_failure_cache_clear();

    artwork_fail_reason_t untouched = ARTWORK_FAIL_PERMANENT;
    assert(artwork_failure_cache_is_blocked(0, 1, &untouched));
    assert(untouched == ARTWORK_FAIL_PERMANENT);
    assert(artwork_failure_cache_is_blocked(-1, 1, NULL));

    artwork_failure_cache_record(10, 100, ARTWORK_FAIL_TEMPORARY);
    assert(blocked_reason(10, 100) == ARTWORK_FAIL_TEMPORARY);
    artwork_failure_cache_record(10, 100, ARTWORK_FAIL_TEMPORARY);
    assert(blocked_reason(10, 100) == ARTWORK_FAIL_TEMPORARY);
    artwork_failure_cache_record(10, 100, ARTWORK_FAIL_TEMPORARY);
    assert(blocked_reason(10, 100) == ARTWORK_FAIL_PERMANENT);

    /* A newer source mtime invalidates a permanent result. */
    assert(!artwork_failure_cache_is_blocked(10, 101, NULL));
    artwork_failure_cache_record(10, 101, ARTWORK_FAIL_PERMANENT);
    assert(blocked_reason(10, 101) == ARTWORK_FAIL_PERMANENT);

    /* note_success clears the matching entry and its temporary promotion run. */
    artwork_failure_cache_clear();
    artwork_failure_cache_record(20, 200, ARTWORK_FAIL_TEMPORARY);
    artwork_failure_cache_record(20, 200, ARTWORK_FAIL_TEMPORARY);
    artwork_failure_cache_note_success(20);
    artwork_failure_cache_record(20, 200, ARTWORK_FAIL_TEMPORARY);
    assert(blocked_reason(20, 200) == ARTWORK_FAIL_TEMPORARY);

    /* Temporary entries block for less than 10 seconds, then expire. */
    artwork_failure_cache_clear();
    artwork_failure_cache_record(30, 300, ARTWORK_FAIL_TEMPORARY);
    fake_monotonic_ms = 10000;
    assert(blocked_reason(30, 300) == ARTWORK_FAIL_TEMPORARY);
    fake_monotonic_ms = 10001;
    assert(!artwork_failure_cache_is_blocked(30, 300, NULL));

    /* A colliding song replaces the direct-mapped slot; success is id-specific. */
    artwork_failure_cache_clear();
    artwork_failure_cache_record(5, 50, ARTWORK_FAIL_PERMANENT);
    artwork_failure_cache_record(261, 2610, ARTWORK_FAIL_PERMANENT);
    artwork_failure_cache_note_success(5);
    assert(blocked_reason(261, 2610) == ARTWORK_FAIL_PERMANENT);

    artwork_failure_cache_clear();
    assert(!artwork_failure_cache_is_blocked(261, 2610, NULL));
    return 0;
}
