#ifndef SWAP_PRESSURE_H
#define SWAP_PRESSURE_H

#include <stdbool.h>

/* Swap counts as pressure when less than a quarter of it is free, with
 * hysteresis: pressure ends again once at least 35% is free. No swap at all
 * (SwapTotal 0) is never pressure. Values are kB from /proc/meminfo. */
static inline bool swap_pressure_next(bool previous, unsigned long total_kb, unsigned long free_kb) {
    if (total_kb == 0) return false;
    if (free_kb > total_kb) free_kb = total_kb;
    if (previous) return free_kb * 100UL < total_kb * 35UL;
    return free_kb * 4UL < total_kb;
}

#endif
