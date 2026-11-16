#pragma once
#include <stdbool.h>

/* Reversal is its own inverse: use for both display rows and title ranks. */
static inline int title_order_index(int index, int total, bool reverse) {
    if (index < 0 || index >= total) return -1;
    return reverse ? total - 1 - index : index;
}
