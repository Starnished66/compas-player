#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "http_stream_file.h"
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    http_stream_t * stream;
    uint8_t * bytes;          /* spans' copies, concatenated in span order */
    http_span_t * spans;
    size_t span_count;
    int64_t pos;
    int64_t file_size;
} span_cookie_t;

/* First span ending after pos, and where its copy starts in bytes. */
static const http_span_t * span_from(const span_cookie_t * cookie, uint64_t pos, size_t * at) {
    size_t base = 0;
    for (size_t i = 0; i < cookie->span_count; i++) {
        const http_span_t * span = &cookie->spans[i];
        if (pos < span->offset + span->length) {
            *at = base;
            return span;
        }
        base += (size_t) span->length;
    }
    return NULL;
}

static ssize_t span_read(void * opaque, char * buf, size_t bytes) {
    span_cookie_t * cookie = opaque;
    if (bytes > SSIZE_MAX) bytes = SSIZE_MAX;
    size_t total = 0;
    while (total < bytes && cookie->pos < cookie->file_size) {
        uint64_t pos = (uint64_t) cookie->pos;
        size_t at = 0;
        const http_span_t * span = span_from(cookie, pos, &at);
        if (span && pos >= span->offset) {
            size_t into = (size_t) (pos - span->offset);
            size_t avail = (size_t) (span->length - into);
            size_t take = bytes - total < avail ? bytes - total : avail;
            memcpy(buf + total, cookie->bytes + at + into, take);
            cookie->pos += (int64_t) take;
            total += take;
            continue;
        }
        size_t want = bytes - total;
        if (span) {
            uint64_t gap = span->offset - pos;
            if (gap < want) want = (size_t) gap;
        }
        uint64_t tail = (uint64_t) cookie->file_size - pos;
        if (tail < want) want = (size_t) tail;
        int64_t tell = http_stream_tell(cookie->stream);
        if (tell != cookie->pos && !http_stream_seek(cookie->stream, cookie->pos, SEEK_SET)) {
            if (total) return (ssize_t) total;
            errno = EIO;
            return -1;
        }
        size_t got = http_stream_read(cookie->stream, buf + total, want);
        if (!got) break;
        cookie->pos += (int64_t) got;
        total += got;
    }
    return (ssize_t) total;
}

static int span_seek(void * opaque, off64_t * offset, int origin) {
    span_cookie_t * cookie = opaque;
    int64_t base;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_CUR) base = cookie->pos;
    else if (origin == SEEK_END) base = cookie->file_size;
    else { errno = EINVAL; return -1; }
    /* Logical only. Touching the stream here would reconnect on every moov
     * rewind; the next read seeks if the byte is not in the span. */
    if (*offset > INT64_MAX || *offset < -INT64_MAX) { errno = EINVAL; return -1; }
    int64_t rel = (int64_t) *offset;
    if (rel >= 0) {
        if ((uint64_t) rel > (uint64_t) cookie->file_size - (uint64_t) base) { errno = EINVAL; return -1; }
    } else if ((uint64_t) (-rel) > (uint64_t) base) {
        errno = EINVAL;
        return -1;
    }
    cookie->pos = base + rel;
    *offset = (off64_t) cookie->pos;
    return 0;
}

static int span_close(void * opaque) {
    span_cookie_t * cookie = opaque;
    free(cookie->bytes);
    free(cookie->spans);
    free(cookie);
    return 0;
}

FILE * http_stream_open_spans_file(http_stream_t * stream, uint8_t * bytes,
                                   http_span_t * spans, size_t span_count) {
    int64_t length = stream ? http_stream_length(stream) : -1;
    bool bad_span = span_count && (!bytes || !spans);
    uint64_t end = 0;
    for (size_t i = 0; i < span_count && !bad_span; i++) {
        bad_span = !spans[i].length || spans[i].offset < end || length < 0 ||
                   spans[i].offset > (uint64_t) length || spans[i].length > (uint64_t) length - spans[i].offset;
        end = spans[i].offset + spans[i].length;
    }
    if (!stream || !http_stream_can_seek(stream) || length < 0 || length > LONG_MAX || bad_span) {
        free(bytes);
        free(spans);
        return NULL;
    }
    span_cookie_t * cookie = calloc(1, sizeof(*cookie));
    if (!cookie) { free(bytes); free(spans); return NULL; }
    cookie->stream = stream;
    cookie->bytes = bytes;
    cookie->spans = spans;
    cookie->span_count = span_count;
    cookie->file_size = length;
    cookie_io_functions_t io = { .read = span_read, .seek = span_seek, .close = span_close };
    FILE * file = fopencookie(cookie, "rb", io);
    if (!file) { span_close(cookie); return NULL; }
    if (setvbuf(file, NULL, _IONBF, 0) != 0) { fclose(file); return NULL; }
    return file;
}

FILE * http_stream_open_file(http_stream_t * stream) {
    return http_stream_open_spans_file(stream, NULL, NULL, 0);
}
