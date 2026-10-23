#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui_snapshot_prefetch.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#ifdef UI_SNAPSHOT_PREFETCH_TEST
#include <stdatomic.h>
#endif
#include <unistd.h>

#define SNAPSHOT_PREFETCH_JOB_COUNT 2
#define SNAPSHOT_PREFETCH_STACK_SIZE (64u * 1024u)

#ifndef UI_SNAPSHOT_PREFETCH_DISABLE
typedef struct {
    const void *identity;
    const uint8_t *data;
    size_t size;
    size_t offset;
    bool active;
} prefetch_job_t;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef UI_SNAPSHOT_PREFETCH_TEST
static pthread_cond_t idle_cond = PTHREAD_COND_INITIALIZER;
#endif
static prefetch_job_t jobs[SNAPSHOT_PREFETCH_JOB_COUNT];
static size_t page_size;
static bool worker_running;
#ifdef UI_SNAPSHOT_PREFETCH_TEST
static atomic_bool test_pause_worker;
static atomic_bool test_worker_paused;
#endif

static bool span_valid(const ui_snapshot_prefetch_span_t *span) {
    if (!span || !span->identity || !span->data || !span->size) return false;
    uintptr_t start = (uintptr_t)span->data;
    return span->size <= UINTPTR_MAX - start;
}

static size_t get_page_size(void) {
    if (page_size) return page_size;
    long value = sysconf(_SC_PAGESIZE);
    if (value <= 0 || (uintmax_t)value > SIZE_MAX) return 0;
    page_size = (size_t)value;
    return page_size;
}

/* One volatile read is enough to fault in the page containing offset; keep
 * the lock window independent of page size and snapshot byte count. */
static void touch_one_page(const uint8_t *data, size_t offset) {
    volatile uint8_t value = data[offset];
    (void)value;
}

static bool next_page_locked(void) {
    for (size_t i = 0; i < SNAPSHOT_PREFETCH_JOB_COUNT; ++i) {
        prefetch_job_t *job = &jobs[i];
        if (!job->active) continue;
#ifdef UI_SNAPSHOT_PREFETCH_TEST
        if (atomic_load_explicit(&test_pause_worker, memory_order_acquire)) {
            atomic_store_explicit(&test_worker_paused, true, memory_order_release);
            while (atomic_load_explicit(&test_pause_worker, memory_order_acquire))
                sched_yield();
        }
#endif
        touch_one_page(job->data, job->offset);
        uintptr_t address = (uintptr_t)job->data + job->offset;
        size_t step = page_size - (address % page_size);
        if (step >= job->size - job->offset) {
            memset(job, 0, sizeof(*job));
        } else {
            job->offset += step;
        }
        return true;
    }
    return false;
}

static void *worker_main(void *unused) {
    (void)unused;
#ifdef SYS_gettid
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    for (;;) {
        pthread_mutex_lock(&state_lock);
        bool had_work = next_page_locked();
        if (!had_work) {
            worker_running = false;
#ifdef UI_SNAPSHOT_PREFETCH_TEST
            pthread_cond_broadcast(&idle_cond);
#endif
        }
        pthread_mutex_unlock(&state_lock);
        if (!had_work) return NULL;
        sched_yield();
    }
}

bool ui_snapshot_prefetch_request(const ui_snapshot_prefetch_span_t *spans,
                                  size_t count) {
    if (!spans || count == 0 || count > SNAPSHOT_PREFETCH_JOB_COUNT) return false;
    size_t bytes = get_page_size();
    if (!bytes) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!span_valid(&spans[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (spans[i].identity == spans[j].identity) return false;
    }

    if (pthread_mutex_trylock(&state_lock) != 0) return false;
    memset(jobs, 0, sizeof(jobs));
    for (size_t i = 0; i < count; ++i) {
        jobs[i] = (prefetch_job_t){
            .identity = spans[i].identity,
            .data = spans[i].data,
            .size = spans[i].size,
            .active = true,
        };
    }

    if (!worker_running) {
        pthread_attr_t attr;
        int rc = pthread_attr_init(&attr);
        bool attr_initialized = rc == 0;
        if (rc == 0) rc = pthread_attr_setstacksize(&attr, SNAPSHOT_PREFETCH_STACK_SIZE);
        if (rc == 0) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (rc != 0) {
            if (attr_initialized) pthread_attr_destroy(&attr);
            memset(jobs, 0, sizeof(jobs));
            pthread_mutex_unlock(&state_lock);
            return false;
        }
        pthread_t thread;
        rc = pthread_create(&thread, &attr, worker_main, NULL);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            memset(jobs, 0, sizeof(jobs));
            pthread_mutex_unlock(&state_lock);
            return false;
        }
        worker_running = true;
    }
    pthread_mutex_unlock(&state_lock);
    return true;
}

void ui_snapshot_prefetch_cancel_all(void) {
    pthread_mutex_lock(&state_lock);
    memset(jobs, 0, sizeof(jobs));
    pthread_mutex_unlock(&state_lock);
}

void ui_snapshot_prefetch_forget(const void *identity) {
    if (!identity) return;
    pthread_mutex_lock(&state_lock);
    for (size_t i = 0; i < SNAPSHOT_PREFETCH_JOB_COUNT; ++i) {
        if (jobs[i].active && jobs[i].identity == identity)
            memset(&jobs[i], 0, sizeof(jobs[i]));
    }
    pthread_mutex_unlock(&state_lock);
}
#ifdef UI_SNAPSHOT_PREFETCH_TEST
void ui_snapshot_prefetch_test_arm_barrier(void) {
    atomic_store_explicit(&test_worker_paused, false, memory_order_relaxed);
    atomic_store_explicit(&test_pause_worker, true, memory_order_release);
}
bool ui_snapshot_prefetch_test_wait_barrier(unsigned timeout_ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        if (atomic_load_explicit(&test_worker_paused, memory_order_acquire)) return true;
        sched_yield();
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL +
                             (int64_t)now.tv_nsec - (int64_t)start.tv_nsec;
        if (elapsed_ns >= (int64_t)timeout_ms * 1000000LL) break;
    } while (true);
    return atomic_load_explicit(&test_worker_paused, memory_order_acquire);
}
void ui_snapshot_prefetch_test_release_barrier(void) {
    atomic_store_explicit(&test_pause_worker, false, memory_order_release);
}
bool ui_snapshot_prefetch_test_wait_idle(unsigned timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t nsec = (uint64_t)deadline.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
    deadline.tv_sec += (time_t)(nsec / 1000000000ULL);
    deadline.tv_nsec = (long)(nsec % 1000000000ULL);
    pthread_mutex_lock(&state_lock);
    int rc = 0;
    while (worker_running && rc == 0)
        rc = pthread_cond_timedwait(&idle_cond, &state_lock, &deadline);
    bool idle = !worker_running;
    pthread_mutex_unlock(&state_lock);
    return idle;
}
#endif
#else
bool ui_snapshot_prefetch_request(const ui_snapshot_prefetch_span_t *spans,
                                  size_t count) {
    (void)spans;
    (void)count;
    return false;
}
void ui_snapshot_prefetch_cancel_all(void) { }
void ui_snapshot_prefetch_forget(const void *identity) { (void)identity; }
#endif
