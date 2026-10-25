#ifndef ARTWORK_CLOCK_H
#define ARTWORK_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline uint64_t artwork_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

#endif
