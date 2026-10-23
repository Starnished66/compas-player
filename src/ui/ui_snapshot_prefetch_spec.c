#define _GNU_SOURCE
#include "ui_snapshot_prefetch.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <sched.h>

bool ui_snapshot_prefetch_test_wait_idle(unsigned timeout_ms);
void ui_snapshot_prefetch_test_arm_barrier(void);
bool ui_snapshot_prefetch_test_wait_barrier(unsigned timeout_ms);
void ui_snapshot_prefetch_test_release_barrier(void);

static bool request_retry(const ui_snapshot_prefetch_span_t *spans, size_t count) {
    for (unsigned int i = 0; i < 2000; ++i) {
        if (ui_snapshot_prefetch_request(spans, count)) return true;
        sched_yield();
    }
    return false;
}

static void wait_idle(void) {
    assert(ui_snapshot_prefetch_test_wait_idle(2000));
}

static void test_validation_and_replacement(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    uint8_t *a = mmap(NULL, (size_t)page * 3, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(a != MAP_FAILED && b != MAP_FAILED);
    memset(a, 0x55, (size_t)page * 3);
    memset(b, 0xaa, (size_t)page * 2);
    int id_a, id_b;
    ui_snapshot_prefetch_span_t spans[] = {
        { &id_a, a + 19, (size_t)page * 2 + 31 },
        { &id_b, b + 7, (size_t)page + 11 },
    };
    assert(request_retry(spans, 2));
    /* Replacing pending work must never retain the first request's pointers. */
    assert(request_retry(&spans[1], 1));
    ui_snapshot_prefetch_forget(&id_b);
    assert(munmap(b, (size_t)page * 2) == 0);
    wait_idle();
    assert(munmap(a, (size_t)page * 3) == 0);

    assert(!ui_snapshot_prefetch_request(NULL, 1));
    ui_snapshot_prefetch_span_t bad = { NULL, a, 1 };
    assert(!ui_snapshot_prefetch_request(&bad, 1));
    bad.identity = &id_a;
    bad.data = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 2);
    bad.size = 8;
    assert(!ui_snapshot_prefetch_request(&bad, 1));
}

typedef struct {
    const void *identity;
    atomic_bool started;
    atomic_bool done;
} forget_args_t;

static void *forget_thread(void *opaque) {
    forget_args_t *args = opaque;
    atomic_store_explicit(&args->started, true, memory_order_release);
    ui_snapshot_prefetch_forget(args->identity);
    atomic_store_explicit(&args->done, true, memory_order_release);
    return NULL;
}

static void test_nonblocking_request_and_forget_barrier(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    size_t length = (size_t)page * 8;
    uint8_t *memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0x4c, length);
    int identity, other_identity;
    ui_snapshot_prefetch_span_t span = { &identity, memory, length };
    ui_snapshot_prefetch_span_t contended = { &other_identity, memory, length };

    ui_snapshot_prefetch_test_arm_barrier();
    assert(ui_snapshot_prefetch_request(&span, 1));
    assert(ui_snapshot_prefetch_test_wait_barrier(2000));

    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    assert(!ui_snapshot_prefetch_request(&contended, 1));
    clock_gettime(CLOCK_MONOTONIC, &after);
    int64_t elapsed_ns = (int64_t)(after.tv_sec - before.tv_sec) * INT64_C(1000000000) +
                         (int64_t)after.tv_nsec - (int64_t)before.tv_nsec;
    assert(elapsed_ns >= 0 && elapsed_ns < INT64_C(50000000));

    forget_args_t args = { .identity = &identity };
    atomic_init(&args.started, false);
    atomic_init(&args.done, false);
    pthread_t forgetter;
    assert(pthread_create(&forgetter, NULL, forget_thread, &args) == 0);
    while (!atomic_load_explicit(&args.started, memory_order_acquire)) sched_yield();
    usleep(1000);
    assert(!atomic_load_explicit(&args.done, memory_order_acquire));
    ui_snapshot_prefetch_test_release_barrier();
    assert(pthread_join(forgetter, NULL) == 0);
    assert(atomic_load_explicit(&args.done, memory_order_acquire));
    assert(munmap(memory, length) == 0);
    wait_idle();
}

static void test_cancel_and_concurrent_forget(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    const size_t length = (size_t)page * 64;
    uint8_t *memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0x72, length);
    int identity;
    ui_snapshot_prefetch_span_t span = { &identity, memory, length };
    assert(request_retry(&span, 1));
    ui_snapshot_prefetch_cancel_all();
    ui_snapshot_prefetch_forget(&identity);
    assert(munmap(memory, length) == 0);
    wait_idle();
}

int main(void) {
    test_validation_and_replacement();

    test_nonblocking_request_and_forget_barrier();
    test_cancel_and_concurrent_forget();
    puts("PASS snapshot prefetch lifetime/cancel tests");
    return 0;
}
