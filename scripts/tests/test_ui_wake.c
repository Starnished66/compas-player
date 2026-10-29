#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "src/core/ui/display/ui_wake.h"

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void * notifier(void * arg) {
    (void) arg;
    usleep(30 * 1000);
    ui_wake_notify();
    return NULL;
}

int main(void) {
    unsetenv("COMPAS_EVENT_UI");
    ui_wake_init();
    /* The pipe exists for workers, but the full event loop stays opt-in. */
    assert(!ui_wake_enabled());
    assert(!ui_wake_take_jobs());

    /* A worker's notify cuts a long sleep short and is reported once. */
    pthread_t thread;
    assert(pthread_create(&thread, NULL, notifier, NULL) == 0);
    long start = now_ms();
    ui_wake_sleep(2000);
    long slept = now_ms() - start;
    pthread_join(thread, NULL);
    assert(slept < 1000);
    assert(ui_wake_take_jobs());
    assert(!ui_wake_take_jobs());

    /* Nothing pending: the sleep lasts about as long as asked. */
    start = now_ms();
    ui_wake_sleep(60);
    slept = now_ms() - start;
    assert(slept >= 50 && slept < 500);
    assert(!ui_wake_take_jobs());

    /* A notify that arrives before the sleep is not lost. */
    ui_wake_notify();
    start = now_ms();
    ui_wake_sleep(2000);
    assert(now_ms() - start < 500);
    assert(ui_wake_take_jobs());
    puts("ui wake tests passed");
    return 0;
}
