/* Deferred rating and play-count writes: queue, background flush and durable waits. */
#include "tagcache_internal.h"

atomic_uint numeric_epoch;

#define NUMERIC_QUEUE_CAP 64
#define NUMERIC_COMMIT_DELAY_SEC 2

typedef struct {
    int32_t gen;
    int32_t slot;
    int32_t playcount;
    int32_t last_played;
    int32_t rating;
    uint64_t ticket; /* newest enqueue this entry carries; see numeric_waiter_t */
} numeric_update_t;

static numeric_update_t numeric_queue[NUMERIC_QUEUE_CAP];
static int numeric_queue_count = 0;
static numeric_update_t numeric_inflight[NUMERIC_QUEUE_CAP];
static int numeric_inflight_count = 0;
static bool numeric_inflight_active;
static bool numeric_write_failed;
/* A rejected enqueue stays visible after later accepted writes succeed.
 * Cleared only when a new database session starts. */
static bool numeric_overflow_failed;
static pthread_mutex_t numeric_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Initialised exactly once by numeric_conds_init; every entry point runs it first. */
static pthread_cond_t numeric_cond;
static pthread_cond_t numeric_drain_cond;
/* Timed waits use the monotonic clock so a wall-clock step cannot stretch or
 * cut them; numeric_conds_init falls back to the realtime default if the
 * condition variables cannot be configured. */
static clockid_t numeric_clock = CLOCK_REALTIME;
static pthread_once_t numeric_conds_once = PTHREAD_ONCE_INIT;
static pthread_t numeric_thread;
static bool numeric_thread_running = false;
static bool numeric_shutdown = false;
/* Durable waiters. Every enqueue takes the next ticket, and a queued entry
 * carries the ticket of the newest value coalesced into it. A waiter records
 * (gen, slot, ticket) when its update is queued; the first batch holding
 * that slot with an entry ticket >= the waiter's settles it: written, or
 * failed/discarded. An in-flight entry for the slot from before the enqueue
 * has a lower ticket and cannot settle it. Dropping queued entries (session
 * stop) fails the waiters they carry. numeric_flush_now asks the worker to
 * skip the commit delay. All guarded by numeric_mutex. */
#define NUMERIC_WAITERS_MAX 8
typedef enum { NUMERIC_WAIT_FREE, NUMERIC_WAIT_PENDING, NUMERIC_WAIT_WRITTEN, NUMERIC_WAIT_FAILED } numeric_wait_state_t;
typedef struct {
    numeric_wait_state_t state;
    int32_t gen;
    int32_t slot;
    uint64_t ticket;
} numeric_waiter_t;
static numeric_waiter_t numeric_waiters[NUMERIC_WAITERS_MAX];
static uint64_t numeric_next_ticket = 1;
static bool numeric_flush_now = false;

/* Caller holds numeric_mutex. Settles the pending waiters that one of the
 * given entries carries. */
static void numeric_settle_waiters_locked(const numeric_update_t * entries, int count, bool written) {
    for (int w = 0; w < NUMERIC_WAITERS_MAX; w++) {
        numeric_waiter_t * waiter = &numeric_waiters[w];
        if (waiter->state != NUMERIC_WAIT_PENDING) continue;
        for (int i = 0; i < count; i++) {
            if (entries[i].gen == waiter->gen && entries[i].slot == waiter->slot &&
                entries[i].ticket >= waiter->ticket) {
                waiter->state = written ? NUMERIC_WAIT_WRITTEN : NUMERIC_WAIT_FAILED;
                break;
            }
        }
    }
}

/* Newest entry for (gen, slot), NULL when absent. Caller holds numeric_mutex. */
static numeric_update_t * numeric_find_locked(numeric_update_t * list, int count, int32_t gen, int32_t slot) {
    for (int i = count - 1; i >= 0; i--)
        if (list[i].gen == gen && list[i].slot == slot) return &list[i];
    return NULL;
}

static void numeric_take(numeric_update_t * u, const struct index_entry * idx) {
    u->playcount = idx->tag_seek[tag_playcount];
    u->last_played = idx->tag_seek[tag_lastplayed];
    u->rating = idx->tag_seek[tag_rating];
}

static void numeric_apply(struct index_entry * idx, const numeric_update_t * u) {
    idx->tag_seek[tag_playcount] = u->playcount;
    idx->tag_seek[tag_lastplayed] = u->last_played;
    idx->tag_seek[tag_rating] = u->rating;
}

/* Performs disk pwrite + fdatasync for a snapshot of numeric updates.
 * *discarded is set when the batch was dropped rather than written. */
