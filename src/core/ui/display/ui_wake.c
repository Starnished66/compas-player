#include "ui_wake.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wake_pipe[2] = { -1, -1 };
/* The full event loop (touch fd included) is the COMPAS_EVENT_UI=1 prototype.
 * The pipe itself always exists, so a finished worker can cut the main loop's
 * sleep short and have its result applied at once. */
static bool event_mode;
/* Set by every notify, taken by the owner loop: "a worker published something". */
static atomic_bool jobs_pending;

void ui_wake_init(void) {
    const char *flag = getenv("COMPAS_EVENT_UI");
    if (wake_pipe[0] >= 0) return;
    event_mode = flag && strcmp(flag, "1") == 0;
    int fds[2];
    if (pipe(fds) != 0) return;
    for (unsigned i = 0; i < 2; ++i) {
        if (fcntl(fds[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
            close(fds[0]); close(fds[1]); event_mode = false; return;
        }
    }
    wake_pipe[0] = fds[0];
    wake_pipe[1] = fds[1];
}

bool ui_wake_enabled(void) { return event_mode && wake_pipe[0] >= 0; }

bool ui_wake_take_jobs(void) { return atomic_exchange(&jobs_pending, false); }

void ui_wake_sleep(uint32_t timeout_ms) {
    if (wake_pipe[0] < 0) { usleep(timeout_ms * 1000u); return; }
    struct pollfd fd = { .fd = wake_pipe[0], .events = POLLIN };
    int timeout = timeout_ms > INT_MAX ? INT_MAX : (int) timeout_ms;
    if (poll(&fd, 1, timeout) > 0 && (fd.revents & POLLIN)) {
        unsigned char bytes[256];
        for (unsigned i = 0; i < 16; ++i)
            if (read(wake_pipe[0], bytes, sizeof(bytes)) <= 0) break;
    }
}

void ui_wake_notify(void) {
    atomic_store(&jobs_pending, true);
    if (wake_pipe[1] < 0) return;
    const unsigned char byte = 1;
    ssize_t n;
    do { n = write(wake_pipe[1], &byte, 1); } while (n < 0 && errno == EINTR);
    /* EAGAIN means a notification is already pending. */
}

unsigned ui_wake_wait(int touch_fd, uint32_t timeout_ms) {
    struct pollfd fds[2] = {
        { .fd = wake_pipe[0], .events = POLLIN },
        { .fd = touch_fd, .events = POLLIN }
    };
    int timeout = timeout_ms > INT_MAX ? INT_MAX : (int) timeout_ms;
    int result = poll(fds, 2, timeout);
    /* EINTR returns to the owner loop to recompute timer deadlines. */
    if (result <= 0) return 0;
    unsigned ready = 0;
    if (fds[0].revents & POLLIN) {
        ready |= UI_WAKE_WORKER;
        unsigned char bytes[256];
        /* Bound draining under a producer flood; the pipe stays readable. */
        for (unsigned i = 0; i < 16; ++i) {
            if (read(wake_pipe[0], bytes, sizeof(bytes)) <= 0) break;
        }
    }
    if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) ready |= UI_WAKE_TOUCH_ERROR;
    else if (fds[1].revents & POLLIN) ready |= UI_WAKE_TOUCH;
    return ready;
}
