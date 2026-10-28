#include "wifi_status.h"
#include "../../core/process/subprocess.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WIFI_INTERFACE "wlan0"
#define WIFI_QUERY_TIMEOUT_MS 1500

#define WPA_CTRL_SOCKET "/var/run/wpa_supplicant/" WIFI_INTERFACE

/* Asks wpa_supplicant over its control socket, the same datagram exchange wpa_cli performs, without
 * spawning a process every status poll. Returns false on any problem (no socket, no reply, FAIL,
 * oversized reply) and the caller then runs wpa_cli exactly as before. */
static int64_t wpa_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits for `events` on fd until the shared deadline; false on timeout or error. */
static bool wpa_wait(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t remaining = deadline - wpa_now_ms();
        if (remaining <= 0) return false;
        struct pollfd pfd = { .fd = fd, .events = events };
        int pr = poll(&pfd, 1, (int) remaining);
        if (pr > 0) return (pfd.revents & events) != 0;
        if (pr == 0 || errno != EINTR) return false;
    }
}

static bool wpa_ctrl_query(const char * command, char * out_buf, size_t out_buf_size) {
    static atomic_uint serial;
    struct sockaddr_un local = { .sun_family = AF_UNIX }, remote = { .sun_family = AF_UNIX };
    if (strlen(WPA_CTRL_SOCKET) >= sizeof(remote.sun_path)) return false;
    strcpy(remote.sun_path, WPA_CTRL_SOCKET);
    /* Nonblocking, with one deadline for connect, send and receive together: a wpa_supplicant that
     * stopped draining its queue must not be able to hold the caller (a UI-adjacent path). */
    int64_t deadline = wpa_now_ms() + WIFI_QUERY_TIMEOUT_MS;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/compas_wpa_%d_%u", (int) getpid(),
             atomic_fetch_add(&serial, 1u));
    unlink(local.sun_path);
    bool ok = false;
    if (bind(fd, (struct sockaddr *) &local, sizeof(local)) == 0) {
        size_t len = strlen(command);
        if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) == 0) {
            bool sent = false;
            for (;;) {
                if (wpa_now_ms() >= deadline) break; /* checked before every attempt, retries after EINTR included */
                ssize_t n = send(fd, command, len, MSG_DONTWAIT);
                if (n == (ssize_t) len) { sent = true; break; }
                if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
                if (errno != EINTR && !wpa_wait(fd, POLLOUT, deadline)) break;
            }
            char reply[4096];
            if (sent && wpa_wait(fd, POLLIN, deadline)) {
                ssize_t n = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC | MSG_DONTWAIT);
                /* MSG_TRUNC reports the real datagram size: a reply that did not fit goes to wpa_cli */
                if (n > 0 && (size_t) n < sizeof(reply) - 1 && out_buf_size > 1 &&
                    strncmp(reply, "FAIL", 4) != 0) {
                    /* Truncate into the caller's buffer like the subprocess path does */
                    size_t copy = (size_t) n < out_buf_size - 1 ? (size_t) n : out_buf_size - 1;
                    memcpy(out_buf, reply, copy);
                    out_buf[copy] = '\0';
                    ok = true;
                }
            }
        }
        unlink(local.sun_path);
    }
    close(fd);
    return ok;
}

/* Runs `wpa_cli -i wlan0 <args>` directly and reads its stdout into out_buf.
 * `args` is always one of this file's own literal strings below. Returns
 * false if wpa_cli cannot be run, times out, exits unsuccessfully, or produces
 * no output -- all normal and expected on the host simulator. */
static bool run_wpa_cli(const char * args, char * out_buf, size_t out_buf_size) {
    const char * command = strcmp(args, "status") == 0 ? "STATUS" : strcmp(args, "signal_poll") == 0 ? "SIGNAL_POLL" : NULL;
    if (command && wpa_ctrl_query(command, out_buf, out_buf_size) && out_buf[0] != '\0') return true;
    char * argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) args, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, out_buf, out_buf_size, WIFI_QUERY_TIMEOUT_MS, &exit_code) && exit_code == 0 &&
           out_buf[0] != '\0';
}

bool wifi_get_status(int * out_signal_level) {
    char status_buf[512];
    if (!run_wpa_cli("status", status_buf, sizeof(status_buf))) return false;
    if (!strstr(status_buf, "wpa_state=COMPLETED")) return false;

    int level = 0;
    char signal_buf[512];
    if (run_wpa_cli("signal_poll", signal_buf, sizeof(signal_buf))) {
        const char * rssi_pos = strstr(signal_buf, "RSSI=");
        if (rssi_pos) level = wifi_signal_level_from_rssi(atoi(rssi_pos + 5));
    }

    *out_signal_level = level;
    return true;
}
