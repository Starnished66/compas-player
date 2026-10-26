/* Host characterization for the minimal init splash entry point. */
#include "../display/fb_draw.h"
#include "../install/internal_storage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EV_STORAGE = 1, EV_OPEN, EV_DRAW, EV_FLUSH, EV_CLOSE };
static int events[8];
static size_t event_count;
static bool storage_ok, open_ok, draw_ok;

static void record(int event) { events[event_count++] = event; }

bool internal_storage_ready(char *reason, size_t reason_size) {
    record(EV_STORAGE);
    assert(reason != NULL && reason_size > 0);
    (void)snprintf(reason, reason_size, "mock");
    return storage_ok;
}
bool fb_open(void) { record(EV_OPEN); return open_ok; }
bool fb_draw_startup_background(void) { record(EV_DRAW); return draw_ok; }
void fb_flush(void) { record(EV_FLUSH); }
void fb_close(void) { record(EV_CLOSE); }

int early_splash_run(void);

static void reset(bool storage, bool open, bool draw) {
    event_count = 0;
    memset(events, 0, sizeof(events));
    storage_ok = storage;
    open_ok = open;
    draw_ok = draw;
}
static void expect(const int *wanted, size_t count) {
    assert(event_count == count);
    assert(memcmp(events, wanted, count * sizeof(*wanted)) == 0);
}

int main(void) {
    static const int success[] = {EV_STORAGE, EV_OPEN, EV_DRAW, EV_FLUSH, EV_CLOSE};
    static const int storage_failure[] = {EV_STORAGE};
    static const int open_failure[] = {EV_STORAGE, EV_OPEN};
    static const int draw_failure[] = {EV_STORAGE, EV_OPEN, EV_DRAW, EV_CLOSE};

    reset(true, true, true);
    assert(early_splash_run() == 0);
    expect(success, sizeof(success) / sizeof(success[0]));

    reset(false, true, true);
    assert(early_splash_run() != 0);
    expect(storage_failure, sizeof(storage_failure) / sizeof(storage_failure[0]));

    reset(true, false, true);
    assert(early_splash_run() != 0);
    expect(open_failure, sizeof(open_failure) / sizeof(open_failure[0]));

    reset(true, true, false);
    assert(early_splash_run() != 0);
    expect(draw_failure, sizeof(draw_failure) / sizeof(draw_failure[0]));
    return 0;
}
