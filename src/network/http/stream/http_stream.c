#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "http_stream.h"
#include "../../../core/system/logging/debug_log.h"
#include "../client/http_conn.h"
#include "../client/http_client.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define STREAM_RING_CAPACITY (256 * 1024)
#define STREAM_MAX_REDIRECTS 5
#define STREAM_CONNECT_TIMEOUT_MS 10000u
#define STREAM_READ_TIMEOUT_MS 5000u
#define STREAM_OPEN_TIMEOUT_MS 20000u

typedef struct {
    http_conn_t * conn;
    http_conn_reader_t reader;
    bool chunked;
    bool has_length;
    uint64_t content_length;
    bool range_valid;
    uint64_t range_start, range_end, range_total;
    int status;
    uint32_t icy_interval;
    char content_type[128];
    char etag[128], last_modified[64];
    char final_url[2048];
} response_t;

struct http_stream {
    http_conn_t * conn;
    http_conn_reader_t reader;
    bool chunked, has_content_length, seekable;
    uint64_t content_length, remaining;
    uint64_t position, length;
    char content_type[128];
    char etag[128], last_modified[64];
    char final_url[2048];
    bool verify_tls;
    uint32_t icy_interval, icy_audio_left;
    size_t icy_used, icy_left;
    char icy_block[4081];
    icy_metadata_t metadata;
    char source_url[2048];
    struct http_stream * metadata_next;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint8_t * ring;
    size_t ring_head, ring_tail, ring_fill;
    /* Bytes already read that still sit intact just behind ring_tail. A short
     * rewind (a container probe, a parser re-reading a header) reuses them
     * instead of opening a new connection. */
    size_t ring_history;
    /* SEEK_END to the entity length, as parsers do to learn its size. The
     * connection and buffered bytes stay, so seeking back costs nothing. */
    bool at_virtual_end;
    bool stop_requested, ended, thread_started;
    uint64_t playback_generation;
    http_cancel_token_t cancel;
    struct http_stream * cancel_next;
};

static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t * metadata_streams;
static uint64_t next_stream_id;
static pthread_mutex_t cancel_mutex = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t * cancel_streams;
static uint64_t cancelled_through_generation;

static uint64_t stream_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void register_cancel_stream(http_stream_t * s) {
    pthread_mutex_lock(&cancel_mutex);
    s->cancel_next = cancel_streams;
    cancel_streams = s;
    if (s->playback_generation && s->playback_generation <= cancelled_through_generation)
        http_cancel_token_cancel(&s->cancel);
    pthread_mutex_unlock(&cancel_mutex);
}

static void unregister_cancel_stream(http_stream_t * s) {
    pthread_mutex_lock(&cancel_mutex);
    http_stream_t ** link = &cancel_streams;
    while (*link && *link != s) link = &(*link)->cancel_next;
    if (*link) *link = s->cancel_next;
    pthread_mutex_unlock(&cancel_mutex);
}

void http_stream_cancel_generation(uint64_t generation) {
    pthread_mutex_lock(&cancel_mutex);
    if (generation > cancelled_through_generation) cancelled_through_generation = generation;
    for (http_stream_t * s = cancel_streams; s; s = s->cancel_next)
        if (s->playback_generation && s->playback_generation <= generation)
            http_cancel_token_cancel(&s->cancel);
    pthread_mutex_unlock(&cancel_mutex);
}

bool http_stream_metadata_for_url(const char * url, icy_metadata_t * out) {
    if (!url || !out || pthread_mutex_trylock(&metadata_mutex) != 0) return false;
    bool found = false;
    for (http_stream_t * s = metadata_streams; s; s = s->metadata_next) {
        if (strcmp(url, s->source_url)) continue;
        if (pthread_mutex_trylock(&s->mutex) == 0) {
            *out = s->metadata;
            found = out->revision != 0;
            pthread_mutex_unlock(&s->mutex);
        }
        break;
    }
    pthread_mutex_unlock(&metadata_mutex);
    return found;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool released;
    http_stream_t * stream;
} pump_gate_t;

