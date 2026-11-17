#pragma once

/* Pure row mapping shared with the host regression harness. Sections are
 * counts indexed by stars 0..5; display order is 5 down to 0. */
static inline int rating_row_position(const int sections[6], int row, int *stars) {
    if (row < 0) return -1;
    int position = 0;
    for (int s = 5; s >= 0; s--) {
        int count = sections[s];
        if (!count) continue;
        if (row == 0) { if (stars) *stars = s; return -1; }
        row--;
        if (row < count) return position + row;
        row -= count;
        position += count;
    }
    return -1;
}

static inline int rating_display_row_for_rank(const int *ranks, int count,
                                                     const int sections[6], int canonical_rank) {
    if (!ranks || canonical_rank < 0) return -1;
    int position = 0, row = 0;
    for (int s = 5; s >= 0; s--) {
        if (sections[s]) row++;
        for (int i = 0; i < sections[s]; i++, position++, row++)
            if (position < count && ranks[position] == canonical_rank) return row;
    }
    return -1;
}
