#include "opusfile_alloc.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OPUSFILE_LIVE_ALLOCATION_LIMIT (16U * 1024U * 1024U)

typedef union {
    max_align_t alignment;
    struct {
        size_t size;
    } allocation;
} allocation_header_t;

static atomic_size_t live_allocation_bytes;

static bool allocation_total(size_t payload_size, size_t * total_out) {
    if (payload_size > SIZE_MAX - sizeof(allocation_header_t)) {
        errno = ENOMEM;
        return false;
    }
    *total_out = sizeof(allocation_header_t) + payload_size;
    return true;
}

static bool reserve_allocation(size_t bytes) {
    size_t current = atomic_load_explicit(&live_allocation_bytes, memory_order_relaxed);
    for (;;) {
        if (current > OPUSFILE_LIVE_ALLOCATION_LIMIT ||
            bytes > OPUSFILE_LIVE_ALLOCATION_LIMIT - current) {
            errno = ENOMEM;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&live_allocation_bytes, &current, current + bytes,
                                                  memory_order_relaxed, memory_order_relaxed)) {
            return true;
        }
    }
}

static void release_allocation(size_t bytes) {
    atomic_fetch_sub_explicit(&live_allocation_bytes, bytes, memory_order_relaxed);
}

void * compas_opusfile_malloc(size_t size) {
    size_t total;
    if (!allocation_total(size, &total) || !reserve_allocation(total)) return NULL;

    allocation_header_t * allocation = malloc(total);
    if (!allocation) {
        release_allocation(total);
        return NULL;
    }
    allocation->allocation.size = size;
    return allocation + 1;
}

void * compas_opusfile_calloc(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t payload_size = count * size;
    void * ptr = compas_opusfile_malloc(payload_size);
    if (ptr) memset(ptr, 0, payload_size);
    return ptr;
}

void compas_opusfile_free(void * ptr) {
    if (!ptr) return;
    allocation_header_t * allocation = (allocation_header_t *) ptr - 1;
    size_t total = sizeof(*allocation) + allocation->allocation.size;
    free(allocation);
    release_allocation(total);
}

void * compas_opusfile_realloc(void * ptr, size_t size) {
    if (!ptr) return compas_opusfile_malloc(size);
    if (size == 0) {
        compas_opusfile_free(ptr);
        return NULL;
    }

    allocation_header_t * old_allocation = (allocation_header_t *) ptr - 1;
    size_t old_size = old_allocation->allocation.size;
    size_t new_total;
    if (!allocation_total(size, &new_total) || !reserve_allocation(new_total)) return NULL;

    allocation_header_t * new_allocation = malloc(new_total);
    if (!new_allocation) {
        release_allocation(new_total);
        return NULL;
    }
    new_allocation->allocation.size = size;
    memcpy(new_allocation + 1, ptr, old_size < size ? old_size : size);
    compas_opusfile_free(ptr);
    return new_allocation + 1;
}

OggOpusFile * compas_opusfile_open(const char * path, bool metadata_only) {
    if (!path) return NULL;

    int error = 0;
    OggOpusFile * file = metadata_only
        ? op_test_file(path, &error)
        : op_open_file(path, &error);
    if (file && !metadata_only && !op_seekable(file)) {
        op_free(file);
        return NULL;
    }
    return file;
}