static bool flush_numeric_batch(const numeric_update_t * batch, int n, bool * discarded) {
    *discarded = false;
    if (n <= 0) return true;
    if (db_dir[0] == '\0') { *discarded = true; return true; }
    /* A queued update belongs to the directory pinned when it was queued.
     * A different directory discards the batch. A failed stat keeps it:
     * the pinned fd may still be the right card, and success would hide
     * the loss. */
    int identity = db_logical_directory_status();
    if (identity < 0) return false;
    if (identity == 0) { *discarded = true; return true; }
    bool ok = true;
    for (int i = 0; i < n; i++) {
        int32_t gen = batch[i].gen;
        if (gen < 0) continue;
        /* Avoid repeating work for the same generation in this batch */
        bool already_done = false;
        for (int k = 0; k < i; k++) {
            if (batch[k].gen == gen) {
                already_done = true;
                break;
            }
        }
        if (already_done) continue;

        char master_path[640];
        master_file_path(master_path, sizeof(master_path), gen);
        int fd = open(master_path, O_RDWR);
        if (fd < 0) { ok = false; continue; }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            ok = false; continue;
        }
        struct master_header mh;
        if (pread(fd, &mh, sizeof(mh), 0) != (ssize_t) sizeof(mh) || !validate_master_header(&mh, (size_t) st.st_size)) {
            close(fd);
            ok = false; continue;
        }
        bool any_written = false;
        for (int j = i; j < n; j++) {
            if (batch[j].gen != gen) continue;
            int32_t slot = batch[j].slot;
            if (slot < 0 || slot >= mh.tch.entry_count) continue;
            struct index_entry ie;
            off_t off = (off_t) sizeof(struct master_header) + (off_t) slot * (off_t) sizeof(ie);
            if (pread(fd, &ie, sizeof(ie), off) != (ssize_t) sizeof(ie)) { ok = false; continue; }
            numeric_apply(&ie, &batch[j]);
            if (pwrite(fd, &ie, sizeof(ie), off) == (ssize_t) sizeof(ie)) {
                any_written = true;
            } else ok = false;
        }
        if (any_written) {
            if (fdatasync(fd) != 0) ok = false;
        }
        close(fd);
    }
    return ok;
}

static bool numeric_take_batch_locked(void) {
    if (numeric_queue_count == 0 || numeric_inflight_active) return false;
    numeric_inflight_count = numeric_queue_count;
    memcpy(numeric_inflight, numeric_queue, sizeof(numeric_update_t) * (size_t) numeric_queue_count);
    numeric_queue_count = 0;
    numeric_inflight_active = true;
    pthread_cond_broadcast(&numeric_drain_cond);
    return true;
}

static void numeric_retain_failed_locked(void) {
    for (int i = 0; i < numeric_inflight_count; i++) {
        numeric_update_t *u = &numeric_inflight[i];
        /* A newer queued value supersedes the failed batch entry. */
        bool replaced = numeric_find_locked(numeric_queue, numeric_queue_count, u->gen, u->slot) != NULL;
        if (!replaced && numeric_queue_count < NUMERIC_QUEUE_CAP)
            numeric_queue[numeric_queue_count++] = *u;
        else if (!replaced) {
            /* The combined cap below makes this unreachable. If it ever
             * happens, keep the failure latched instead of forgetting it
             * on the next successful drain. */
            numeric_write_failed = true;
            numeric_overflow_failed = true;
        }
    }
}

/* Caller holds numeric_mutex after flushing the in-flight batch. Shared
 * bookkeeping for every flush site. */
static void numeric_finish_batch_locked(bool ok, bool discarded) {
    /* A failed batch is retried later, but its waiters report the failure. */
    numeric_settle_waiters_locked(numeric_inflight, numeric_inflight_count, ok && !discarded);
    numeric_write_failed |= !ok;
    if (!ok) numeric_retain_failed_locked();
    else if (numeric_queue_count == 0) numeric_write_failed = false;
    numeric_inflight_active = false;
    numeric_inflight_count = 0;
    atomic_fetch_add_explicit(&numeric_epoch, 1, memory_order_release);
    pthread_cond_broadcast(&numeric_drain_cond);
}

/* Caller holds numeric_mutex and has just taken a batch. Flushes it with the
 * mutex released and returns with it held again. numeric_inflight cannot
 * change meanwhile: numeric_take_batch_locked refuses while it is active. */
