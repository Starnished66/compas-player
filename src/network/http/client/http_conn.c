/* pthread_timedjoin_np (net_connect_timeout's bounded DNS resolution below)
 * is a musl/glibc extension gated behind _GNU_SOURCE -- must be defined
 * before any system header pulls in pthread.h transitively, so this has to
 * be the file's very first line. */
#ifndef _GNU_SOURCE /* the host build also passes -D_GNU_SOURCE=1 */
#define _GNU_SOURCE
#endif

#include "http_conn.h"
#include "http_client.h" /* for struct http_cancel_token's real definition */
#include "../../security/ca_bundle.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>

static uint64_t monotonic_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static bool cancel_requested_conn(struct http_cancel_token * cancel) {
    if (!cancel) return false;
    pthread_mutex_lock(&cancel->mutex);
    bool requested = cancel->cancel_requested;
    pthread_mutex_unlock(&cancel->mutex);
    return requested;
}

/* Registers fd into cancel (compare-and-set: only succeeds if the token
 * hasn't already been cancelled). Returns false if cancellation raced us
 * -- caller should abandon fd immediately in that case. */
static bool cancel_register(struct http_cancel_token * cancel, int fd) {
    if (!cancel) return true;
    pthread_mutex_lock(&cancel->mutex);
    bool already = cancel->cancel_requested;
    if (!already) cancel->fd = fd;
    pthread_mutex_unlock(&cancel->mutex);
    return !already;
}

/* Unregisters fd from cancel token and closes fd atomically under cancel mutex. */
static void cancel_unregister_and_close(struct http_cancel_token * cancel, int fd) {
    if (!cancel) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&cancel->mutex);
    if (cancel->fd == fd) cancel->fd = -1;
    close(fd);
    pthread_mutex_unlock(&cancel->mutex);
}

/* getaddrinfo() has no timeout parameter of its own and is not interruptible
 * mid-call -- on a Wi-Fi network where DNS packets are silently dropped
 * (association succeeds, so the radio-level guard callers already run
 * passes, but the resolver never gets a reply) it can block far longer than
 * connect_timeout_ms, or effectively forever, with no way for a caller's
 * cancel token to take effect (real-device report: Subsonic's "Connecting
 * to server..." busy overlay never dismissing). Run it on its own thread and
 * bound the WAIT with pthread_timedjoin_np instead of bounding the call
 * itself, which can't be done for a blocking libc function.
 *
 * Ownership handoff on timeout: the resolver thread is still running when
 * the caller gives up, so the caller must not free/touch ctx or ctx->result
 * -- the thread itself takes over that responsibility (checked via
 * ctx->abandoned under ctx->mutex) once getaddrinfo() finally returns,
 * detaching itself so the OS reclaims its resources without anyone having
 * to join it. This deliberately leaks a small ctx struct and one thread
 * stack until that eventually-returning call completes -- an accepted
 * tradeoff for turning an unbounded hang into a bounded one. */
typedef struct {
    char host[256];
    char port[16];
    pthread_mutex_t mutex;
    bool abandoned; /* caller gave up waiting; thread must clean up on exit */
    bool done;      /* thread finished; only meaningful while NOT abandoned */
    int rc;
    struct addrinfo * result;
} dns_resolve_ctx_t;

static void * dns_resolve_thread_func(void * arg) {
    dns_resolve_ctx_t * ctx = (dns_resolve_ctx_t *) arg;

    struct addrinfo hints, * addr_list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int rc = getaddrinfo(ctx->host, ctx->port, &hints, &addr_list);

    pthread_mutex_lock(&ctx->mutex);
    if (ctx->abandoned) {
        pthread_mutex_unlock(&ctx->mutex);
        if (rc == 0) freeaddrinfo(addr_list);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        pthread_detach(pthread_self());
        return NULL;
    }
    ctx->rc = rc;
    ctx->result = (rc == 0) ? addr_list : NULL;
    ctx->done = true;
    pthread_mutex_unlock(&ctx->mutex);
    return NULL;
}

