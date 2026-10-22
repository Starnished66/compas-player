/* Positional regression for group_songs' bulk-delete scroll anchor mapping. */
#include "gui_library.c"

#include <assert.h>

static void expect_anchor(int old_top, int song_count, const int *deleted, int deleted_count,
                          int expected) {
    assert(group_songs_anchor_index_after_delete(old_top, song_count, deleted, deleted_count) ==
           expected);
}

int main(void) {
    /* Edit/Done toggles have no deletions, so the visible row keeps its index. */
    expect_anchor(17, 300, NULL, 0, 17);

    /* Deleting entries above the anchor shifts it by every removed position. */
    const int before_anchor[] = {1, 2, 8};
    expect_anchor(17, 300, before_anchor, 3, 14);

    /* Two identical paths at positions 4 and 8 are still separate entries;
     * deleting the visible occurrence at 4 keeps the next row at 4. */
    const int one_duplicate_occurrence[] = {4};
    expect_anchor(4, 10, one_duplicate_occurrence, 1, 4);

    /* Repeated playlist paths remain distinct positions; consecutive selected
     * rows above the next survivor must all contribute to its new index. */
    const int duplicate_positions[] = {10, 11, 12};
    expect_anchor(10, 30, duplicate_positions, 3, 10);

    /* The first selected row falls forward to the next surviving row. */
    const int first_deleted[] = {0};
    expect_anchor(0, 5, first_deleted, 1, 0);

    /* At EOF, the anchor falls back to the previous surviving row. */
    const int last_deleted[] = {4};
    expect_anchor(4, 5, last_deleted, 1, 3);

    /* If every entry is removed there is no row to anchor. */
    const int all_deleted[] = {0, 1, 2, 3, 4};
    expect_anchor(2, 5, all_deleted, 5, -1);

    /* Mapping stays positional across the 200-row page boundary. */
    const int boundary_deleted[] = {200, 201};
    expect_anchor(200, 401, boundary_deleted, 2, 200);

    return 0;
}