static bool numeric_flush_inflight_locked(void) {
    int n = numeric_inflight_count;
    pthread_mutex_unlock(&numeric_mutex);
    bool discarded;
    bool ok = flush_numeric_batch(numeric_inflight, n, &discarded);
    pthread_mutex_lock(&numeric_mutex);
    numeric_finish_batch_locked(ok, discarded);
    return ok;
}

static bool numeric_conds_init_with(const pthread_condattr_t * attr) {
    if (pthread_cond_init(&numeric_cond, attr) != 0) return false;
    if (pthread_cond_init(&numeric_drain_cond, attr) == 0) return true;
    pthread_cond_destroy(&numeric_cond);
    return false;
}

static void numeric_conds_init(void) {
    /* macOS, where Compas Tools builds this file, has no pthread_condattr_setclock;
     * it takes the realtime fallback below. */
#ifndef __APPLE__
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) == 0) {
        bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && numeric_conds_init_with(&attr);
        pthread_condattr_destroy(&attr);
        if (ok) { numeric_clock = CLOCK_MONOTONIC; return; }
    }
#endif
    if (!numeric_conds_init_with(NULL)) fprintf(stderr, "tagcache: numeric condition variables unavailable\n");
}

static void numeric_conds_ready(void) { pthread_once(&numeric_conds_once, numeric_conds_init); }

bool tagcache_flush_numeric(void) {
    numeric_conds_ready();
    for (;;) {
        pthread_mutex_lock(&numeric_mutex);
        while (numeric_inflight_active) pthread_cond_wait(&numeric_drain_cond, &numeric_mutex);
        if (!numeric_take_batch_locked()) { bool ok = !numeric_write_failed; pthread_mutex_unlock(&numeric_mutex); return ok; }
        bool ok = numeric_flush_inflight_locked();
        pthread_mutex_unlock(&numeric_mutex);
        if (!ok) return false;
    }
}

static void * numeric_worker_func(void * arg) {
    (void) arg;
    pthread_mutex_lock(&numeric_mutex);
    while (!numeric_shutdown) {
        if (numeric_queue_count == 0) {
            pthread_cond_wait(&numeric_cond, &numeric_mutex);
            continue;
        }
        int rc = 0;
        if (!numeric_flush_now) {
            struct timespec ts;
            clock_gettime(numeric_clock, &ts);
            ts.tv_sec += NUMERIC_COMMIT_DELAY_SEC;
            rc = pthread_cond_timedwait(&numeric_cond, &numeric_mutex, &ts);
        }
        if (rc == ETIMEDOUT || numeric_shutdown || numeric_flush_now || numeric_queue_count >= NUMERIC_QUEUE_CAP) {
            numeric_flush_now = false;
            if (!numeric_take_batch_locked()) continue;
            numeric_flush_inflight_locked();
        }
    }
    if (numeric_take_batch_locked()) numeric_flush_inflight_locked();
    pthread_mutex_unlock(&numeric_mutex);
    return NULL;
}

void numeric_worker_start(void) {
    numeric_conds_ready();
    pthread_mutex_lock(&numeric_mutex);
    if (!numeric_thread_running) {
        numeric_shutdown = false;
        numeric_queue_count = 0;
        numeric_inflight_count = 0;
        numeric_inflight_active = false;
        numeric_write_failed = false;
        numeric_overflow_failed = false;
        const char * disable_env = getenv("TAGCACHE_TEST_DISABLE_NUMERIC_WORKER");
        if (disable_env && atoi(disable_env) != 0) {
            numeric_thread_running = false;
        } else if (pthread_create(&numeric_thread, NULL, numeric_worker_func, NULL) == 0) {
            numeric_thread_running = true;
        } else {
            fprintf(stderr, "tagcache: failed to spawn numeric worker thread -- running in degraded sync mode\n");
            numeric_thread_running = false;
        }
    }
    pthread_mutex_unlock(&numeric_mutex);
}

void numeric_worker_stop(void) {
    numeric_conds_ready();
    pthread_mutex_lock(&numeric_mutex);
    if (numeric_thread_running) {
        numeric_shutdown = true;
        pthread_cond_broadcast(&numeric_cond);
        pthread_cond_broadcast(&numeric_drain_cond);
        pthread_mutex_unlock(&numeric_mutex);
        pthread_join(numeric_thread, NULL);
        pthread_mutex_lock(&numeric_mutex);
        numeric_thread_running = false;
        numeric_shutdown = false;
    }
    pthread_mutex_unlock(&numeric_mutex);
    tagcache_flush_numeric();
    pthread_mutex_lock(&numeric_mutex);
    /* Anything still queued (a failed flush) is dropped with the session. */
    numeric_settle_waiters_locked(numeric_queue, numeric_queue_count, false);
    numeric_queue_count = 0;
    pthread_cond_broadcast(&numeric_drain_cond);
    pthread_mutex_unlock(&numeric_mutex);
}

