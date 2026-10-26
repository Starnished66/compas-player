#define _GNU_SOURCE
#include "remote_import_files.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef EXPECT_TIMEOUT_SECONDS
#define EXPECT_TIMEOUT_SECONDS 12
#endif

bool file_op_external_begin(void) { return true; }
void file_op_external_end(void) {}
bool usb_mode_control_storage_write_begin(void) { return true; }
void usb_mode_control_storage_write_end(void) {}

int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
ssize_t __real_write(int fd, const void *buffer, size_t size);

typedef enum { CLOCK_REAL, CLOCK_TIMEOUT, CLOCK_PROGRESS, CLOCK_HOLD,
               CLOCK_BOUNDARY, CLOCK_WRITE_DELAY, CLOCK_SYNC_DELAY } clock_mode_t;
static atomic_int test_clock_mode = CLOCK_REAL;
static atomic_llong fake_clock_ms;
static atomic_int progress_bytes;
static atomic_bool poll_entered;
static atomic_bool release_poll;
static atomic_bool fail_next_write;
static atomic_bool delay_next_write;
static atomic_bool delay_next_sync;
static int current_peer_fd = -1;

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value) {
    if (clock_id == CLOCK_MONOTONIC && atomic_load(&test_clock_mode) != CLOCK_REAL) {
        long long ms = atomic_load(&fake_clock_ms);
        value->tv_sec = (time_t)(ms / 1000);
        value->tv_nsec = (long)(ms % 1000) * 1000000L;
        return 0;
    }
    return __real_clock_gettime(clock_id, value);
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    int mode = atomic_load(&test_clock_mode);
    if (mode == CLOCK_REAL) return __real_poll(fds, count, timeout);
    if (mode == CLOCK_WRITE_DELAY || mode == CLOCK_SYNC_DELAY)
        return __real_poll(fds, count, timeout);
    if (mode == CLOCK_HOLD) {
        atomic_store(&poll_entered, true);
        while (!atomic_load(&release_poll)) sched_yield();
        return 0;
    }
    if (mode == CLOCK_BOUNDARY) {
        static const int increments[] = {11001, 998, 1};
        int index = atomic_fetch_add(&progress_bytes, 1);
        assert(index < (int)(sizeof(increments) / sizeof(increments[0])));
        atomic_fetch_add(&fake_clock_ms, increments[index]);
        return 0;
    }
    int step_ms = mode == CLOCK_PROGRESS ? 7000 : timeout;
    atomic_fetch_add(&fake_clock_ms, step_ms);
    if (mode == CLOCK_PROGRESS) {
        int index = atomic_fetch_add(&progress_bytes, 1);
        if (index < 3) {
            static const char bytes[] = "xyz";
            assert(current_peer_fd >= 0);
            assert(send(current_peer_fd, &bytes[index], 1, MSG_NOSIGNAL) == 1);
            fds[0].revents = POLLIN;
            return 1;
        }
    }
    (void)fds;
    (void)count;
    return 0;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
    if (atomic_exchange(&fail_next_write, false)) {
        errno = ENOSPC;
        return -1;
    }
    ssize_t written = __real_write(fd, buffer, size);
    if (written > 0 && atomic_exchange(&delay_next_write, false))
        atomic_fetch_add(&fake_clock_ms, 13000);
    return written;
}

int __real_fdatasync(int fd);
int __wrap_fdatasync(int fd) {
    int result = __real_fdatasync(fd);
    if (result == 0 && atomic_exchange(&delay_next_sync, false))
        atomic_fetch_add(&fake_clock_ms, 13000);
    return result;
}

typedef struct {
    int fd;
    char path[256];
    long long length;
} request_t;

static void *serve_upload(void *opaque) {
    request_t *request = opaque;
    assert(remote_import_files_handle(request->fd, "POST", request->path,
                                      request->length, true));
    close(request->fd);
    return NULL;
}

static pthread_t start_upload(const char *name, long long length, int *peer_out,
                              request_t *request) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    request->fd = pair[0];
    assert(snprintf(request->path, sizeof(request->path),
                    "/api/import/files/upload?path=&name=%s", name) < (int)sizeof(request->path));
    request->length = length;
    current_peer_fd = pair[1];
    *peer_out = pair[1];
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve_upload, request) == 0);
    return thread;
}

