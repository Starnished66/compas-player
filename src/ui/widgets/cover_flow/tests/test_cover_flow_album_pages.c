#define COVER_FLOW_ALBUMS_TEST
#include "../cover_flow_albums.c"
#include <assert.h>
#include <stdio.h>

static compact_list_page_row_t mock_rows[35];
static int calls;

int albums_fetch_page(void *ctx, int offset, int count, compact_list_page_row_t out[]) {
    (void)ctx;
    calls++;
    if (offset >= 35) return 0;
    int got = 35 - offset < count ? 35 - offset : count;
    memcpy(out, &mock_rows[offset], (size_t)got * sizeof(*out));
    return got;
}

static void header(compact_list_page_row_t *row, int stars) {
    row->is_section_header = true;
    row->section_stars = stars;
}

static void album(compact_list_page_row_t *row, const char *name, int64_t id) {
    snprintf(row->label, sizeof(row->label), "%s", name);
    snprintf(row->subtitle, sizeof(row->subtitle), "%s", "Artist");
    row->identity = id;
}

int main(void) {
    /* Exercise a header-only first page, then the short final album page. */
    for (int i = 0; i < PAGE_ROWS; i++) header(&mock_rows[i], 5);
    album(&mock_rows[32], "First", 11);
    header(&mock_rows[33], 0);
    album(&mock_rows[34], "Second", 22);

    cover_flow_item_t items[2] = {0};
    assert(read_albums(items, 2) == 2);
    assert(calls == 2);
    assert(strcmp(items[0].title, "First") == 0 && items[0].song_id == 11);
    assert(strcmp(items[1].title, "Second") == 0 && items[1].song_id == 22);
    free((char *)items[0].title);
    free((char *)items[0].subtitle);
    free((char *)items[1].title);
    free((char *)items[1].subtitle);

    /* A page with only headers must advance by displayed rows, not albums. */
    calls = 0;
    cover_flow_item_t one[1] = {0};
    assert(read_albums(one, 1) == 1);
    assert(calls == 2);
    assert(strcmp(one[0].title, "First") == 0);
    free((char *)one[0].title);
    free((char *)one[0].subtitle);
    return 0;
}
