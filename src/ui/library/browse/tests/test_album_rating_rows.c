#include "../gui_rating_rows.h"
#include <assert.h>

int main(void) {
    /* One five-star, two three-star, one unrated album. Each nonempty
     * section adds a display header; headers never resolve to albums. */
    const int sections[6] = {1, 0, 0, 2, 0, 1};
    const int ranks[] = {90, 20, 30, 7};
    int stars = -1;
    assert(rating_row_position(sections, 0, &stars) == -1 && stars == 5);
    assert(rating_row_position(sections, 1, &stars) == 0 && stars == 5);
    assert(rating_row_position(sections, 2, &stars) == -1 && stars == 3);
    assert(rating_row_position(sections, 3, &stars) == 1 && stars == 3);
    assert(rating_row_position(sections, 4, &stars) == 2 && stars == 3);
    assert(rating_row_position(sections, 5, &stars) == -1 && stars == 0);
    assert(rating_row_position(sections, 6, &stars) == 3 && stars == 0);
    assert(rating_row_position(sections, 7, &stars) == -1);
    assert(rating_display_row_for_rank(ranks, 4, sections, 90) == 1);
    assert(rating_display_row_for_rank(ranks, 4, sections, 20) == 3);
    assert(rating_display_row_for_rank(ranks, 4, sections, 7) == 6);
    assert(rating_display_row_for_rank(ranks, 4, sections, 999) == -1);
    return 0;
}
