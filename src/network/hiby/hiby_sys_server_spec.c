#define _POSIX_C_SOURCE 200809L
#include "hiby_sys_server.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_SERVER_SOCKET_PATH
#error "Compile with SYS_SERVER_SOCKET_PATH set to a test socket path"
#endif

#define CAPTURE_LIMIT 64
#define COMMAND_LIMIT 1024

static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t capture_cond = PTHREAD_COND_INITIALIZER;
static char captured[CAPTURE_LIMIT][COMMAND_LIMIT];
static size_t captured_count;
static atomic_bool slow_next_request;
static atomic_bool reset_peers;
static size_t reset_peer_count;
static bool block_volume_lookup, volume_lookup_entered, release_volume_lookup;
static int listener_fd = -1;
static int volume_lookup_count;
static pthread_t caller_thread;
static bool volume_lookup_on_worker;

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    pthread_mutex_lock(&capture_mutex);
    if (block_volume_lookup) {
        volume_lookup_entered = true;
        pthread_cond_broadcast(&capture_cond);
        while (!release_volume_lookup) pthread_cond_wait(&capture_cond, &capture_mutex);
    }
    volume_lookup_count++;
    volume_lookup_on_worker = !pthread_equal(pthread_self(), caller_thread);
    pthread_cond_broadcast(&capture_cond);
    pthread_mutex_unlock(&capture_mutex);
    snprintf(out, out_size, "01:23:45:67:89:AB");
    return true;
}

static void *server_thread(void *unused) {
    (void) unused;
    for (;;) {
        int fd = accept(listener_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (atomic_load(&reset_peers)) {
            struct linger rst = { .l_onoff = 1, .l_linger = 0 };
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
            close(fd);
            pthread_mutex_lock(&capture_mutex);
            reset_peer_count++;
            pthread_cond_broadcast(&capture_cond);
            pthread_mutex_unlock(&capture_mutex);
            continue;
        }

        char command[COMMAND_LIMIT];
        ssize_t got;
        do { got = recv(fd, command, sizeof(command) - 1, 0); }
        while (got < 0 && errno == EINTR);
        size_t used = got > 0 ? (size_t) got : 0;
        command[used] = '\0';

        pthread_mutex_lock(&capture_mutex);
        bool delay = atomic_exchange(&slow_next_request, false);
        if (captured_count < CAPTURE_LIMIT)
            snprintf(captured[captured_count++], COMMAND_LIMIT, "%s", command);
        pthread_cond_broadcast(&capture_cond);
        pthread_mutex_unlock(&capture_mutex);

        if (delay) {
            /* The command bytes are already received, but withholding the
             * reply keeps the worker in its bounded socket wait while caller
             * updates overwrite metadata and position slots. */
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 450000000 };
            nanosleep(&pause, NULL);
        }
        close(fd);
    }
}

static void start_server(bool slow, bool reset) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYS_SERVER_SOCKET_PATH);
    unlink(addr.sun_path);
    listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener_fd >= 0);
    assert(bind(listener_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    assert(listen(listener_fd, 8) == 0);
    atomic_store(&slow_next_request, slow);
    atomic_store(&reset_peers, reset);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, server_thread, NULL) == 0);
    pthread_detach(thread);
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wait_for_count(size_t count, int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&capture_mutex);
    while (captured_count < count) {
        int result = pthread_cond_timedwait(&capture_cond, &capture_mutex, &deadline);
        if (result == ETIMEDOUT) break;
    }
    bool ready = captured_count >= count;
    pthread_mutex_unlock(&capture_mutex);
    return ready;
}

static void wait_for_reset_peers(size_t count, int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&capture_mutex);
    while (reset_peer_count < count) {
        int result = pthread_cond_timedwait(&capture_cond, &capture_mutex, &deadline);
        if (result == ETIMEDOUT) break;
    }
    bool ready = reset_peer_count >= count;
    pthread_mutex_unlock(&capture_mutex);
    assert(ready);
}

static void wait_for_volume_lookup(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2000 / 1000;
    deadline.tv_nsec += (long) (2000 % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&capture_mutex);
    while (!volume_lookup_entered) {
        int result = pthread_cond_timedwait(&capture_cond, &capture_mutex, &deadline);
        if (result == ETIMEDOUT) break;
    }
    bool ready = volume_lookup_entered;
    pthread_mutex_unlock(&capture_mutex);
    assert(ready);
}

static void release_volume_lookup_gate(void) {
    pthread_mutex_lock(&capture_mutex);
    release_volume_lookup = true;
    pthread_cond_broadcast(&capture_cond);
    pthread_mutex_unlock(&capture_mutex);
}

static void test_nonblocking_coalescing_and_framing(void) {
    start_server(false, false);
    caller_thread = pthread_self();

    pthread_mutex_lock(&capture_mutex);
    block_volume_lookup = true;
    pthread_mutex_unlock(&capture_mutex);
    long long start = monotonic_ms();
    hiby_sys_server_report_volume(50);
    assert(monotonic_ms() - start < 100);
    wait_for_volume_lookup();

    /* Keep the worker inside the slow Bluetooth lookup while replacing the
     * pending metadata/position slots. This makes coalescing deterministic. */
    start = monotonic_ms();
    hiby_sys_server_report_metadata("Old", "Artist", "Album", "Genre", 1);
    hiby_sys_server_report_position(111);
    hiby_sys_server_report_metadata("Café 🚲\tTitle", "Art\nist", "Álbum", "génre", 123456);
    hiby_sys_server_report_position(98765);
    assert(monotonic_ms() - start < 100);
    release_volume_lookup_gate();

    assert(wait_for_count(3, 5000));
    assert(strncmp(captured[0], "BT:ABSVOL:01:23:45:67:89:AB ", 28) == 0);
    assert(strcmp(captured[1], "BT:METADATA:Café 🚲 Title\tArt ist\tÁlbum\tgénre\t123456") == 0);
    assert(strcmp(captured[2], "BT:POSITION:98765") == 0);
    assert(strchr(captured[1], '\n') == NULL && strchr(captured[1], '\r') == NULL);
    assert(strstr(captured[1], "Old") == NULL);

    pthread_mutex_lock(&capture_mutex);
    assert(volume_lookup_count == 1);
    assert(volume_lookup_on_worker);
    pthread_mutex_unlock(&capture_mutex);

    /* Independently verify API calls remain nonblocking while a Unix server
     * has received a command and withholds its response. */
    atomic_store(&slow_next_request, true);
    hiby_sys_server_report_playback_status(true);
    assert(wait_for_count(4, 3000));
    start = monotonic_ms();
    hiby_sys_server_report_playback_status(false);
    assert(monotonic_ms() - start < 100);
    assert(wait_for_count(5, 3000));
    assert(strcmp(captured[4], "BT:PLAYER_STATUS:paused") == 0);
}

static void test_closed_peer_does_not_raise_sigpipe(void) {
    /* Every command uses a new client socket. Force an accepted peer to RST,
     * then restore a healthy listener and prove the worker can report again. */
    atomic_store(&reset_peers, true);
    hiby_sys_server_report_playback_status(true);
    wait_for_reset_peers(1, 2000);
    atomic_store(&reset_peers, false);
    hiby_sys_server_report_playback_status(false);
    assert(wait_for_count(6, 3000));
    assert(strcmp(captured[5], "BT:PLAYER_STATUS:paused") == 0);
}

int main(void) {
    test_nonblocking_coalescing_and_framing();
    test_closed_peer_does_not_raise_sigpipe();
    puts("hiby_sys_server async spec passed");
    return 0;
}
