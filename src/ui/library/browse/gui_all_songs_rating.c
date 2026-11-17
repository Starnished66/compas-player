#include "gui_rating_rows.h"
#include "gui_library_internal.h"
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
    .mode = all_songs_rating_mode, .load = metadata_db_load_rating_order,
    .landed = rating_landed
};
#define order rating.order
#define order_mutex rating.mutex
#define rating_enabled rating.enabled

bool all_songs_rating_mode(void) { return current_settings.all_songs_sort_mode == 1; }

bool all_songs_rating_active(void) {
    if (!all_songs_rating_mode() || !order.loaded) return false;
    /* Busy readers keep the displayed order. Pages validate the generation
     * before publishing, and playback owns its immutable title snapshot. */
    int32_t generation;
    return !metadata_db_try_get_generation(&generation) || generation == order.generation;
}

static int rating_total(void) {
    int total = order.count;
    for (int s = 0; s < 6; s++) total += order.sections[s] > 0;
    return total;
}

int all_songs_paged_total(void) {
    return all_songs_rating_active() ? rating_total() : (int)metadata_db_get_song_count();
}

/* Maps a display row to the queue position, or identifies its section header. */
static int rating_position(int row, int *stars) {
    return rating_row_position(order.sections, row, stars);
}

int all_songs_rating_rank_at(int row, int *song_pos) {
    if (!all_songs_rating_active()) return -1;
    int pos = rating_position(row, NULL);
    if (song_pos) *song_pos = pos;
    return pos >= 0 ? order.ranks[pos] : -1;
}

int all_songs_rating_display_row(int rank) {
    if (rank < 0 || !all_songs_rating_active()) return rank;
    return rating_display_row_for_rank(order.ranks, order.count, order.sections, rank);
}


int all_songs_rating_fetch_page(void *ctx, int offset, int count, compact_list_page_row_t rows[]) {
    (void)ctx;
    if (offset < 0 || count <= 0) return 0;
    pthread_mutex_lock(&order_mutex);
    bool available = rating_enabled && order.loaded;
    pthread_mutex_unlock(&order_mutex);
    if (!available) return -1;
    int *ranks = malloc(sizeof(*ranks) * (size_t)count);
    song_row_t *songs = malloc(sizeof(*songs) * (size_t)count);
    if (!ranks || !songs) { free(ranks); free(songs); return 0; }
    int n = 0;
    int32_t generation = 0;
    pthread_mutex_lock(&order_mutex);
    generation = order.generation;
    count = rating_enabled ? rating_order_copy_page(&order, offset, count, rows, ranks, &n) : -1;
    pthread_mutex_unlock(&order_mutex);
    if (count > 0 && metadata_db_get_generation() != generation) count = -1;
    if (count > 0 && (metadata_db_get_songs_at_title_ranks(ranks, n, songs) != n ||
                     metadata_db_get_generation() != generation)) count = 0;
    for (int i = 0, j = 0; i < count; i++) {
        if (rows[i].is_section_header) continue;
        fill_song_page_visual(&rows[i], &songs[j]);
        library_selection_snapshot_store(&songs[j], SELECTION_SNAPSHOT_ALL_SONGS, ranks[j], generation);
        j++;
    }
    free(ranks);
    free(songs);
    return count;
}

void all_songs_rating_play(int song_pos) {
    if (!all_songs_rating_active() || song_pos < 0 || song_pos >= order.count) return;
    int *ranks = malloc(sizeof(*ranks) * (size_t)order.count);
    if (!ranks) return;
    memcpy(ranks, order.ranks, sizeof(*ranks) * (size_t)order.count);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, ranks[song_pos], 0, NULL);
    on_file_selected_lazy_ranks(ranks, order.count, song_pos,
                                metadata_db_snapshot_open(false), order.generation);
}

static bool rating_available(void) {
    return metadata_db_try_rating_order_available();
}

static bool rating_visible(void) {
    return all_songs_list && lv_screen_active() == all_songs_screen;
}

static void rating_landed(bool first) {
    rearm_library_list(all_songs_list, all_songs_fetch_page, all_songs_paged_total());
    if (first && !library_search_is_active(SEARCH_BINDING_ALL_SONGS)) {
        lv_obj_update_layout(all_songs_list);
        lv_obj_scroll_to_y(all_songs_list, 0, LV_ANIM_OFF);
    }
    refresh_now_playing_indicators();
}

void all_songs_rating_teardown(void) { rating_list_teardown(&rating); }
void all_songs_rating_start(void) { rating_list_start(&rating); }

void all_songs_rearm(void) {
    if (all_songs_rating_mode()) all_songs_rating_start();
    else all_songs_rating_teardown();
    rearm_library_list(all_songs_list, all_songs_fetch_page, all_songs_paged_total());
}
