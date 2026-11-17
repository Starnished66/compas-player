#include "remote_reply.h"

#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define LINGER_MAX_SECONDS 10

/* Reads and discards up to `remaining` bytes, bounded in bytes and time. */
static void drain_body(int cfd, size_t remaining) {
    if (remaining > REPLY_DRAIN_MAX_BYTES) remaining = REPLY_DRAIN_MAX_BYTES;
    time_t give_up = time(NULL) + LINGER_MAX_SECONDS;
    char buffer[1024];
    while (remaining > 0 && time(NULL) < give_up) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return;
        ssize_t n = read(cfd, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        remaining -= (size_t) n;
    }
}

void send_api_error_drain(int cfd, const char * status, const char * error, const char * code,
                          size_t remaining) {
    send_api_error(cfd, status, error, code);
    if (remaining == 0) return;
    shutdown(cfd, SHUT_WR);
    drain_body(cfd, remaining);
}