#ifdef HTTP_STREAM_TESTING
static bool fail_next_pump_create;
void http_stream_test_fail_next_pump_create(void) {
    fail_next_pump_create = true;
}
#endif

static bool parse_u64(const char * p, uint64_t * out) {
    if (!p || !*p || *p < '0' || *p > '9')
        return false;
    uint64_t n = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned d = (unsigned)(*p - '0');
        if (n > (UINT64_MAX - d) / 10)
            return false;
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

static bool parse_content_range(const char * v, uint64_t * start, uint64_t * end,
                                uint64_t * total) {
    if (strncasecmp(v, "bytes ", 6) != 0)
        return false;
    const char * p = v + 6;
    const char * dash = strchr(p, '-');
    const char * slash = dash ? strchr(dash + 1, '/') : NULL;
    if (!dash || !slash || strchr(slash + 1, '/'))
        return false;
    char a[32], b[32];
    size_t al = (size_t)(dash - p), bl = (size_t)(slash - dash - 1);
    if (!al || al >= sizeof(a) || !bl || bl >= sizeof(b))
        return false;
    memcpy(a, p, al);
    a[al] = 0;
    memcpy(b, dash + 1, bl);
    b[bl] = 0;
    return parse_u64(a, start) && parse_u64(b, end) &&
           parse_u64(slash + 1, total) && *start <= *end && *end < *total;
}

static bool copy_safe_header_value(char * out, size_t capacity,
                                   const char * value) {
    size_t n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t'))
        --n;
    if (!n || n >= capacity)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] > 0x7e)
            return false;
    memcpy(out, value, n);
    out[n] = '\0';
    return true;
}