/* Bounded getaddrinfo(): returns 0 and *out_result on success, matching
 * getaddrinfo()'s own return convention, or a nonzero sentinel
 * (DNS_RESOLVE_TIMED_OUT) distinct from any real getaddrinfo() error code
 * when the deadline passes before the resolver thread finishes.
 * timeout_ms == 0 means "wait indefinitely" (same convention connect_
 * timeout_ms already uses), matching plain getaddrinfo()'s own blocking
 * behavior for that case. */
#define DNS_RESOLVE_TIMED_OUT (-1000)
static int dns_resolve_bounded(const char * host, const char * port, uint32_t timeout_ms,
                               struct addrinfo ** out_result, bool require_bounded) {
    dns_resolve_ctx_t * ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return EAI_MEMORY;
    snprintf(ctx->host, sizeof(ctx->host), "%s", host);
    snprintf(ctx->port, sizeof(ctx->port), "%s", port);
    pthread_mutex_init(&ctx->mutex, NULL);

    pthread_t thread;
    if (pthread_create(&thread, NULL, dns_resolve_thread_func, ctx) != 0) {
        /* Thread creation itself failing is rare and not the hang this
         * exists to guard against -- fall back to a direct blocking call
         * rather than failing the request outright. */
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        if (require_bounded) return EAI_AGAIN;
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        return getaddrinfo(host, port, &hints, out_result);
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t deadline_ns = (uint64_t) deadline.tv_nsec + (uint64_t) timeout_ms * 1000000ULL;
    deadline.tv_sec += (time_t) (deadline_ns / 1000000000ULL);
    deadline.tv_nsec = (long) (deadline_ns % 1000000000ULL);

    int join_rc = timeout_ms > 0 ? pthread_timedjoin_np(thread, NULL, &deadline)
                                  : pthread_join(thread, NULL);
    if (join_rc == 0) {
        int rc = ctx->rc;
        *out_result = ctx->result;
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return rc;
    }

    /* Timed out (or some other join failure) -- the thread may still be
     * blocked inside getaddrinfo(). Hand ctx off to it via the abandoned
     * flag rather than freeing something it might still write into. */
    pthread_mutex_lock(&ctx->mutex);
    ctx->abandoned = true;
    bool already_done = ctx->done;
    struct addrinfo * leftover = already_done ? ctx->result : NULL;
    int leftover_rc = already_done ? ctx->rc : 0;
    pthread_mutex_unlock(&ctx->mutex);
    if (already_done) {
        /* Vanishingly rare race: it finished between the join timing out
         * and this lock -- reclaim ctx ourselves instead of leaving the
         * thread to do it, and use the real result. */
        pthread_join(thread, NULL);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        if (leftover_rc == 0) { *out_result = leftover; return 0; }
        return leftover_rc;
    }
    return DNS_RESOLVE_TIMED_OUT;
}

static uint32_t conn_remaining_ms(uint32_t requested, uint64_t deadline) {
    if (!deadline) return requested;
    uint64_t now = monotonic_now_ms();
    if (now >= deadline) return 1;
    uint64_t remaining = deadline - now;
    if (requested && remaining > requested) return requested;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static bool conn_apply_deadline(http_conn_t *conn);

/* Connects to host:port with a timeout using non-blocking connect() and poll().
 * Registers socket into cancel token to allow cancellation during connect. */
static http_conn_error_t net_connect_timeout(mbedtls_net_context * ctx, const char * host, const char * port,
                                              uint32_t connect_timeout_ms, struct http_cancel_token * cancel, uint64_t deadline) {
    if (cancel_requested_conn(cancel)) return HTTP_CONN_ERR_CANCELLED;

    struct addrinfo * addr_list, * cur;
    int dns_rc = dns_resolve_bounded(host, port, conn_remaining_ms(connect_timeout_ms, deadline), &addr_list, deadline != 0);
    if (dns_rc == DNS_RESOLVE_TIMED_OUT) return HTTP_CONN_ERR_DNS_TIMEOUT;
    if (dns_rc != 0) return HTTP_CONN_ERR_DNS;

    if (cancel_requested_conn(cancel)) { freeaddrinfo(addr_list); return HTTP_CONN_ERR_CANCELLED; }

    http_conn_error_t result = HTTP_CONN_ERR_CONNECT;
    for (cur = addr_list; cur != NULL; cur = cur->ai_next) {
        if (deadline && monotonic_now_ms() >= deadline) { result = HTTP_CONN_ERR_CONNECT_TIMEOUT; break; }
        int fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (fd < 0) { result = HTTP_CONN_ERR_SOCKET; continue; }

        if (!cancel_register(cancel, fd)) {
            close(fd);
            result = HTTP_CONN_ERR_CANCELLED;
            break;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        bool connected = connect(fd, cur->ai_addr, cur->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            /* poll()'s own -1 means "wait indefinitely" -- exactly
             * connect_timeout_ms == 0's intended meaning, and shutdown()
             * (from a cancel) still unblocks it immediately either way. */
            uint32_t wait_ms = conn_remaining_ms(connect_timeout_ms, deadline);
            int pr = poll(&pfd, 1, wait_ms > 0 ? (int)(wait_ms > INT32_MAX ? INT32_MAX : wait_ms) : -1);
            if (pr == 0) {
                cancel_unregister_and_close(cancel, fd);
                result = HTTP_CONN_ERR_CONNECT_TIMEOUT;
                continue;
            }
            if (pr < 0) {
                bool cancelled = cancel_requested_conn(cancel);
                cancel_unregister_and_close(cancel, fd);
                result = cancelled ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_CONNECT;
                continue;
            }
            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0;
        }
        if (!connected) {
            bool cancelled = cancel_requested_conn(cancel);
            cancel_unregister_and_close(cancel, fd);
            result = cancelled ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_CONNECT;
            continue;
        }

        fcntl(fd, F_SETFL, flags); /* back to blocking for the rest of this connection's life */
        ctx->fd = fd;
        result = HTTP_CONN_OK;
        break;
    }

    freeaddrinfo(addr_list);
    return result;
}

/* TLS may perform several BIO calls within one handshake/read/write. */
static int conn_tls_send(void *context, const unsigned char *data, size_t size) {
    http_conn_t *conn = context;
    if (!conn_apply_deadline(conn)) return MBEDTLS_ERR_SSL_TIMEOUT;
    return mbedtls_net_send(&conn->net, data, size);
}

static int conn_tls_recv(void *context, unsigned char *data, size_t size) {
    http_conn_t *conn = context;
    if (!conn_apply_deadline(conn)) return MBEDTLS_ERR_SSL_TIMEOUT;
    return mbedtls_net_recv(&conn->net, data, size);
}

static bool conn_tls_configure(http_conn_t *conn, bool verify_tls) {
    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->conf);
    mbedtls_x509_crt_init(&conn->cacert);
    mbedtls_ctr_drbg_init(&conn->ctr_drbg);
    mbedtls_entropy_init(&conn->entropy);
    conn->ssl_initialized = true;
    if (mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func, &conn->entropy, NULL, 0) != 0)
        return false;
    if (mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
    mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &conn->ctr_drbg);
    if (verify_tls) {
        if (mbedtls_x509_crt_parse(&conn->cacert, ca_bundle_pem, ca_bundle_pem_len) < 0) return false;
        mbedtls_ssl_conf_ca_chain(&conn->conf, &conn->cacert, NULL);
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* Explicit per-connection trust of a self-signed server. */
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    return true;
}

static bool conn_tls_handshake(http_conn_t *conn, bool verify_tls, http_conn_error_t *error) {
    uint64_t deadline = conn->read_timeout_ms ? monotonic_now_ms() + conn->read_timeout_ms : 0;
    if (conn->deadline_ms && (!deadline || conn->deadline_ms < deadline)) deadline = conn->deadline_ms;
    for (;;) {
        if (!conn_apply_deadline(conn)) { *error = HTTP_CONN_ERR_TLS_HANDSHAKE_TIMEOUT; return false; }
        errno = 0;
        int ret = mbedtls_ssl_handshake(&conn->ssl);
        if (ret == 0) break;
        bool would_block = ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                           errno == EAGAIN || errno == EWOULDBLOCK;
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT || (deadline && monotonic_now_ms() >= deadline)) {
            *error = HTTP_CONN_ERR_TLS_HANDSHAKE_TIMEOUT;
            return false;
        }
        if (!would_block) {
            *error = cancel_requested_conn(conn->cancel_token) ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_TLS_HANDSHAKE;
            return false;
        }
    }
    if (verify_tls && mbedtls_ssl_get_verify_result(&conn->ssl) != 0) {
        *error = HTTP_CONN_ERR_TLS_VERIFY;
        return false;
    }
    return true;
}

static bool conn_tls_open(http_conn_t *conn, const char *host, bool verify_tls, http_conn_error_t *error) {
    if (!conn_tls_configure(conn, verify_tls) || mbedtls_ssl_setup(&conn->ssl, &conn->conf) != 0 ||
        mbedtls_ssl_set_hostname(&conn->ssl, host) != 0) {
        *error = HTTP_CONN_ERR_TLS_SETUP;
        return false;
    }
    mbedtls_ssl_set_bio(&conn->ssl, conn, conn_tls_send, conn_tls_recv, NULL);
    return conn_tls_handshake(conn, verify_tls, error);
}

bool http_conn_open_deadline_ex(http_conn_t *conn, const char *host, const char *port, bool https, bool verify_tls,
    uint32_t connect_timeout_ms, uint32_t read_timeout_ms, struct http_cancel_token *cancel,
    http_conn_error_t *out_error, uint64_t deadline_ms) {
    http_conn_error_t local_error = HTTP_CONN_OK;
    if (!out_error) out_error = &local_error;
    *out_error = HTTP_CONN_OK;
    memset(conn, 0, sizeof(*conn));
    conn->is_https = https;
    conn->read_timeout_ms = read_timeout_ms;
    conn->deadline_ms = deadline_ms;
    conn->cancel_token = cancel;
    mbedtls_net_init(&conn->net);
    *out_error = net_connect_timeout(&conn->net, host, port, connect_timeout_ms, cancel, deadline_ms);
    if (*out_error != HTTP_CONN_OK) return false;
    if (read_timeout_ms > 0) {
        struct timeval tv = { .tv_sec = (time_t)(read_timeout_ms / 1000),
                              .tv_usec = (suseconds_t)(read_timeout_ms % 1000) * 1000 };
        setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (!conn_apply_deadline(conn)) { *out_error = HTTP_CONN_ERR_CONNECT_TIMEOUT; return false; }
    return !https || conn_tls_open(conn, host, verify_tls, out_error);
}

bool http_conn_open_ex(http_conn_t *conn, const char *host, const char *port, bool https, bool verify_tls,
    uint32_t connect_ms, uint32_t read_ms, struct http_cancel_token *cancel, http_conn_error_t *error) {
    return http_conn_open_deadline_ex(conn, host, port, https, verify_tls, connect_ms, read_ms, cancel, error, 0);
}

bool http_conn_open(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls) {
    return http_conn_open_ex(conn, host, port, https, verify_tls, 0, 0, NULL, NULL);
}

/* Re-clamp before every socket operation, including buffered reader refills. */
static bool conn_apply_deadline(http_conn_t *conn) {
    if (!conn->deadline_ms) return true;
    uint64_t now = monotonic_now_ms();
    if (now >= conn->deadline_ms) {
        conn->last_read_timed_out = true;
        errno = ETIMEDOUT;
        return false;
    }
    uint64_t ms = conn->deadline_ms - now;
    if (conn->read_timeout_ms && ms > conn->read_timeout_ms) ms = conn->read_timeout_ms;
    struct timeval tv = { .tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)(ms % 1000) * 1000 };
    setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

int http_conn_write(http_conn_t * conn, const uint8_t * data, size_t len) {
    if (!conn_apply_deadline(conn)) return -1;
    if (conn->is_https) return mbedtls_ssl_write(&conn->ssl, data, len);
    return mbedtls_net_send(&conn->net, data, len);
}

int http_conn_read(http_conn_t * conn, uint8_t * buf, size_t len) {
    if (!conn_apply_deadline(conn)) return -1;
    errno = 0;
    int n = conn->is_https ? mbedtls_ssl_read(&conn->ssl, buf, len) : mbedtls_net_recv(&conn->net, buf, len);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        conn->last_read_timed_out = false;
        return 0;
    }
    /* Check errno directly for timeout (EAGAIN/EWOULDBLOCK) in addition to
     * mbedTLS want-read/want-write status codes. */
    conn->last_read_timed_out = (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT) ||
                                 (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    return n;
}

void http_conn_close(http_conn_t * conn) {
    if (conn->is_https) {
        if (conn->ssl_initialized) {
            if (conn_apply_deadline(conn)) mbedtls_ssl_close_notify(&conn->ssl);
            mbedtls_ssl_free(&conn->ssl);
            mbedtls_ssl_config_free(&conn->conf);
            mbedtls_x509_crt_free(&conn->cacert);
            mbedtls_ctr_drbg_free(&conn->ctr_drbg);
            mbedtls_entropy_free(&conn->entropy);
        }
    }
    /* Unregister from cancel token and free network context under cancel mutex. */
    struct http_cancel_token * cancel = conn->cancel_token;
    if (cancel) {
        pthread_mutex_lock(&cancel->mutex);
        if (cancel->fd == conn->net.fd) cancel->fd = -1;
        mbedtls_net_free(&conn->net);
        pthread_mutex_unlock(&cancel->mutex);
    } else {
        mbedtls_net_free(&conn->net);
    }
}

static int reader_fill(http_conn_reader_t * r) {
    r->pos = 0;
    int n = http_conn_read(r->conn, r->buf, sizeof(r->buf));
    r->len = (n > 0) ? (size_t) n : 0;
    return n;
}

static bool http_conn_reader_line_ex(http_conn_reader_t * r, char * out, size_t out_size, bool * truncated) {
    if (truncated) *truncated = false;
    size_t out_pos = 0;
    for (;;) {
        if (r->pos >= r->len) {
            int n = reader_fill(r);
            if (n <= 0) return false;
        }
        uint8_t c = r->buf[r->pos++];
        if (c == '\n') {
            if (out_pos > 0 && out[out_pos - 1] == '\r') out_pos--;
            out[out_pos] = '\0';
            return true;
        }
        if (out_pos + 1 < out_size) out[out_pos++] = (char) c;
        else if (truncated) *truncated = true;
    }
}

bool http_conn_reader_line(http_conn_reader_t * r, char * out, size_t out_size) {
    return http_conn_reader_line_ex(r, out, out_size, NULL);
}

bool http_conn_reader_line_strict(http_conn_reader_t *r, char *out, size_t out_size) {
    size_t used = 0;
    for (;;) {
        uint8_t c;
        if (!http_conn_reader_read_exact(r, &c, 1)) return false;
        if (c == '\r') {
            if (!http_conn_reader_read_exact(r, &c, 1) || c != '\n') return false;
            out[used] = '\0';
            return true;
        }
        if (c == '\n' || !c || used + 1 >= out_size) return false;
        out[used++] = (char)c;
    }
}

static bool parse_content_length(const char * value, uint64_t * out) {
    if (!*value) return false;
    uint64_t n = 0;
    for (; *value; value++) {
        if (*value < '0' || *value > '9') return false;
        unsigned d = (unsigned)(*value - '0');
        if (n > (UINT64_MAX - d) / 10) return false;
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

static http_head_result_t read_head_line(http_conn_reader_t *r, bool strict,
                                         char *line, size_t size) {
    if (strict) {
        if (http_conn_reader_line_strict(r, line, size)) return HTTP_HEAD_OK;
        return r->conn->last_read_timed_out ? HTTP_HEAD_IO : HTTP_HEAD_MALFORMED;
    }
    bool truncated;
    if (!http_conn_reader_line_ex(r, line, size, &truncated)) return HTTP_HEAD_IO;
    return truncated ? HTTP_HEAD_MALFORMED : HTTP_HEAD_OK;
}

http_head_result_t http_conn_read_head(http_conn_reader_t * r, http_head_t * h,
                                       http_header_fn fn, void * ctx) {
    char line[4096];
    h->status = 0;
    h->chunked = false;
    h->has_length = false;
    h->content_length = 0;
    if (h->location && h->location_size) h->location[0] = '\0';
    http_head_result_t result = read_head_line(r, h->strict, line, sizeof(line));
    if (result != HTTP_HEAD_OK) return result;
    const char * sp = strchr(line, ' ');
    if (!sp) return HTTP_HEAD_MALFORMED;
    if (h->strict && (strncmp(line, "HTTP/1.1 ", 9) != 0 && strncmp(line, "HTTP/1.0 ", 9) != 0))
        return HTTP_HEAD_MALFORMED;
    if (h->strict && (strlen(sp + 1) < 3 || sp[1] < '1' || sp[1] > '5' ||
        sp[2] < '0' || sp[2] > '9' || sp[3] < '0' || sp[3] > '9' || (sp[4] && sp[4] != ' ')))
        return HTTP_HEAD_MALFORMED;
    h->status = atoi(sp + 1);

    for (;;) {
        result = read_head_line(r, h->strict, line, sizeof(line));
        if (result != HTTP_HEAD_OK)
            return !h->strict && h->allow_header_eof && result == HTTP_HEAD_IO ? HTTP_HEAD_OK : result;
        if (!line[0]) return h->strict && h->chunked && h->has_length ? HTTP_HEAD_MALFORMED : HTTP_HEAD_OK;
        char * colon = strchr(line, ':');
        if (!colon) { if (h->strict) return HTTP_HEAD_MALFORMED; continue; }
        *colon = '\0';
        char * value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        size_t len = strlen(value);
        while (len && (value[len - 1] == ' ' || value[len - 1] == '\t'))
            value[--len] = '\0';

        if (strcasecmp(line, "Content-Length") == 0) {
            uint64_t n;
            if (!parse_content_length(value, &n) ||
                (h->has_length && h->content_length != n))
                return HTTP_HEAD_MALFORMED;
            h->has_length = true;
            h->content_length = n;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (h->strict && (h->chunked || strcasecmp(value, "chunked") != 0)) return HTTP_HEAD_MALFORMED;
            if (strcasecmp(value, "chunked") == 0) h->chunked = true;
        } else if (strcasecmp(line, "Location") == 0 && h->location && h->location_size) {
            h->location[0] = '\0';
            if (len < h->location_size) memcpy(h->location, value, len + 1);
        }
        if (fn && !fn(ctx, line, value)) return HTTP_HEAD_MALFORMED;
    }
}

bool http_conn_reader_read_exact(http_conn_reader_t * r, uint8_t * out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (r->pos >= r->len) {
            int rn = reader_fill(r);
            if (rn <= 0) return false;
        }
        size_t avail = r->len - r->pos;
        size_t want = n - got;
        size_t take = avail < want ? avail : want;
        memcpy(out + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return true;
}

int http_conn_reader_read_some(http_conn_reader_t * r, uint8_t * buf, size_t len) {
    if (r->pos < r->len) {
        size_t avail = r->len - r->pos;
        size_t take = avail < len ? avail : len;
        memcpy(buf, r->buf + r->pos, take);
        r->pos += take;
        return (int) take;
    }
    return http_conn_read(r->conn, buf, len);
}
