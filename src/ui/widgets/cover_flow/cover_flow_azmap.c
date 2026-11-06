#include "cover_flow_azmap.h"
#include "../../library/library_az_table.h"

static const char * item_title(int i, void * ctx) { return ((const cover_flow_item_t *) ctx)[i].title; }

void cover_flow_az_build(const cover_flow_item_t * items, int count, int jump[COVER_FLOW_AZ_LETTERS],
                         bool has[COVER_FLOW_AZ_LETTERS]) {
    library_az_table_build(item_title, (void *) items, count, jump);
    /* A letter has an item when its jump row really starts with it (not borrowed). */
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++)
        has[i] = jump[i] >= 0 && library_az_letter(items[jump[i]].title) == i;
}
