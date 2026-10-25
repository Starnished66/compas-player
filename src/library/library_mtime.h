#ifndef LIBRARY_MTIME_H
#define LIBRARY_MTIME_H

#include <stdbool.h>
#include <stdint.h>

/* File times on the SD card are not comparable across machines.
 *
 * FAT (and exFAT) store local wall-clock time, 2 s apart. The player mounts
 * the card without a tz= or time_offset= option and never sets the kernel
 * time zone, so it reads the stored digits as UTC. Windows and macOS convert
 * those digits with the computer's own zone, so the same file reports a time
 * that differs by that zone's offset, plus an hour across DST. Every real UTC
 * offset is a whole number of 15 minutes and none exceeds 14 hours, so two
 * machines can disagree by at most 26 hours, always in 15-minute steps.
 *
 * A recorded time and a current one therefore describe the same file version
 * when they are equal or differ by a whole number of 15 minutes within 26
 * hours. The size is compared separately, so a rewrite that keeps the size
 * and moves the time by exactly such a step is missed (about 1 in 900 for an
 * edit made within a day of the last one). */
#define LIBRARY_MTIME_ZONE_STEP_SECONDS 900
#define LIBRARY_MTIME_ZONE_SPAN_SECONDS (26 * 3600)

static inline bool library_mtime_same_version(int64_t recorded, int64_t current) {
    int64_t delta = recorded > current ? recorded - current : current - recorded;
    return delta == 0 || (delta <= LIBRARY_MTIME_ZONE_SPAN_SECONDS &&
                          delta % LIBRARY_MTIME_ZONE_STEP_SECONDS == 0);
}

#endif /* LIBRARY_MTIME_H */
