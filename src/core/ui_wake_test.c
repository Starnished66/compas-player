#define _POSIX_C_SOURCE 200809L
#include "ui_wake.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static void *producer(void *unused) {
    (void)unused;
    struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    ui_wake_notify(); return NULL;
}
int main(void) {
    unsetenv("COMPAS_EVENT_UI"); ui_wake_init(); assert(!ui_wake_enabled());
    ui_wake_notify();
    setenv("COMPAS_EVENT_UI", "1", 1); ui_wake_init(); assert(ui_wake_enabled());
    pthread_t thread; assert(pthread_create(&thread, NULL, producer, NULL) == 0);
    double start = now_ms();
    assert(ui_wake_wait(-1, 500) == UI_WAKE_WORKER);
    double elapsed = now_ms() - start;
    assert(elapsed < 300); assert(pthread_join(thread, NULL) == 0);
    assert(ui_wake_wait(-1, 0) == 0);
    for (unsigned i = 0; i < 100000; ++i) ui_wake_notify(); // flood never blocks
    while (ui_wake_wait(-1, 0) & UI_WAKE_WORKER) {}
    int touch[2]; assert(pipe(touch) == 0);
    assert(write(touch[1], "x", 1) == 1);
    assert(ui_wake_wait(touch[0], 500) == UI_WAKE_TOUCH);
    char byte; assert(read(touch[0], &byte, 1) == 1 && byte == 'x'); // waiter did not steal input
    ui_wake_notify(); assert(write(touch[1], "y", 1) == 1);
    assert(ui_wake_wait(touch[0], 500) == (UI_WAKE_TOUCH | UI_WAKE_WORKER));
    assert(read(touch[0], &byte, 1) == 1);
    close(touch[1]);
    assert(ui_wake_wait(touch[0], 0) == UI_WAKE_TOUCH_ERROR);
    assert(ui_wake_wait(-1, 0) == 0); // removed/disabled descriptors are not watched
    close(touch[0]);
    printf("UI wake tests passed; worker wake %.2f ms\n", elapsed);
}
