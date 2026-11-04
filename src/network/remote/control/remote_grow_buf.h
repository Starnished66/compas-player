#ifndef REMOTE_GROW_BUF_H
#define REMOTE_GROW_BUF_H

/* Heap string that grows to exactly what a response needs. Once an append
 * fails (out of memory, formatting error or max exceeded) the buffer stays
 * failed and every later append returns false, so callers may chain appends
 * and check `failed` once. Header-only so each remote source can use it
 * without extra link dependencies. */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char * data;
    size_t length, capacity;
    size_t max; /* 0 = unbounded, else the most bytes (including the NUL) allowed */
    bool failed;
} grow_buf_t;

static inline bool grow_buf_reserve(grow_buf_t * b, size_t total) {
    size_t capacity = b->capacity ? b->capacity : 4096;
    while (capacity < total) capacity *= 2;
    char * grown = realloc(b->data, capacity);
    if (!grown) return false;
    b->data = grown;
    b->capacity = capacity;
    return true;
}

__attribute__((format(printf, 2, 3)))
static inline bool grow_buf_appendf(grow_buf_t * b, const char * format, ...) {
    if (b->failed) return false;
    va_list ap, ap2;
    va_start(ap, format);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    size_t total = need >= 0 ? b->length + (size_t) need + 1 : 0;
    if (need < 0 || (b->max && total > b->max) || (total > b->capacity && !grow_buf_reserve(b, total))) {
        b->failed = true;
        va_end(ap2);
        return false;
    }
    b->length += (size_t) vsnprintf(b->data + b->length, b->capacity - b->length, format, ap2);
    va_end(ap2);
    return true;
}

#endif /* REMOTE_GROW_BUF_H */
