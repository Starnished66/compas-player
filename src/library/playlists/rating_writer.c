#include "rating_writer.h"
#include "../metadata/db/metadata_db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>

/* Ratings persistence may take global metadata locks, update tagcache, or
 * rewrite and fsync() the remote-state sidecar, so the player's heart never
 * writes on the LVGL thread. A long-lived worker persists queued requests;
 * taps on one path within RATING_DEBOUNCE_MS collapse into the final
 * state, and distinct paths keep independent entries with owned copies. */
#define RATING_QUEUE_INITIAL_CAPACITY 8
#define RATING_DEBOUNCE_MS 150
#define RATING_WORKER_STACK_SIZE (128 * 1024)
/* The worker plus the Wi-Fi and Bluetooth request threads. */
#define RATING_INFLIGHT_MAX 4

/* Every request takes the next sequence number; for one path a higher
 * number is a later request and wins. */
typedef struct {
    char * path;
    int stars;
    struct timespec deadline;
    unsigned long long seq;
} rating_req_t;

/* A synchronous write that is waiting for its turn on a path. */
typedef struct {
    const char * path; /* the caller's string, valid while it waits */
    unsigned long long seq;
} rating_waiter_t;

typedef struct {
    const char * path; /* owned by the writing thread for the write's duration */
    int stars;
} rating_inflight_t;

static pthread_t worker_thread;
static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_cond;
static pthread_once_t writer_once = PTHREAD_ONCE_INIT;
static bool cond_ready = false;
static bool worker_ready = false;

static rating_req_t * queue = NULL;
static int queue_count = 0;
static int queue_capacity = 0;
static rating_inflight_t inflight[RATING_INFLIGHT_MAX];
static int inflight_count = 0;
static rating_waiter_t waiters[RATING_INFLIGHT_MAX];
static int waiter_count = 0;
static unsigned long long next_seq = 1;

/* All helpers below run with writer_mutex held. */
static int queue_find(const char * path) {
    for (int i = 0; i < queue_count; i++) if (strcmp(queue[i].path, path) == 0) return i;
    return -1;
}

static void queue_remove_at(int index) {
    free(queue[index].path);
    memmove(&queue[index], &queue[index + 1], sizeof(*queue) * (size_t) (queue_count - index - 1));
    queue_count--;
}

static int inflight_find(const char * path) {
    for (int i = 0; i < inflight_count; i++) if (strcmp(inflight[i].path, path) == 0) return i;
    return -1;
}

/* True when a synchronous write for path older than seq is still waiting;
 * that write must run (or be skipped as superseded) first. */
static bool older_waiter(const char * path, unsigned long long seq) {
    for (int i = 0; i < waiter_count; i++)
        if (waiters[i].seq < seq && strcmp(waiters[i].path, path) == 0) return true;
    return false;
}

/* True when a request for path newer than seq is waiting or queued. */
static bool newer_request(const char * path, unsigned long long seq) {
    for (int i = 0; i < waiter_count; i++)
        if (waiters[i].seq > seq && strcmp(waiters[i].path, path) == 0) return true;
    int queued = queue_find(path);
    return queued >= 0 && queue[queued].seq > seq;
}

static void waiter_remove(unsigned long long seq) {
    for (int i = 0; i < waiter_count; i++) {
        if (waiters[i].seq != seq) continue;
        waiters[i] = waiters[--waiter_count];
        break;
    }
    pthread_cond_broadcast(&writer_cond);
}

static void inflight_remove(const char * path) {
    for (int i = 0; i < inflight_count; i++) {
        if (inflight[i].path != path) continue;
        inflight[i] = inflight[--inflight_count];
        break;
    }
    pthread_cond_broadcast(&writer_cond);
}

static bool deadline_before(const struct timespec * a, const struct timespec * b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void * worker_main(void * unused) {
    (void) prctl(PR_SET_NAME, "ratings");
    (void) unused;
    pthread_mutex_lock(&writer_mutex);
    for (;;) {
        /* Earliest-due entry whose path is not being written right now and
         * has no older synchronous write still waiting for its turn. */
        int next = -1;
        for (int i = 0; i < queue_count; i++) {
            if (inflight_find(queue[i].path) >= 0 || older_waiter(queue[i].path, queue[i].seq)) continue;
            if (next < 0 || deadline_before(&queue[i].deadline, &queue[next].deadline)) next = i;
        }
        if (next < 0 || inflight_count >= RATING_INFLIGHT_MAX) {
            pthread_cond_wait(&writer_cond, &writer_mutex);
            continue;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (deadline_before(&now, &queue[next].deadline)) {
            struct timespec due = queue[next].deadline;
            pthread_cond_timedwait(&writer_cond, &writer_mutex, &due);
            continue; /* re-evaluate: the queue or in-flight set may have changed */
        }
        char * path = queue[next].path;
        int stars = queue[next].stars;
        memmove(&queue[next], &queue[next + 1], sizeof(*queue) * (size_t) (queue_count - next - 1));
        queue_count--;
        inflight[inflight_count++] = (rating_inflight_t) { path, stars };
        pthread_mutex_unlock(&writer_mutex);

        metadata_db_song_rating_set(path, stars);

        pthread_mutex_lock(&writer_mutex);
        inflight_remove(path);
        free(path);
    }
    return NULL;
}

static void writer_init(void) {
    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0) {
        fprintf(stderr, "rating_writer: failed to initialize condition variable\n");
        return;
    }
    if (pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&writer_cond, &cattr) != 0) {
        fprintf(stderr, "rating_writer: failed to initialize condition variable\n");
        pthread_condattr_destroy(&cattr);
        return;
    }
    pthread_condattr_destroy(&cattr);
    cond_ready = true;

    queue = calloc(RATING_QUEUE_INITIAL_CAPACITY, sizeof(*queue));
    if (!queue) {
        fprintf(stderr, "rating_writer: failed to allocate queue\n");
        return;
    }
    queue_capacity = RATING_QUEUE_INITIAL_CAPACITY;

    pthread_attr_t attr;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) pthread_attr_setstacksize(&attr, RATING_WORKER_STACK_SIZE);
    int create_rc = pthread_create(&worker_thread, attr_initialized ? &attr : NULL, worker_main, NULL);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc == 0) {
        pthread_detach(worker_thread);
        worker_ready = true;
    } else {
        fprintf(stderr, "rating_writer: failed to spawn worker thread\n");
        free(queue);
        queue = NULL;
        queue_capacity = 0;
    }
}

