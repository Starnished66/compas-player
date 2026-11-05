/* Cover flow over the library's albums: the Albums screen's list, order and
 * actions, shown as a carousel. */
#include "cover_flow.h"
#include "../../library/browse/gui_library.h"
#include "../../text/i18n/i18n.h"
#include "../../../library/metadata/db/metadata_db.h"
#include <stdlib.h>
#include <string.h>

#define PAGE_ROWS 32

/* An item's title and subtitle are the album and its album artist: the album's identity, so a
 * tap acts on that album even if the list was re-sorted since. */
static void on_select(int index) {
    const cover_flow_item_t * item = cover_flow_item(index);
    if (item) gui_library_open_album(item->title, item->subtitle);
}

static void on_hold(int index) {
    const cover_flow_item_t * item = cover_flow_item(index);
    if (item) gui_library_open_album_menu(item->title, item->subtitle);
}

static void free_items(cover_flow_item_t * items, int count) {
    for (int i = 0; i < count; i++) {
        free((char *) items[i].title);
        free((char *) items[i].subtitle);
    }
    free(items);
}

static bool set_item(cover_flow_item_t * item, const compact_list_page_row_t * row) {
    item->title = strdup(row->label);
    item->subtitle = strdup(row->subtitle);
    item->song_id = row->identity;
    if (item->title && item->subtitle) return true;
    free((char *) item->title);
    free((char *) item->subtitle);
    return false;
}

/* Reads up to `count` albums page by page; how many were read. */
static int read_albums(cover_flow_item_t * items, int count) {
    compact_list_page_row_t * page = malloc(PAGE_ROWS * sizeof(*page));
    int n = 0;
    while (page && n < count) {
        int want = count - n < PAGE_ROWS ? count - n : PAGE_ROWS;
        int got = albums_fetch_page(NULL, n, want, page);
        int used = 0;
        while (used < got && set_item(&items[n + used], &page[used])) used++;
        n += used;
        if (used < want) break; /* the end of the list, or out of memory */
    }
    free(page);
    return n;
}

void cover_flow_open_albums(void) {
    int artists = 0, album_artists = 0, albums = 0;
    metadata_db_get_group_counts(&artists, &album_artists, &albums);
    if (albums > COVER_FLOW_MAX_ITEMS) albums = COVER_FLOW_MAX_ITEMS;
    cover_flow_item_t * items = albums > 0 ? calloc((size_t) albums, sizeof(*items)) : NULL;
    int count = items ? read_albums(items, albums) : 0;
    if (count == 0) {
        gui_library_open_albums(); /* the Albums screen's own empty state */
    } else {
        cover_flow_params_t params = { TR("Cover Flow"), items, count, 0, on_select, on_hold, true };
        (void) cover_flow_show(&params);
    }
    free_items(items, count);
}