static bool write_request(http_conn_t * c, const char * request, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = http_conn_write(c, (const uint8_t *)request + sent, len - sent);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool read_stream_header(void * ctx, const char * name, char * value) {
    response_t * r = ctx;
    if (!strcasecmp(name, "icy-metaint")) {
        char * end;
        errno = 0;
        unsigned long interval = strtoul(value, &end, 10);
        if (errno || end == value || *end || value[0] == '-' || interval > UINT32_MAX) return false;
        r->icy_interval = (uint32_t)interval;
    } else if (!strcasecmp(name, "Content-Range")) {
        if (!parse_content_range(value, &r->range_start, &r->range_end,
                                 &r->range_total) || r->range_valid)
            return false;
        r->range_valid = true;
    } else if (!strcasecmp(name, "Content-Type")) {
        char * semi = strchr(value, ';');
        if (semi)
            *semi = 0;
        snprintf(r->content_type, sizeof(r->content_type), "%s", value);
    } else if (!strcasecmp(name, "ETag")) {
        (void)copy_safe_header_value(r->etag, sizeof(r->etag), value);
    } else if (!strcasecmp(name, "Last-Modified")) {
        (void)copy_safe_header_value(r->last_modified, sizeof(r->last_modified), value);
    }
    return true;
}

static bool send_stream_request(http_conn_t *conn, const char *host, const char *path,
                                 bool range, uint64_t offset, const char *if_range, bool request_icy) {
    char request[2800];
    int n = range ? snprintf(request, sizeof(request),
                             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
                             "Compas/compas_player\r\nIcy-MetaData: %d\r\nRange: "
                             "bytes=%llu-\r\n%s%s%sConnection: close\r\n\r\n",
                             path, host, request_icy ? 1 : 0, (unsigned long long)offset,
                             if_range ? "If-Range: " : "",
                             if_range ? if_range : "", if_range ? "\r\n" : "")
                  : snprintf(request, sizeof(request),
                             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
                             "Compas/compas_player\r\nIcy-MetaData: "
                             "%d\r\nConnection: close\r\n\r\n",
                             path, host, request_icy ? 1 : 0);
    if (n < 0 || (size_t)n >= sizeof(request) ||
        !write_request(conn, request, (size_t)n))
        return false;
    return true;
}

static bool open_response(const char * url, bool verify_tls, int redirects_left,
                          bool range, uint64_t offset, const char * if_range, bool request_icy,
                          http_cancel_token_t * cancel, uint64_t deadline_ms, response_t * r) {
    memset(r, 0, sizeof(*r));
    bool https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &https, host, sizeof(host), port, sizeof(port),
                             path, sizeof(path)))
        return false;
    r->conn = calloc(1, sizeof(*r->conn));
    if (!r->conn)
        return false;
    http_conn_error_t conn_error;
    if (!http_conn_open_deadline_ex(r->conn, host, port, https, verify_tls,
                                    STREAM_CONNECT_TIMEOUT_MS, STREAM_READ_TIMEOUT_MS,
                                    cancel, &conn_error, deadline_ms)) {
        http_conn_close(r->conn);
        free(r->conn);
        r->conn = NULL;
        return false;
    }
    r->reader.conn = r->conn;
    if (!send_stream_request(r->conn, host, path, range, offset, if_range, request_icy)) goto fail;
    char location[2048] = "";
    http_head_t head = { .location = location, .location_size = sizeof(location),
                         .allow_header_eof = true };
    http_head_result_t head_result = http_conn_read_head(&r->reader, &head, read_stream_header, r);
    /* The absolute cap covers connect, request write and the complete header
     * phase only. The cancel token interrupts stalled body reads. */
    http_conn_end_deadline(r->conn, 0);
    if (head_result != HTTP_HEAD_OK)
        goto fail;
    r->status = head.status;
    r->chunked = head.chunked;
    r->has_length = head.has_length;
    r->content_length = head.content_length;
    if (r->status >= 300 && r->status < 400 && location[0]) {
        http_conn_close(r->conn);
        free(r->conn);
        r->conn = NULL;
        if (redirects_left <= 0 || (strncasecmp(location, "http://", 7) &&
                                    strncasecmp(location, "https://", 8)))
            return false;
        /* An entity validator applies only to the original origin and must
     * not be forwarded if a redirect changes the selected resource. */
        return open_response(location, verify_tls, redirects_left - 1, range,
                             offset, NULL, request_icy, cancel, deadline_ms, r);
    }
    if (range) {
        /* A server may ignore the initial capability probe and return the
     * ordinary complete response. Keep it for playback; seek callers
     * independently require a matching 206 before switching readers. */
        if (r->status == 200 && offset == 0) {
            /* accepted as a nonseekable response */
        } else if (r->status != 206 || !r->range_valid ||
                   r->range_start != offset ||
                   (r->has_length &&
                    r->content_length != r->range_end - r->range_start + 1))
            goto fail;
    } else if (r->status < 200 || r->status >= 300)
        goto fail;
    else if (r->status == 206 && (!r->range_valid || r->range_start != 0))
        goto fail;
    snprintf(r->final_url, sizeof(r->final_url), "%s", url);
    return true;
fail:
    http_conn_close(r->conn);
    free(r->conn);
    r->conn = NULL;
    return false;
}

