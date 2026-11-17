#include "gui_library_internal.h"
#include "gui_rating_rows.h"
#include "../library_selection_snapshot.h"
#include "../../../library/remote/remote_state.h"
#include "../../text/i18n/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_rating_lifecycle.h"
static bool rating_available(void);
static bool rating_visible(void);
static void rating_landed(bool first);
static rating_list_t rating = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .available = rating_available, .visible = rating_visible,
    .mode = albums_rating_mode, .load = metadata_db_load_album_rating_order,
    .landed = rating_landed
};
#define order rating.order
#define order_mutex rating.mutex
#define rating_enabled rating.enabled

bool albums_rating_mode(void) { return current_settings.album_sort_mode == METADATA_DB_ALBUM_SORT_RATING; }

bool albums_rating_active(void) {
    if (!albums_rating_mode() || !order.loaded) return false;
    int32_t generation;
    uint32_t incarnation;
    if (!metadata_db_try_get_generation(&generation) || !metadata_db_try_get_incarnation(&incarnation)) return true;
    return generation == order.generation && incarnation == order.incarnation;
}

bool albums_rating_snapshot_loaded(void) {
    pthread_mutex_lock(&order_mutex);
    bool loaded = rating_enabled && order.loaded;
    pthread_mutex_unlock(&order_mutex);
    return loaded;
}

int albums_rating_total(void) {
    if (!albums_rating_active()) {
        int artists = 0, album_artists = 0, albums = 0;
        metadata_db_get_group_counts(&artists, &album_artists, &albums);
        return albums;
    }
    int total = order.count;
    for (int s = 0; s < 6; s++) total += order.sections[s] > 0;
    return total;
}

bool albums_rating_resolve_row(int row, group_row_t *out) {
    if (!out || !albums_rating_mode() || row < 0) return false;
    pthread_mutex_lock(&order_mutex);
    bool loaded = rating_enabled && order.loaded;
    int canonical = loaded ? rating_row_position(order.sections, row, NULL) : -1;
    if (canonical >= 0 && canonical < order.count) canonical = order.ranks[canonical];
    else canonical = -1;
    int32_t expected_generation = order.generation;
    uint32_t expected_incarnation = order.incarnation;
    pthread_mutex_unlock(&order_mutex);
    if (canonical < 0) return false;
    if (metadata_db_get_albums_at_canonical_ranks(&canonical, 1, out) != 1) return false;
    int32_t generation;
    uint32_t incarnation;
    return metadata_db_try_get_generation(&generation) &&
           metadata_db_try_get_incarnation(&incarnation) &&
           generation == expected_generation && incarnation == expected_incarnation;
}

int albums_rating_display_row(int canonical_rank) {
    if (canonical_rank < 0 || !albums_rating_active()) return -1;
    return rating_display_row_for_rank(order.ranks, order.count, order.sections, canonical_rank);
}


int albums_rating_fetch_page(void *ctx, int offset, int count, compact_list_page_row_t rows[]) {
    (void)ctx;
    if (offset < 0 || count <= 0) return 0;
    pthread_mutex_lock(&order_mutex);
    bool available = rating_enabled && order.loaded;
    pthread_mutex_unlock(&order_mutex);
    if (!available) return -1;
    int *ranks = malloc(sizeof(*ranks) * (size_t)count);
    group_row_t *groups = malloc(sizeof(*groups) * (size_t)count);
    if (!ranks || !groups) { free(ranks); free(groups); return 0; }
    pthread_mutex_lock(&order_mutex);
    int32_t generation = order.generation;
    uint32_t expected_incarnation = order.incarnation;
    int albums = 0;
    int display_count = rating_enabled ?
        rating_order_copy_page(&order, offset, count, rows, ranks, &albums) : -1;
    pthread_mutex_unlock(&order_mutex);
    if (albums > 0 && metadata_db_get_generation() != generation) albums = -2;
    if (albums > 0 && (metadata_db_get_albums_at_canonical_ranks(ranks, albums, groups) != albums ||
                        metadata_db_get_generation() != generation)) albums = -2;
    uint32_t current_incarnation = albums >= 0 ? metadata_db_get_incarnation() : 0;
    if (albums >= 0 && current_incarnation != expected_incarnation) albums = -2;
    for (int i = 0, j = 0; i < display_count && albums >= 0; i++) {
        if (rows[i].is_section_header) continue;
        if (j >= albums) break;
        snprintf(rows[i].label, sizeof(rows[i].label), "%s", groups[j].name);
        snprintf(rows[i].subtitle, sizeof(rows[i].subtitle), "%s", groups[j].album_artist);
        rows[i].identity = groups[j].first_song_id;
        fill_album_cover_key(&rows[i], &groups[j]);
        snprintf(rows[i].trailing_asset, sizeof(rows[i].trailing_asset), "%s", "navigation/more.png");
        j++;
    }
    free(ranks);
    free(groups);
    return albums == -2 ? -2 : display_count;
}

static bool rating_available(void) {
    bool available = false;
    return metadata_db_try_album_sort_available(METADATA_DB_ALBUM_SORT_RATING, &available) && available;
}

static bool rating_visible(void) {
    return albums_list && lv_screen_active() == lv_obj_get_screen(albums_list);
}

static void rating_landed(bool first) {
    rearm_library_list(albums_list, albums_fetch_page, albums_rating_total());
    if (first && !library_search_is_active(SEARCH_BINDING_ALBUMS)) {
        lv_obj_update_layout(albums_list);
        lv_obj_scroll_to_y(albums_list, 0, LV_ANIM_OFF);
    }
    refresh_now_playing_indicators();
}

void albums_rating_teardown(void) { rating_list_teardown(&rating); }
void albums_rating_start(void) { rating_list_start(&rating); }

void albums_rating_rearm(void) {
    if (albums_rating_mode()) albums_rating_start();
    else albums_rating_teardown();
    int artists = 0, album_artists = 0, count = 0;
    metadata_db_get_group_counts(&artists, &album_artists, &count);
    rearm_library_list(albums_list, albums_fetch_page,
                       albums_rating_active() ? albums_rating_total() : count);
}
