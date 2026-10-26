#include "artwork_coordinator.h"
#include "artwork_clock.h"

#include <pthread.h>
#include <time.h>

#include "../../../audio/engine/audio.h"
#include "../../../core/system/logging/debug_log.h"

/* --- Process-Wide Artwork Decode Coordinator --- */

static pthread_mutex_t coord_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t coord_cond = PTHREAD_COND_INITIALIZER;

static bool is_decoding = false;
static artwork_priority_t active_prio = ARTWORK_PRIO_WARMER;
static int pending_player_count = 0;
static int pending_thumb_count = 0;

artwork_acquire_result_t artwork_coordinator_acquire(artwork_priority_t prio, size_t estimated_bytes,
                                                     uint32_t timeout_ms,
                                                     artwork_cancel_fn cancel_cb, void * user_data) {
    if (cancel_cb && cancel_cb(user_data)) return ARTWORK_ACQUIRE_CANCELLED;

    /* Check memory admission first before waiting */
    if (!artwork_check_memory_admission(prio, estimated_bytes)) {
        DBG_LOG("artwork_coord: memory admission rejected (prio=%d est=%zu avail=%zu)\n",
                prio, estimated_bytes, system_get_mem_available_bytes());
        return ARTWORK_ACQUIRE_LOW_MEM;
    }

    pthread_mutex_lock(&coord_lock);

    /* Background warmer is suspended if audio is playing or higher priority is pending/active */
    if (prio == ARTWORK_PRIO_WARMER) {
        if (audio_is_playing()) {
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_SUSPENDED;
        }
        if (pending_player_count > 0 || pending_thumb_count > 0 || is_decoding) {
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_BUSY;
        }
    }

    if (prio == ARTWORK_PRIO_PLAYER) pending_player_count++;
    else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count++;

    uint64_t start_ms = artwork_monotonic_ms();
    uint64_t deadline_ms = start_ms + (uint64_t) timeout_ms;

    /* Strict priority predicate: lower priorities must wait while higher priorities are pending */
    while (is_decoding ||
           (prio == ARTWORK_PRIO_THUMBNAIL && pending_player_count > 0) ||
           (prio == ARTWORK_PRIO_WARMER && (pending_player_count > 0 || pending_thumb_count > 0))) {

        if (cancel_cb && cancel_cb(user_data)) {
            if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
            else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_CANCELLED;
        }

        /* If higher priority arrives while a lower priority is running, wake waiters */
        if (prio > active_prio && is_decoding) {
            pthread_cond_broadcast(&coord_cond);
        }

        uint64_t now = artwork_monotonic_ms();
        if (now >= deadline_ms) {
            if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
            else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_BUSY;
        }

        uint64_t wait_ms = deadline_ms - now;
        if (wait_ms > 50) wait_ms = 50; /* Poll cancel_cb every 50ms */

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nsec = (uint64_t) ts.tv_nsec + wait_ms * 1000000ULL;
        ts.tv_sec += (time_t) (nsec / 1000000000ULL);
        ts.tv_nsec = (long) (nsec % 1000000000ULL);

        pthread_cond_timedwait(&coord_cond, &coord_lock, &ts);
    }

    if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
    else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;

    /* Re-verify memory admission right before taking the slot */
    if (!artwork_check_memory_admission(prio, estimated_bytes)) {
        pthread_mutex_unlock(&coord_lock);
        return ARTWORK_ACQUIRE_LOW_MEM;
    }

    is_decoding = true;
    active_prio = prio;
    pthread_mutex_unlock(&coord_lock);
    return ARTWORK_ACQUIRE_OK;
}

void artwork_coordinator_release(artwork_priority_t prio) {
    (void) prio;
    pthread_mutex_lock(&coord_lock);
    is_decoding = false;
    pthread_cond_broadcast(&coord_cond);
    pthread_mutex_unlock(&coord_lock);
}

bool artwork_coordinator_should_yield(artwork_priority_t my_prio,
                                     artwork_cancel_fn cancel_cb, void * user_data) {
    if (cancel_cb && cancel_cb(user_data)) return true;
    if (my_prio == ARTWORK_PRIO_PLAYER) return false; /* Player never yields to lower priority */

    pthread_mutex_lock(&coord_lock);
    bool yield = false;
    if (my_prio == ARTWORK_PRIO_WARMER) {
        yield = (pending_player_count > 0 || pending_thumb_count > 0 || audio_is_playing());
    } else if (my_prio == ARTWORK_PRIO_THUMBNAIL) {
        yield = (pending_player_count > 0);
    }
    pthread_mutex_unlock(&coord_lock);
    return yield;
}
