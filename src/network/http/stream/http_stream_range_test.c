#define _GNU_SOURCE
#include "http_stream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum {
    GOOD,
    IGNORE_RANGE,
    BAD_RANGE,
    MALFORMED_RANGE,
    SEEK_416,
    BAD_LENGTH,
    LIVE
} test_mode_t;
typedef struct {
    int fd;
    test_mode_t mode;
    int requests;
} server_t;
#define TEST_ENTITY_SIZE 300000ULL

static void send_all(int fd, const char * p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void * server_main(void * arg) {
    server_t * s = arg;
    for (;;) {
        int c = accept(s->fd, NULL, NULL);
        if (c < 0)
            break;
        char req[4096] = {0};
        size_t used = 0;
        while (used < sizeof(req) - 1 && !strstr(req, "\r\n\r\n")) {
            ssize_t n = recv(c, req + used, sizeof(req) - used - 1, 0);
            if (n <= 0)
                break;
            used += (size_t)n;
            req[used] = 0;
        }
        bool ranged = strstr(req, "Range: bytes=") != NULL;
        int which = s->requests++;
        if (s->mode == SEEK_416 && which > 0)
            assert(strstr(req, "If-Range: \"v1\"\r\n"));
        if (s->mode == LIVE) {
            const char * h =
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: "
                "audio/mpeg\r\n\r\n4\r\ntest\r\n";
            send_all(c, h, strlen(h));
            char b;
            (void)recv(c, &b, 1, 0);
            close(c);
            break;
        } else if (s->mode == IGNORE_RANGE ||
                   ((s->mode == BAD_RANGE || s->mode == MALFORMED_RANGE) &&
                    !ranged) ||
                   (s->mode == SEEK_416 && !ranged)) {
            const char * h = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nContent-Type: "
                             "audio/mpeg\r\n\r\n0123456789";
            send_all(c, h, strlen(h));
            close(c);
        } else if (s->mode == BAD_LENGTH && ranged) {
            const char * h =
                "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                "0-9/10\r\nContent-Length: 18446744073709551616\r\n\r\n0123456789";
            send_all(c, h, strlen(h));
            close(c);
        } else if (s->mode == BAD_LENGTH) {
            const char * h =
                "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n";
            send_all(c, h, strlen(h));
            close(c);
        } else if (s->mode == BAD_RANGE && ranged) {
            const char * h = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                             "1-9/10\r\nContent-Length: 9\r\n\r\n123456789";
            send_all(c, h, strlen(h));
            close(c);
        } else if (s->mode == MALFORMED_RANGE && ranged) {
            const char * h = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                             "nope/10\r\nContent-Length: 10\r\n\r\n0123456789";
            send_all(c, h, strlen(h));
            close(c);
        } else if (s->mode == SEEK_416 && ranged && which > 0) {
            const char * h = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: "
                             "bytes */10\r\nContent-Length: 0\r\n\r\n";
            send_all(c, h, strlen(h));
            close(c);
        } else if (ranged) {
            unsigned long long start = 0;
            const char * p = strstr(req, "Range: bytes=");
            if (p)
                sscanf(p, "Range: bytes=%llu-", &start);
            char h[256];
            int n =
                snprintf(h, sizeof(h),
                         "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                         "%llu-%llu/%llu\r\nContent-Length: %llu\r\nContent-Type: "
                         "audio/mpeg\r\nETag: \"v1\"\r\n\r\n",
                         start, TEST_ENTITY_SIZE - 1, TEST_ENTITY_SIZE,
                         TEST_ENTITY_SIZE - start);
            send_all(c, h, (size_t)n);
            char * body = malloc((size_t)(TEST_ENTITY_SIZE - start));
            assert(body);
            for (size_t i = 0; i < TEST_ENTITY_SIZE - start; i++)
                body[i] = (char)('0' + (start + i) % 10);
            send_all(c, body, (size_t)(TEST_ENTITY_SIZE - start));
            free(body);
            close(c);
        } else {
            close(c);
        }
        int max_requests =
            s->mode == GOOD ? 2
                            : (s->mode == BAD_RANGE || s->mode == MALFORMED_RANGE ||
                                       s->mode == SEEK_416 || s->mode == BAD_LENGTH
                                   ? 2
                                   : 1);
        if (s->requests >= max_requests)
            break;
        if ((s->mode == IGNORE_RANGE || s->mode == BAD_LENGTH) && which > 0)
            break;
        if ((s->mode == BAD_RANGE || s->mode == MALFORMED_RANGE) && which > 1)
            break;
        if (s->mode == SEEK_416 && which > 1)
            break;
    }
    close(s->fd);
    return NULL;
}

static http_stream_t * open_test(server_t * s, pthread_t * thread, char url[80]) {
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(s->fd >= 0);
    int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    assert(bind(s->fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    assert(listen(s->fd, 4) == 0);
    socklen_t z = sizeof(a);
    getsockname(s->fd, (struct sockaddr *)&a, &z);
    snprintf(url, 80, "http://127.0.0.1:%u/test", ntohs(a.sin_port));
    assert(pthread_create(thread, NULL, server_main, s) == 0);
    return http_stream_open(url, false);
}

static void test_good(void) {
    server_t s = {.mode = GOOD};
    pthread_t t;
    char url[80];
    http_stream_t * x = open_test(&s, &t, url);
    assert(x);
    assert(http_stream_can_seek(x));
    assert(http_stream_length(x) == (int64_t)TEST_ENTITY_SIZE);
    char b[16] = {0};
    assert(http_stream_read(x, b, 3) == 3 && !memcmp(b, "012", 3));
    assert(!http_stream_seek(x, INT64_MAX, SEEK_SET));
    assert(http_stream_seek(x, 280000, SEEK_SET));
    assert(http_stream_tell(x) == 280000);
    assert(http_stream_read(x, b, 5) == 5 && !memcmp(b, "01234", 5));
    assert(http_stream_seek(x, -2, SEEK_END));
    assert(http_stream_tell(x) == (int64_t)TEST_ENTITY_SIZE - 2);
    assert(http_stream_read(x, b, 10) == 2 && !memcmp(b, "89", 2));
    assert(http_stream_read(x, b, 1) == 0);
    http_stream_close(x);
    pthread_join(t, NULL);
}
static void test_ignored_and_bad_range(void) {
    for (test_mode_t m = IGNORE_RANGE; m <= MALFORMED_RANGE;
         m = (test_mode_t)(m + 1)) {
        server_t s = {.mode = m};
        pthread_t t;
        char url[80];
        http_stream_t * x = open_test(&s, &t, url);
        assert(x);
        assert(!http_stream_can_seek(x));
        char b[16] = {0};
        assert(http_stream_read(x, b, 10) == 10 && !memcmp(b, "0123456789", 10));
        http_stream_close(x);
        pthread_join(t, NULL);
    }
}
static void test_failed_seek_keeps_old(void) {
    server_t s = {.mode = SEEK_416};
    pthread_t t;
    char url[80];
    http_stream_t * x = open_test(&s, &t, url);
    assert(x && http_stream_can_seek(x));
    char b[16] = {0};
    assert(http_stream_read(x, b, 1) == 1 && b[0] == '0');
    /* Past the buffered window, so the replacement request (a 416) is made. */
    assert(!http_stream_seek(x, 280000, SEEK_SET));
    assert(http_stream_tell(x) == 1);
    memset(b, 0, sizeof(b));
    assert(http_stream_read(x, b, 5) == 5 && !memcmp(b, "12345", 5));
    http_stream_close(x);
    pthread_join(t, NULL);
}
/* A probe rewind and a SEEK_END/tell/seek-back size check reuse the open
 * connection: one request in total, and the bytes stay correct. */
static void test_rewind_and_end_reuse_connection(void) {
    server_t s = {.mode = GOOD};
    pthread_t t;
    char url[80];
    http_stream_t * x = open_test(&s, &t, url);
    assert(x && http_stream_can_seek(x));
    char b[16] = {0};
    assert(http_stream_read(x, b, 12) == 12 && !memcmp(b, "012345678901", 12));
    assert(http_stream_seek(x, 0, SEEK_SET));
    assert(http_stream_tell(x) == 0);
    memset(b, 0, sizeof(b));
    assert(http_stream_read(x, b, 4) == 4 && !memcmp(b, "0123", 4));
    assert(http_stream_seek(x, 0, SEEK_END));
    assert(http_stream_tell(x) == (int64_t)TEST_ENTITY_SIZE);
    assert(http_stream_read(x, b, 1) == 0);
    assert(http_stream_seek(x, 7, SEEK_SET));
    memset(b, 0, sizeof(b));
    assert(http_stream_read(x, b, 3) == 3 && !memcmp(b, "789", 3));
    assert(s.requests == 1);
    http_stream_close(x);
    /* The server waits for a second request it will never get. */
    shutdown(s.fd, SHUT_RDWR);
    pthread_join(t, NULL);
}
static void test_pump_creation_failure_keeps_old(void) {
    server_t s = {.mode = GOOD};
    pthread_t t;
    char url[80];
    http_stream_t * x = open_test(&s, &t, url);
    assert(x && http_stream_can_seek(x));
    char b[8] = {0};
    assert(http_stream_read(x, b, 1) == 1 && b[0] == '0');
    http_stream_test_fail_next_pump_create();
    assert(!http_stream_seek(x, 280000, SEEK_SET));
    assert(http_stream_tell(x) == 1);
    memset(b, 0, sizeof(b));
    assert(http_stream_read(x, b, 5) == 5 && !memcmp(b, "12345", 5));
    http_stream_close(x);
    pthread_join(t, NULL);
}
static void test_overflow_and_live_close(void) {
    server_t s = {.mode = BAD_LENGTH};
    pthread_t t;
    char url[80];
    http_stream_t * x = open_test(&s, &t, url);
    assert(!x);
    pthread_join(t, NULL);
    server_t live = {.mode = LIVE};
    x = open_test(&live, &t, url);
    assert(x && !http_stream_can_seek(x));
    char b[8] = {0};
    assert(http_stream_read(x, b, 4) == 4 && !memcmp(b, "test", 4));
    http_stream_close(x);
    pthread_join(t, NULL);
}
static void test_tls_range(unsigned port) {
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/audio", port);
    http_stream_t * stream = http_stream_open(url, false);
    assert(stream && http_stream_can_seek(stream));
    char b[8] = {0};
    assert(http_stream_read(stream, b, 4) == 4 && !memcmp(b, "0123", 4));
    /* Both targets lie outside the buffered and rewind windows, so each one
   * opens and transfers a replacement TLS connection. */
    assert(http_stream_seek(stream, 290000, SEEK_SET));
    memset(b, 0, sizeof(b));
    assert(http_stream_read(stream, b, 4) == 4 && !memcmp(b, "0123", 4));
    assert(http_stream_seek(stream, 0, SEEK_SET));
    memset(b, 0, sizeof(b));
    assert(http_stream_read(stream, b, 4) == 4 && !memcmp(b, "0123", 4));
    http_stream_close(stream);
}
int main(int argc, char ** argv) {
    signal(SIGPIPE, SIG_IGN);
    test_good();
    test_ignored_and_bad_range();
    test_failed_seek_keeps_old();
    test_rewind_and_end_reuse_connection();
    test_pump_creation_failure_keeps_old();
    test_overflow_and_live_close();
    if (argc > 1)
        test_tls_range((unsigned)strtoul(argv[1], NULL, 10));
    puts("http_stream range tests passed");
    return 0;
}
