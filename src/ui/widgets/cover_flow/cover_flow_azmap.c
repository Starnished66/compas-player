#include "cover_flow_azmap.h"
#include "cover_flow_span.h"
#include "../../library/library_az_table.h"

static const char * item_title(int i, void * ctx) { return ((const cover_flow_item_t *) ctx)[i].title; }

void cover_flow_az_build(const cover_flow_item_t * items, int count, int jump[COVER_FLOW_AZ_LETTERS],
                         bool has[COVER_FLOW_AZ_LETTERS]) {
    library_az_table_build(item_title, (void *) items, count, jump);
    /* A letter has an item when its jump row really starts with it (not borrowed). */
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++)
        has[i] = jump[i] >= 0 && library_az_letter(items[jump[i]].title) == i;
}

int32_t cover_flow_scrub_pos(int32_t x, int32_t left, int32_t width, int count) {
    if (count <= 1 || width <= 0 || x <= left) return 0;
    int64_t max = (int64_t) (count - 1) * COVER_FLOW_ONE;
    if ((int64_t) x >= (int64_t) left + width) return (int32_t) max;
    return (int32_t) (((int64_t) x - left) * max / width);
}

int32_t cover_flow_scrub_x(int32_t pos, int32_t left, int32_t width, int count) {
    if (count <= 1 || width <= 0 || pos <= 0) return left;
    int64_t max = (int64_t) (count - 1) * COVER_FLOW_ONE;
    if (pos >= max) return left + width;
    return left + (int32_t) (((int64_t) pos * width + max / 2) / max);
}

int32_t cover_flow_scrub_notch(int letter, const int * jump, const bool * has,
                              int32_t left, int32_t width, int count) {
    if (letter < 0 || letter >= COVER_FLOW_AZ_LETTERS || !has[letter] ||
        jump[letter] < 0 || jump[letter] >= count) return -1;
    return cover_flow_scrub_x(jump[letter] * COVER_FLOW_ONE, left, width, count);
}
