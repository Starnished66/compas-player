#ifndef COMPAS_MONOTONIC_H
#define COMPAS_MONOTONIC_H

#include <stdint.h>
#include <time.h>

static inline uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000u + (uint64_t) now.tv_nsec / 1000000u;
}

#endif