static char *finish_upload(int peer, pthread_t thread) {
    char *response = malloc(8192);
    assert(response);
    size_t used = 0;
    for (;;) {
        assert(used < 8191);
        ssize_t n = read(peer, response + used, 8191 - used);
        if (n < 0 && errno == ECONNRESET) break;
        assert(n >= 0);
        if (n == 0) break;
        used += (size_t)n;
    }
    response[used] = '\0';
    close(peer);
    assert(pthread_join(thread, NULL) == 0);
    current_peer_fd = -1;
    return response;
}

static bool exists(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "music/%s", name);
    return access(path, F_OK) == 0;
}

static void send_all(int fd, const void *buffer, size_t size) {
    const char *p = buffer;
    while (size) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        assert(n > 0);
        p += n;
        size -= (size_t)n;
    }
}

static void assert_file_contents(const char *name, const char *expected) {
    char path[128], actual[64] = {0};
    snprintf(path, sizeof(path), "music/%s", name);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, actual, sizeof(actual) - 1);
    assert(n == (ssize_t)strlen(expected));
    assert(close(fd) == 0);
    assert(strcmp(actual, expected) == 0);
}

static char *request_stalled_upload(const char *name, bool fake) {
    atomic_store(&fake_clock_ms, 0);
    atomic_store(&test_clock_mode, fake ? CLOCK_TIMEOUT : CLOCK_REAL);
    request_t request;
    int peer;
    pthread_t thread = start_upload(name, 4, &peer, &request);
    char *response = finish_upload(peer, thread);
    assert(strstr(response, "400 Bad Request"));
    assert(strstr(response, "upload_failed"));
    assert(!exists(name));
    return response;
}

static void test_fake_timeout_and_retry(void) {
    char *response = request_stalled_upload("retry.bin", true);
    free(response);
    long long expected_ms = (long long)EXPECT_TIMEOUT_SECONDS * 1000;
    assert(atomic_load(&fake_clock_ms) == expected_ms);

    atomic_store(&test_clock_mode, CLOCK_REAL);
    request_t request;
    int peer;
    pthread_t thread = start_upload("retry.bin", 5, &peer, &request);
    assert(send(peer, "retry", 5, MSG_NOSIGNAL) == 5);
    response = finish_upload(peer, thread);
    assert(strstr(response, "201 Created"));
    free(response);
    assert_file_contents("retry.bin", "retry");
}

#if EXPECT_TIMEOUT_SECONDS == 12 && !defined(EXPECT_STALE_ACTIVITY)
static void test_exact_idle_boundary(void) {
    atomic_store(&fake_clock_ms, 999);
    atomic_store(&progress_bytes, 0);
    atomic_store(&test_clock_mode, CLOCK_BOUNDARY);
    request_t request;
    int peer;
    pthread_t thread = start_upload("boundary.bin", 4, &peer, &request);
    char *response = finish_upload(peer, thread);
    assert(strstr(response, "upload_failed"));
    free(response);
    assert(atomic_load(&fake_clock_ms) == 12999);
    assert(!exists("boundary.bin"));
    atomic_store(&test_clock_mode, CLOCK_REAL);
}
#endif

static void test_slow_progress_resets_idle(void) {
    atomic_store(&fake_clock_ms, 0);
    atomic_store(&progress_bytes, 0);
    atomic_store(&test_clock_mode, CLOCK_PROGRESS);
    request_t request;
    int peer;
    pthread_t thread = start_upload("slow.bin", 3, &peer, &request);
    char *response = finish_upload(peer, thread);
    assert(strstr(response, "201 Created"));
    free(response);
    assert(atomic_load(&fake_clock_ms) == 21000);
    assert_file_contents("slow.bin", "xyz");
    atomic_store(&test_clock_mode, CLOCK_REAL);
}