void rating_writer_start(void) {
    pthread_once(&writer_once, writer_init);
}

/* Caller holds writer_mutex. */
static void queue_append(const char * path, int stars, struct timespec deadline, unsigned long long seq) {
    char * path_copy = strdup(path);
    if (!path_copy) {
        fprintf(stderr, "rating_writer: strdup failed for rating path '%s'\n", path);
        return;
    }
    /* Grow only for distinct tracks, so the LVGL thread never waits on slow
     * metadata or SD-card I/O and an acknowledged change is never evicted. */
    if (queue_count >= queue_capacity) {
        int new_capacity = queue_capacity * 2;
        rating_req_t * grown = realloc(queue, sizeof(*queue) * (size_t) new_capacity);
        if (!grown) {
            fprintf(stderr, "rating_writer: failed to grow queue\n");
            free(path_copy);
            return;
        }
        queue = grown;
        queue_capacity = new_capacity;
    }
    queue[queue_count++] = (rating_req_t) { path_copy, stars, deadline, seq };
}

void rating_writer_submit(const char * path, int stars) {
    if (!path) return;
    pthread_once(&writer_once, writer_init);
    if (!worker_ready) {
        fprintf(stderr, "rating_writer: worker not ready; dropping async persistence\n");
        return;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long) RATING_DEBOUNCE_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    pthread_mutex_lock(&writer_mutex);
    unsigned long long seq = next_seq++;
    int existing = queue_find(path);
    if (existing >= 0) {
        queue[existing].stars = stars;
        queue[existing].deadline = deadline;
        queue[existing].seq = seq;
        pthread_cond_broadcast(&writer_cond);
        pthread_mutex_unlock(&writer_mutex);
        return;
    }
    queue_append(path, stars, deadline, seq);
    pthread_cond_broadcast(&writer_cond);
    pthread_mutex_unlock(&writer_mutex);
}

bool rating_writer_flush(void) {
    pthread_once(&writer_once, writer_init);
    if (!worker_ready) return true;
    pthread_mutex_lock(&writer_mutex);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < queue_count; i++) queue[i].deadline = now;
    pthread_cond_broadcast(&writer_cond);
    struct timespec deadline = now;
    deadline.tv_sec += 5;
    while (queue_count > 0 || inflight_count > 0) {
        if (pthread_cond_timedwait(&writer_cond, &writer_mutex, &deadline) != 0) break;
    }
    bool drained = queue_count == 0 && inflight_count == 0;
    pthread_mutex_unlock(&writer_mutex);
    return drained;
}

bool rating_writer_write_now(const char * path, int stars) {
    if (!path) return false;
    pthread_once(&writer_once, writer_init);
    if (!cond_ready) return false;
    pthread_mutex_lock(&writer_mutex);
    while (waiter_count >= RATING_INFLIGHT_MAX) pthread_cond_wait(&writer_cond, &writer_mutex);
    unsigned long long seq = next_seq++;
    int pending = queue_find(path);
    if (pending >= 0) queue_remove_at(pending); /* this write is the later one */
    /* Registered in request order before waiting, so a newer heart tap
     * cannot be written ahead of it and then overwritten by it. */
    waiters[waiter_count++] = (rating_waiter_t) { path, seq };
    while (inflight_find(path) >= 0 || older_waiter(path, seq) || inflight_count >= RATING_INFLIGHT_MAX)
        pthread_cond_wait(&writer_cond, &writer_mutex);
    waiter_remove(seq);
    if (newer_request(path, seq)) {
        /* A later request for this path replaces this one; writing now
         * would put the older value last. */
        pthread_mutex_unlock(&writer_mutex);
        return true;
    }
    inflight[inflight_count++] = (rating_inflight_t) { path, stars };
    pthread_mutex_unlock(&writer_mutex);

    bool saved = metadata_db_song_rating_set_durable(path, stars);

    pthread_mutex_lock(&writer_mutex);
    inflight_remove(path);
    pthread_mutex_unlock(&writer_mutex);
    return saved;
}

int rating_writer_effective_stars(const char *path, int stored) {
    if (!path) return 0;
    pthread_mutex_lock(&writer_mutex);
    int queued = queue_find(path);
    int writing = queued < 0 ? inflight_find(path) : -1;
    bool known = queued >= 0 || writing >= 0;
    int value = queued >= 0 ? queue[queued].stars : writing >= 0 ? inflight[writing].stars : 0;
    pthread_mutex_unlock(&writer_mutex);
    return known ? value : stored;
}

int rating_writer_stars(const char *path) {
    int pending = rating_writer_effective_stars(path, -1);
    return pending >= 0 ? pending : metadata_db_song_rating_get(path);
}