static bool push_ring(http_stream_t * s, const uint8_t * data, size_t len) {
    size_t off = 0;
    while (off < len) {
        pthread_mutex_lock(&s->mutex);
        while (s->ring_fill == STREAM_RING_CAPACITY && !s->stop_requested)
            pthread_cond_wait(&s->cond, &s->mutex);
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->mutex);
            return false;
        }
        size_t space = STREAM_RING_CAPACITY - s->ring_fill,
               take = len - off < space ? len - off : space;
        for (size_t i = 0; i < take; i++) {
            s->ring[s->ring_head] = data[off + i];
            s->ring_head = (s->ring_head + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill += take;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        off += take;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    return true;
}

/* HTTP chunks and socket reads may split any ICY field. Only audio reaches the ring. */
static bool push_stream_body(http_stream_t * s, const uint8_t * data, size_t len) {
    if (!s->icy_interval) return push_ring(s, data, len);
    while (len) {
        if (s->icy_audio_left) {
            size_t n = len < s->icy_audio_left ? len : s->icy_audio_left;
            if (!push_ring(s, data, n)) return false;
            s->icy_audio_left -= (uint32_t)n;
            data += n; len -= n;
        } else if (s->icy_left) {
            size_t n = len < s->icy_left ? len : s->icy_left;
            memcpy(s->icy_block + s->icy_used, data, n);
            s->icy_used += n; s->icy_left -= n;
            data += n; len -= n;
            if (!s->icy_left) {
                s->icy_block[s->icy_used] = '\0';
                pthread_mutex_lock(&s->mutex);
                icy_metadata_parse(s->icy_block, &s->metadata);
                pthread_mutex_unlock(&s->mutex);
                s->icy_audio_left = s->icy_interval;
            }
        } else {
            s->icy_left = (size_t)*data++ * 16;
            --len;
            s->icy_used = 0;
            if (!s->icy_left) s->icy_audio_left = s->icy_interval;
        }
    }
    return true;
}

static void * pump(void * arg) {
    http_stream_t * s = arg;
    uint8_t buf[HTTP_CONN_READ_CHUNK];
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        bool stop = s->stop_requested;
        bool limited = s->has_content_length;
        uint64_t rem = s->remaining;
        pthread_mutex_unlock(&s->mutex);
        if (stop || (limited && rem == 0))
            break;
        if (s->chunked) {
            char line[64];
            if (!http_conn_reader_line(&s->reader, line, sizeof(line)))
                break;
            char * semi = strchr(line, ';');
            if (semi)
                *semi = 0;
            errno = 0;
            char * end = NULL;
            unsigned long long z = strtoull(line, &end, 16);
            if (errno || !end || *end || end == line || z > UINT32_MAX)
                break;
            if (z == 0)
                break;
            uint64_t left = z;
            bool ok = true;
            while (left) {
                size_t take = left < sizeof(buf) ? (size_t)left : sizeof(buf);
                if (!http_conn_reader_read_exact(&s->reader, buf, take) ||
                    !push_stream_body(s, buf, take)) {
                    ok = false;
                    break;
                }
                left -= take;
            }
            if (!ok)
                break;
            char crlf[8];
            if (!http_conn_reader_line(&s->reader, crlf, sizeof(crlf)) || crlf[0])
                break;
        } else {
            size_t want = sizeof(buf);
            if (limited && rem < want)
                want = (size_t)rem;
            int n = http_conn_reader_read_some(&s->reader, buf, want);
            if (n <= 0)
                break;
            if (!push_stream_body(s, buf, (size_t)n))
                break;
            pthread_mutex_lock(&s->mutex);
            if (s->has_content_length)
                s->remaining -= (uint64_t)n;
            pthread_mutex_unlock(&s->mutex);
        }
    }
    pthread_mutex_lock(&s->mutex);
    s->ended = true;
    pthread_cond_broadcast(&s->cond);
    http_conn_close(s->conn);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static bool start_pump(http_stream_t * s) {
#ifdef HTTP_STREAM_TESTING
    if (fail_next_pump_create) {
        fail_next_pump_create = false;
        return false;
    }
#endif
    if (pthread_create(&s->thread, NULL, pump, s) != 0)
        return false;
    s->thread_started = true;
    return true;
}

static void * gated_pump(void * arg) {
    pump_gate_t * gate = arg;
    pthread_mutex_lock(&gate->mutex);
    while (!gate->released)
        pthread_cond_wait(&gate->cond, &gate->mutex);
    http_stream_t * stream = gate->stream;
    pthread_mutex_unlock(&gate->mutex);
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
    free(gate);
    return pump(stream);
}

static bool create_gated_pump(http_stream_t * stream, pthread_t * thread_out,
                              pump_gate_t ** gate_out) {
    pump_gate_t * gate = calloc(1, sizeof(*gate));
    if (!gate)
        return false;
    gate->stream = stream;
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        free(gate);
        return false;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
#ifdef HTTP_STREAM_TESTING
    if (fail_next_pump_create) {
        fail_next_pump_create = false;
        pthread_cond_destroy(&gate->cond);
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
#endif
    if (pthread_create(thread_out, NULL, gated_pump, gate) != 0) {
        pthread_cond_destroy(&gate->cond);
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
    *gate_out = gate;
    return true;
}

static void release_gated_pump(pump_gate_t * gate) {
    pthread_mutex_lock(&gate->mutex);
    gate->released = true;
    pthread_cond_signal(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static void stream_adopt_response(http_stream_t *s, response_t *r, bool negotiated, bool verify_tls) {
    s->conn = r->conn;
    r->conn = NULL;
    s->reader = r->reader;
    s->reader.conn = s->conn;
    s->chunked = r->chunked;
    s->has_content_length = r->has_length && !r->chunked;
    s->content_length = r->content_length;
    s->remaining = r->content_length;
    s->icy_audio_left = s->icy_interval = r->icy_interval;
    s->seekable = !r->icy_interval && negotiated && r->status == 206 && r->range_valid &&
                  r->range_start == 0 && r->range_end + 1 == r->range_total &&
                  r->range_total <= INT64_MAX &&
                  (!r->has_length || r->content_length == r->range_total);
    if (s->seekable && !r->has_length) {
        s->has_content_length = true;
        s->remaining = r->range_total;
    }
    s->length = r->icy_interval ? UINT64_MAX : s->seekable ? r->range_total
                            : (r->has_length ? r->content_length : UINT64_MAX);
    s->verify_tls = verify_tls;
    snprintf(s->content_type, sizeof(s->content_type), "%s", r->content_type);
    snprintf(s->final_url, sizeof(s->final_url), "%s", r->final_url);
    snprintf(s->etag, sizeof(s->etag), "%s", r->etag);
    snprintf(s->last_modified, sizeof(s->last_modified), "%s", r->last_modified);
}

http_stream_t * http_stream_open(const char * url, bool verify_tls,
                                                bool request_icy, uint64_t generation) {
    http_stream_t * s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->playback_generation = generation;
    http_cancel_token_init(&s->cancel);
    register_cancel_stream(s);
    response_t r;
    uint64_t deadline = stream_monotonic_ms() + STREAM_OPEN_TIMEOUT_MS;
    /* First negotiate Range: 0-. Live servers commonly ignore it and still
   * return 200/chunked, preserving the established radio behavior. */
    bool negotiated =
        open_response(url, verify_tls, STREAM_MAX_REDIRECTS, true, 0, NULL, request_icy,
                      &s->cancel, deadline, &r);
    if (!negotiated) {
        if (!open_response(url, verify_tls, STREAM_MAX_REDIRECTS, false, 0, NULL, request_icy,
                           &s->cancel, deadline, &r)) {
            goto fail;
        }
    }
    s->ring = malloc(STREAM_RING_CAPACITY);
    if (!s->ring) {
        http_conn_close(r.conn);
        free(r.conn);
        goto fail;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    stream_adopt_response(s, &r, negotiated, verify_tls);
    if (!start_pump(s)) {
        http_conn_close(s->conn);
        free(s->conn);
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        free(s->ring);
        goto fail;
    }
    snprintf(s->source_url, sizeof(s->source_url), "%s", url);
    pthread_mutex_lock(&metadata_mutex);
    pthread_mutex_lock(&s->mutex);
    s->metadata.stream_id = ++next_stream_id;
    pthread_mutex_unlock(&s->mutex);
    s->metadata_next = metadata_streams;
    metadata_streams = s;
    pthread_mutex_unlock(&metadata_mutex);
    return s;
fail:
    unregister_cancel_stream(s);
    http_cancel_token_destroy(&s->cancel);
    free(s);
    return NULL;
}


size_t http_stream_read(http_stream_t * s, void * buf, size_t n) {
    if (!s || !buf || !n)
        return 0;
    uint8_t * out = buf;
    size_t total = 0;
    pthread_mutex_lock(&s->mutex);
    while (total < n && !s->at_virtual_end) {
        while (!s->ring_fill && !s->ended && !s->stop_requested)
            pthread_cond_wait(&s->cond, &s->mutex);
        if (!s->ring_fill)
            break;
        size_t take = n - total < s->ring_fill ? n - total : s->ring_fill;
        if (s->seekable && s->length - s->position < take)
            take = (size_t)(s->length - s->position);
        if (!take)
            break;
        for (size_t i = 0; i < take; i++) {
            out[total + i] = s->ring[s->ring_tail];
            s->ring_tail = (s->ring_tail + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill -= take;
        s->ring_history += take;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        total += take;
        s->position += take;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
    return total;
}

const char * http_stream_content_type(http_stream_t * s) {
    return s ? s->content_type : "";
}
bool http_stream_can_seek(http_stream_t * s) { return s && s->seekable; }
int64_t http_stream_tell(http_stream_t * s) {
    if (!s)
        return -1;
    pthread_mutex_lock(&s->mutex);
    uint64_t p = s->at_virtual_end ? s->length : s->position;
    pthread_mutex_unlock(&s->mutex);
    return p > INT64_MAX ? -1 : (int64_t)p;
}
int64_t http_stream_length(http_stream_t * s) {
    if (!s || s->length == UINT64_MAX || s->length > INT64_MAX)
        return -1;
    return (int64_t)s->length;
}

static bool open_seek_response(http_stream_t *s, uint64_t dest, response_t *candidate) {
    const char * if_range = NULL;
    if (s->etag[0] && strncmp(s->etag, "W/", 2) != 0 && s->etag[0] == '"')
        if_range = s->etag;
    else if (s->last_modified[0])
        if_range = s->last_modified;
    bool if_range_is_etag = if_range && s->etag[0] && if_range == s->etag;
    if (!open_response(s->final_url, s->verify_tls, STREAM_MAX_REDIRECTS, true,
                       dest, if_range, false, &s->cancel,
                       stream_monotonic_ms() + STREAM_OPEN_TIMEOUT_MS, candidate))
        return false;
    if ((candidate->status != 206 || candidate->range_total != s->length ||
         candidate->icy_interval || !candidate->range_valid || candidate->range_start != dest ||
         candidate->range_end + 1 != candidate->range_total)) {
        http_conn_close(candidate->conn);
        free(candidate->conn);
        return false;
    }
    if (((if_range &&
          strcmp(if_range, if_range_is_etag ? candidate->etag
                                            : candidate->last_modified) != 0) ||
         (s->etag[0] && candidate->etag[0] &&
          strcmp(s->etag, candidate->etag) != 0) ||
         (s->last_modified[0] && candidate->last_modified[0] &&
          strcmp(s->last_modified, candidate->last_modified) != 0))) {
        http_conn_close(candidate->conn);
        free(candidate->conn);
        return false;
    }
    return true;
}

bool http_stream_seek(http_stream_t * s, int64_t offset, int origin) {
    if (!s || !s->seekable)
        return false;
    pthread_mutex_lock(&s->mutex);
    uint64_t pos = s->at_virtual_end ? s->length : s->position;
    bool active = !s->stop_requested;
    pthread_mutex_unlock(&s->mutex);
    if (!active)
        return false;
    uint64_t base;
    if (origin == SEEK_SET)
        base = 0;
    else if (origin == SEEK_CUR)
        base = pos;
    else if (origin == SEEK_END)
        base = s->length;
    else
        return false;
    uint64_t dest;
    if (offset >= 0) {
        uint64_t forward = (uint64_t)offset;
        if (base > s->length || forward > s->length - base)
            return false;
        dest = base + forward;
    } else {
        /* Avoid negating INT64_MIN directly. */
        uint64_t backward = (uint64_t)(-(offset + 1)) + 1;
        if (backward > base)
            return false;
        dest = base - backward;
    }
    /* The cheap cases below move only within bytes the connection already
     * delivered; anything else opens a validated replacement response. */
    pthread_mutex_lock(&s->mutex);
    if (dest == s->length) {
        s->at_virtual_end = true;
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest == s->position) {
        s->at_virtual_end = false;
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest > s->position && dest - s->position <= s->ring_fill) {
        size_t skip = (size_t)(dest - s->position);
        s->ring_tail = (s->ring_tail + skip) % STREAM_RING_CAPACITY;
        s->ring_fill -= skip;
        s->ring_history += skip;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        s->position = dest;
        s->at_virtual_end = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest < s->position && s->position - dest <= s->ring_history) {
        size_t back = (size_t)(s->position - dest);
        s->ring_tail = (s->ring_tail + STREAM_RING_CAPACITY - back) % STREAM_RING_CAPACITY;
        s->ring_fill += back;
        s->ring_history -= back;
        s->position = dest;
        s->at_virtual_end = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    pthread_mutex_unlock(&s->mutex);
    response_t candidate;
    bool have_body = dest < s->length;
    if (have_body && !open_seek_response(s, dest, &candidate)) return false;
    pthread_t candidate_thread;
    pump_gate_t * candidate_gate = NULL;
    if (have_body && !create_gated_pump(s, &candidate_thread, &candidate_gate)) {
        http_conn_close(candidate.conn);
        free(candidate.conn);
        return false;
    }
    /* Stop and join the old pump only after the replacement response has
   * passed validation. shutdown is done while the mutex protects fd life. */
    pthread_mutex_lock(&s->mutex);
    s->stop_requested = true;
    pthread_cond_broadcast(&s->cond);
    /* The pump closes the connection under this same mutex. Shutdown while
   * holding it so the descriptor cannot be closed/reused between lookup
   * and shutdown. */
    if (s->conn && s->conn->net.fd >= 0)
        shutdown(s->conn->net.fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->mutex);
    if (s->thread_started)
        pthread_join(s->thread, NULL);
    free(s->conn);
    s->conn = NULL;
    pthread_mutex_lock(&s->mutex);
    s->ring_head = s->ring_tail = s->ring_fill = 0;
    s->ring_history = 0;
    s->at_virtual_end = false;
    s->position = dest;
    s->stop_requested = false;
    s->ended = !have_body;
    if (have_body) {
        s->conn = candidate.conn;
        candidate.conn = NULL;
        s->reader = candidate.reader;
        s->reader.conn = s->conn;
        s->chunked = candidate.chunked;
        s->has_content_length = !candidate.chunked;
        s->remaining = candidate.has_length
                           ? candidate.content_length
                           : candidate.range_end - candidate.range_start + 1;
        s->thread = candidate_thread;
        s->thread_started = true;
    } else {
        s->thread_started = false;
        s->conn = NULL;
        s->reader.conn = NULL;
    }
    pthread_mutex_unlock(&s->mutex);
    if (have_body)
        release_gated_pump(candidate_gate);
    return true;
}

void http_stream_close(http_stream_t * s) {
    if (!s)
        return;
    pthread_mutex_lock(&metadata_mutex);
    http_stream_t ** link = &metadata_streams;
    while (*link && *link != s) link = &(*link)->metadata_next;
    if (*link) *link = s->metadata_next;
    pthread_mutex_unlock(&metadata_mutex);
    unregister_cancel_stream(s);
    http_cancel_token_cancel(&s->cancel);
    pthread_mutex_lock(&s->mutex);
    s->stop_requested = true;
    pthread_cond_broadcast(&s->cond);
    if (s->conn && s->conn->net.fd >= 0)
        shutdown(s->conn->net.fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->mutex);
    if (s->thread_started)
        pthread_join(s->thread, NULL);
    free(s->conn);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s->ring);
    http_cancel_token_destroy(&s->cancel);
    free(s);
}