static void test_slow_local_io_resets_idle(void) {
    const size_t size = 64u * 1024u + 1u;
    char *body = malloc(size);
    assert(body);
    memset(body, 'w', size);
    atomic_store(&fake_clock_ms, 0);
    atomic_store(&delay_next_write, true);
    atomic_store(&test_clock_mode, CLOCK_WRITE_DELAY);
    request_t request;
    int peer;
    pthread_t thread = start_upload("slow-write.bin", (long long)size, &peer, &request);
    send_all(peer, body, size);
    char *response = finish_upload(peer, thread);
#ifdef EXPECT_STALE_ACTIVITY
    assert(!*response || strstr(response, "upload_failed"));
#else
    assert(strstr(response, "201 Created"));
#endif
    free(response);
    free(body);
    assert(atomic_load(&fake_clock_ms) >= 13000);
#ifdef EXPECT_STALE_ACTIVITY
    assert(!exists("slow-write.bin"));
#else
    assert(exists("slow-write.bin"));
#endif

    const size_t sync_size = 4u * 1024u * 1024u + 1u;
    body = malloc(sync_size);
    assert(body);
    memset(body, 's', sync_size);
    atomic_store(&fake_clock_ms, 0);
    atomic_store(&delay_next_sync, true);
    atomic_store(&test_clock_mode, CLOCK_SYNC_DELAY);
    thread = start_upload("slow-sync.bin", (long long)sync_size, &peer, &request);
    send_all(peer, body, sync_size);
    response = finish_upload(peer, thread);
#ifdef EXPECT_STALE_ACTIVITY
    assert(!*response || strstr(response, "upload_failed"));
#else
    assert(strstr(response, "201 Created"));
#endif
    free(response);
    free(body);
    assert(atomic_load(&fake_clock_ms) >= 13000);
#ifdef EXPECT_STALE_ACTIVITY
    assert(!exists("slow-sync.bin"));
    puts("stale receive-only activity timeout reproduced for write and sync stalls");
#else
    assert(exists("slow-sync.bin"));
#endif
    atomic_store(&test_clock_mode, CLOCK_REAL);
}

static void test_eof_and_enospc_cleanup(void) {
    atomic_store(&test_clock_mode, CLOCK_REAL);
    request_t request;
    int peer;
    pthread_t thread = start_upload("eof.bin", 5, &peer, &request);
    assert(shutdown(peer, SHUT_WR) == 0);
    char *response = finish_upload(peer, thread);
    assert(strstr(response, "upload_failed"));
    free(response);
    assert(!exists("eof.bin"));

    thread = start_upload("full.bin", 4, &peer, &request);
    assert(send(peer, "full", 4, MSG_NOSIGNAL) == 4);
    atomic_store(&fail_next_write, true);
    response = finish_upload(peer, thread);
    assert(strstr(response, "upload_failed"));
    free(response);
    assert(!exists("full.bin"));

    thread = start_upload("full.bin", 4, &peer, &request);
    assert(send(peer, "free", 4, MSG_NOSIGNAL) == 4);
    response = finish_upload(peer, thread);
    assert(strstr(response, "201 Created"));
    free(response);
    assert_file_contents("full.bin", "free");
}

static void *stop_import(void *unused) {
    (void)unused;
    remote_import_files_stop();
    return NULL;
}

static void test_stop_cleans_partial(void) {
    atomic_store(&poll_entered, false);
    atomic_store(&release_poll, false);
    atomic_store(&test_clock_mode, CLOCK_HOLD);
    request_t request;
    int peer;
    pthread_t upload = start_upload("stopped.bin", 4, &peer, &request);
    while (!atomic_load(&poll_entered)) sched_yield();
    assert(exists("stopped.bin"));

    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_import, NULL) == 0);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    nanosleep(&pause, NULL);
    atomic_store(&release_poll, true);
    char *response = finish_upload(peer, upload);
    free(response);
    assert(pthread_join(stopper, NULL) == 0);
    assert(!exists("stopped.bin"));
}

#ifndef SKIP_WALLCLOCK
static void test_wall_clock_idle_deadline(void) {
    struct timespec start, end;
    assert(__real_clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    char *response = request_stalled_upload("wall-timeout.bin", false);
    assert(__real_clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    double elapsed = (double)(end.tv_sec - start.tv_sec) +
                     (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    assert(elapsed >= 11.0 && elapsed < 19.0);
    printf("wall-clock stalled upload timeout: %.2f seconds\n", elapsed);
    free(response);
}
#endif

int main(void) {
    assert(mkdir("music", 0700) == 0);
    assert(remote_import_files_start());
    test_fake_timeout_and_retry();
#if EXPECT_TIMEOUT_SECONDS == 12 && !defined(EXPECT_STALE_ACTIVITY)
    test_exact_idle_boundary();
#endif
    test_slow_progress_resets_idle();
    test_slow_local_io_resets_idle();
    test_eof_and_enospc_cleanup();
    test_stop_cleans_partial();
#ifndef SKIP_WALLCLOCK
    assert(remote_import_files_start());
    test_wall_clock_idle_deadline();
#endif
    remote_import_files_stop();
    printf("remote import upload timeout (%d-second expected idle): PASS\n", EXPECT_TIMEOUT_SECONDS);
    return 0;
}
