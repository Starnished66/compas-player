#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>

/* Background worker helpers: a lowered scheduling priority and a small explicit
 * stack, so list and page workers never compete with presentation or audio. */

/* Lowers the calling thread to at least `nice_value` without raising an
 * inherited background priority. Linux applies setpriority(PRIO_PROCESS, 0)
 * to the calling thread only.
 *
 * The panel pan blocks the UI thread until vsync, so a frame has 40-60% idle
 * time; a worker only has to stay out of the way of the other 40-60%. Under
 * CFS a nice 10 thread gets about 10% of a core while the UI thread is
 * runnable and a nice 19 thread about 1.5%, so neither stretches a draw pass.
 * WORKER_NICE_BACKGROUND: work somebody is waiting for (page loads, visible
 * thumbnails, the album cover, the track probes and duration lookups a page
 * requested, the duration cache load). WORKER_NICE_IDLE: opportunistic work
 * (boot warm-up, prune, cache merge and write-back). Not SCHED_IDLE on purpose:
 * a SCHED_IDLE thread preempted while it holds the metadata lock would stall the
 * UI thread. For the same reason nothing at IDLE holds the metadata lock across
 * allocations or callbacks: the walks copy a batch under it and visit outside. */
#define WORKER_NICE_BACKGROUND 10
#define WORKER_NICE_IDLE 19

static inline void worker_lower_priority(int nice_value) {
#ifdef __linux__
    errno = 0;
    int inherited = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && inherited < nice_value) (void) setpriority(PRIO_PROCESS, 0, nice_value);
#else
    (void) nice_value;
#endif
}

/* Raises a thread that dropped itself to IDLE back to `nice_value` for work somebody waits
 * for. Only the superuser may lower a nice level again; elsewhere the call is refused and
 * the thread simply keeps the lower priority. */
static inline void worker_raise_priority(int nice_value) {
#ifdef __linux__
    errno = 0;
    int current = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && current > nice_value) (void) setpriority(PRIO_PROCESS, 0, nice_value);
#else
    (void) nice_value;
#endif
}

/* Starts a joinable thread with `stack_bytes` of stack (falls back to the
 * default if the size is refused). Returns the pthread_create result. */
static inline int worker_thread_create(pthread_t * thread, size_t stack_bytes, void * (*fn)(void *), void * arg) {
    pthread_attr_t attr;
    pthread_attr_t * use = NULL;
    int inited = pthread_attr_init(&attr) == 0;
    if (inited && pthread_attr_setstacksize(&attr, stack_bytes) == 0) use = &attr;
    int rc = pthread_create(thread, use, fn, arg);
    if (inited) pthread_attr_destroy(&attr);
    return rc;
}

/* One shared deadline for final shutdown: every worker wait takes what is left
 * of it, so a stuck card costs the total once, not once per worker. */
static inline int64_t worker_deadline_in_ms(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + ms;
}

static inline int worker_ms_left(int64_t deadline_ms) {
    int64_t left = deadline_ms - worker_deadline_in_ms(0);
    return left > 0 ? (int) left : 0;
}

#endif
