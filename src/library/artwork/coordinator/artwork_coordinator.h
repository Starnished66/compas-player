#ifndef ARTWORK_COORDINATOR_H
#define ARTWORK_COORDINATOR_H

#include "artwork_memory.h"
#include "artwork_failure_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARTWORK_ACQUIRE_OK = 0,      /* Acquired coordinator lock; caller MUST release */
    ARTWORK_ACQUIRE_BUSY,        /* Higher/equal priority active, or lock timeout */
    ARTWORK_ACQUIRE_LOW_MEM,     /* MemAvailable below safety reserve */
    ARTWORK_ACQUIRE_SUSPENDED,   /* Warmer suspended due to active playback or UI */
    ARTWORK_ACQUIRE_CANCELLED,   /* Cancelled before or during wait */
} artwork_acquire_result_t;


/* Dynamic cancellation callback: evaluated live during multi-stage decode */
typedef bool (*artwork_cancel_fn)(void * user_data);

/* Attempts to acquire the exclusive decode slot for the given priority and estimated memory.
 * Strictly guarantees priority ordering: Player beats Thumbnail, Thumbnail beats Warmer.
 * On ARTWORK_ACQUIRE_OK, caller MUST call artwork_coordinator_release(). */
artwork_acquire_result_t artwork_coordinator_acquire(artwork_priority_t prio, size_t estimated_bytes,
                                                     uint32_t timeout_ms,
                                                     artwork_cancel_fn cancel_cb, void * user_data);

/* Releases the exclusive decode slot. */
void artwork_coordinator_release(artwork_priority_t prio);

/* Checks if an active lower-priority decode should yield/abandon because a higher-priority
 * request is pending or cancellation was requested. */
bool artwork_coordinator_should_yield(artwork_priority_t my_prio,
                                     artwork_cancel_fn cancel_cb, void * user_data);


#ifdef __cplusplus
}
#endif

#endif /* ARTWORK_COORDINATOR_H */