/* waiter: when non-NULL, registers a durable waiter for this update and
 * skips the degraded-mode synchronous flush (the waiter flushes later,
 * outside the caller's database lock); *waiter gets its handle, 0 when
 * nothing was queued or no waiter slot was free. */
void queue_numeric_update(int32_t slot, const struct index_entry *idx, int *waiter) {
    if (waiter) *waiter = 0;
    numeric_conds_ready();
    if (!disk_ready || disk_gen < 0) return;
    pthread_mutex_lock(&numeric_mutex);
    int handle = 0;
    if (waiter) {
        for (int w = 0; w < NUMERIC_WAITERS_MAX && !handle; w++)
            if (numeric_waiters[w].state == NUMERIC_WAIT_FREE) handle = w + 1;
        if (!handle) { pthread_mutex_unlock(&numeric_mutex); return; }
    }
    uint64_t ticket = numeric_next_ticket++;
    numeric_update_t *u = numeric_find_locked(numeric_queue, numeric_queue_count, disk_gen, slot);
    bool appended = u == NULL;
    if (appended) {
        /* Queue and in-flight batches share one cap, so a failed flush can
         * always copy its batch back. A distinct slot past that cap is rejected
         * without blocking the caller. */
        if (numeric_queue_count + numeric_inflight_count >= NUMERIC_QUEUE_CAP) {
            numeric_overflow_failed = true;
            pthread_mutex_unlock(&numeric_mutex);
            return;
        }
        u = &numeric_queue[numeric_queue_count++];
        u->gen = disk_gen;
        u->slot = slot;
    }
    numeric_take(u, idx);
    u->ticket = ticket;
    if (handle) {
        numeric_waiters[handle - 1] = (numeric_waiter_t) { NUMERIC_WAIT_PENDING, disk_gen, slot, ticket };
        *waiter = handle;
    }
    /* Only a newly queued entry flushes synchronously in degraded mode. */
    bool sync = appended && !numeric_thread_running && !handle;
    if (numeric_thread_running) pthread_cond_signal(&numeric_cond);
    pthread_mutex_unlock(&numeric_mutex);
    if (sync) tagcache_flush_numeric();
}

bool tagcache_numeric_wait_durable(int waiter, int timeout_ms) {
    if (waiter <= 0 || waiter > NUMERIC_WAITERS_MAX) return false;
    numeric_waiter_t * slot = &numeric_waiters[waiter - 1];
    numeric_conds_ready();
    struct timespec deadline;
    clock_gettime(numeric_clock, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&numeric_mutex);
    while (slot->state == NUMERIC_WAIT_PENDING) {
        if (!numeric_thread_running) {
            /* Degraded sync mode: the enqueue skipped its flush; do it here,
             * outside the caller's database lock. A flush drains or fails
             * the whole queue, so the waiter is settled afterwards. */
            pthread_mutex_unlock(&numeric_mutex);
            tagcache_flush_numeric();
            pthread_mutex_lock(&numeric_mutex);
            if (slot->state == NUMERIC_WAIT_PENDING && !numeric_thread_running) break;
            continue;
        }
        numeric_flush_now = true;
        pthread_cond_signal(&numeric_cond);
        if (pthread_cond_timedwait(&numeric_drain_cond, &numeric_mutex, &deadline) == ETIMEDOUT) break;
    }
    bool written = slot->state == NUMERIC_WAIT_WRITTEN;
    slot->state = NUMERIC_WAIT_FREE;
    pthread_mutex_unlock(&numeric_mutex);
    return written;
}

void numeric_overlay(int32_t slot, struct index_entry *idx) {
    pthread_mutex_lock(&numeric_mutex);
    const numeric_update_t *u = numeric_find_locked(numeric_queue, numeric_queue_count, disk_gen, slot);
    if (!u) u = numeric_find_locked(numeric_inflight, numeric_inflight_count, disk_gen, slot);
    if (u) numeric_apply(idx, u);
    pthread_mutex_unlock(&numeric_mutex);
}

bool tagcache_numeric_write_failed(void) {
    pthread_mutex_lock(&numeric_mutex);
    bool failed = numeric_write_failed || numeric_overflow_failed;
    pthread_mutex_unlock(&numeric_mutex);
    return failed;
}
