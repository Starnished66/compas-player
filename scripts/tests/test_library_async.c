#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "src/core/process/worker_thread.h"
#include "src/ui/library/browse/gui_library_async.h"

/* The handoff between a UI thread that gives a read up and the worker that may still be running:
 * every request is freed exactly once, by whichever side comes second, and the orphan count
 * returns to zero. */
typedef struct {
    atomic_int fin;
    atomic_int * freed;
    int delay_us;
} request_t;

static atomic_int freed_total;
static atomic_int kind_orphans;

static void free_request(request_t * r) {
    atomic_fetch_add(r->freed, 1);
    free(r);
}

static void * worker(void * arg) {
    request_t * r = arg;
    usleep((useconds_t) r->delay_us);
    if (library_async_worker_done(&r->fin, &kind_orphans)) free_request(r);
    return NULL;
}

int main(void) {
    enum { ROUNDS = 400 };
    unsigned seed = 7;
    int abandoned = 0;
    for (int i = 0; i < ROUNDS; i++) {
        request_t * r = calloc(1, sizeof(*r));
        assert(r);
        r->freed = &freed_total;
        seed = seed * 1103515245u + 12345u;
        r->delay_us = (int) ((seed >> 16) % 400);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, worker, r) == 0);
        seed = seed * 1103515245u + 12345u;
        usleep((useconds_t) ((seed >> 16) % 400));
        if (i % 3 == 0) {
            /* the owner keeps the request: it joins and frees it itself */
            pthread_join(thread, NULL);
            assert(atomic_load(&r->fin) == 1);
            free_request(r);
        } else {
            pthread_detach(thread);
            if (library_async_abandon(&r->fin, &kind_orphans)) free_request(r);
            abandoned++;
        }
    }
    library_async_wait_orphans(worker_deadline_in_ms(5000));
    /* every detached worker has ended: all requests are freed, none twice */
    for (int tries = 0; tries < 500 && atomic_load(&freed_total) < ROUNDS; tries++) usleep(1000);
    assert(atomic_load(&freed_total) == ROUNDS);
    assert(!library_async_kind_has_orphan(&kind_orphans)); /* the per-kind count returned to zero as well */
    assert(abandoned > 0);
    /* two stalled reads of a kind saturate it: no third one is started */
    atomic_int fin_a = 0, fin_b = 0, stalled = 0;
    assert(!library_async_kind_saturated(&stalled));
    assert(!library_async_abandon(&fin_a, &stalled));
    assert(!library_async_kind_saturated(&stalled));
    assert(!library_async_abandon(&fin_b, &stalled));
    assert(library_async_kind_saturated(&stalled));
    assert(library_async_worker_done(&fin_a, &stalled));
    assert(!library_async_kind_saturated(&stalled));
    puts("test_library_async: ok");
    return 0;
}
