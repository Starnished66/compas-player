#ifndef COMPAS_MONOTONIC_H
#define COMPAS_MONOTONIC_H

#include <stdint.h>
#include <time.h>

static inline uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000u + (uint64_t) now.tv_nsec / 1000000u;
}

/* Low 32 bits of the monotonic clock in microseconds: differences between
 * nearby reads stay exact across the 71 minute wrap. */
static inline uint32_t monotonic_us32(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t) ((uint64_t) now.tv_sec * 1000000u + (uint64_t) now.tv_nsec / 1000u);
}

#endif
