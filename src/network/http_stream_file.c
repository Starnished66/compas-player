#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "http_stream_file.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    http_stream_t * stream;
    uint8_t * span;
    uint64_t span_off;
    uint64_t span_len;
    int64_t pos;
    int64_t file_size;
} span_cookie_t;

static ssize_t span_read(void * opaque, char * buf, size_t bytes) {
    span_cookie_t * cookie = opaque;
    if (bytes > SSIZE_MAX) bytes = SSIZE_MAX;
    size_t total = 0;
    while (total < bytes && cookie->pos < cookie->file_size) {
        uint64_t pos = (uint64_t) cookie->pos;
        if (cookie->span && pos >= cookie->span_off && pos < cookie->span_off + cookie->span_len) {
            size_t into = (size_t) (pos - cookie->span_off);
            size_t avail = (size_t) (cookie->span_len - into);
            size_t take = bytes - total < avail ? bytes - total : avail;
            memcpy(buf + total, cookie->span + into, take);
            cookie->pos += (int64_t) take;
            total += take;
            continue;
        }
        size_t want = bytes - total;
        if (cookie->span && pos < cookie->span_off) {
            uint64_t gap = cookie->span_off - pos;
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
    free(cookie->span);
    free(cookie);
    return 0;
}

FILE * http_stream_open_span_file(http_stream_t * stream, uint8_t * span,
                                  uint64_t span_offset, uint64_t span_length) {
    int64_t length = stream ? http_stream_length(stream) : -1;
    int bad_span = span_length && (length < 0 || span_offset > (uint64_t) length ||
                                   span_length > (uint64_t) length - span_offset);
    if (!span_length) { free(span); span = NULL; }
    if (!stream || !http_stream_can_seek(stream) || length < 0 || length > LONG_MAX || bad_span) {
        free(span);
        return NULL;
    }
    span_cookie_t * cookie = calloc(1, sizeof(*cookie));
    if (!cookie) { free(span); return NULL; }
    cookie->stream = stream;
    cookie->span = span;
    cookie->span_off = span_offset;
    cookie->span_len = span_length;
    cookie->file_size = length;
    cookie_io_functions_t io = { .read = span_read, .seek = span_seek, .close = span_close };
    FILE * file = fopencookie(cookie, "rb", io);
    if (!file) { free(span); free(cookie); return NULL; }
    if (setvbuf(file, NULL, _IONBF, 0) != 0) { fclose(file); return NULL; }
    return file;
}

FILE * http_stream_open_file(http_stream_t * stream) {
    return http_stream_open_span_file(stream, NULL, 0, 0);
}
