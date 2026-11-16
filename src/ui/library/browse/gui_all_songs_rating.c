#include "gui_library_internal.h"
#include "../library_selection_snapshot.h"
#include "../../../library/remote/remote_state.h"
#include "../../text/i18n/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int *ranks, count, sections[6];
    int32_t generation;
    unsigned revision;
    bool loaded;
} rating_order_t;

typedef struct {
    pthread_t thread;
    atomic_bool done;
    unsigned lifetime;
    rating_order_t order;
} rating_job_t;

/* Page providers run on compact-list workers. Copy only the requested page
 * under this lock; database reads and LVGL calls always happen outside it. */
static pthread_mutex_t order_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool rating_enabled;
static rating_order_t order;
static rating_job_t *job;
static unsigned lifetime;
static lv_timer_t *refresh_timer;
static void rating_schedule(void);

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
    if (row < 0) return -1;
    int pos = 0;
    for (int s = 5; s >= 0; s--) {
        int count = order.sections[s];
        if (!count) continue;
        if (row == 0) {
            if (stars) *stars = s;
            return -1;
        }
        row--;
        if (row < count) return pos + row;
        row -= count;
        pos += count;
    }
    return -1;
}

int all_songs_rating_rank_at(int row, int *song_pos) {
    if (!all_songs_rating_active()) return -1;
    int pos = rating_position(row, NULL);
    if (song_pos) *song_pos = pos;
    return pos >= 0 ? order.ranks[pos] : -1;
}

int all_songs_rating_display_row(int rank) {
    if (rank < 0 || !all_songs_rating_active()) return rank;
    int pos = 0, row = 0;
    for (int s = 5; s >= 0; s--) {
        if (order.sections[s]) row++;
        for (int i = 0; i < order.sections[s]; i++, pos++, row++)
            if (order.ranks[pos] == rank) return row;
    }
    return -1;
}

static int rating_copy_page(int offset, int count, compact_list_page_row_t rows[],
                            int *ranks, int *song_count, int32_t *generation) {
    if (!rating_enabled || !order.loaded) return -1;
    int total = rating_total();
    if (offset >= total) return 0;
    if (count > total - offset) count = total - offset;
    *generation = order.generation;
    for (int i = 0; i < count; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        int stars = -1;
        int pos = rating_position(offset + i, &stars);
        if (stars >= 0) {
            rows[i].is_section_header = true;
            rows[i].section_stars = stars;
            if (!stars) snprintf(rows[i].label, sizeof(rows[i].label), "%s", TR("Unrated"));
        } else ranks[(*song_count)++] = order.ranks[pos];
    }
    return count;
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
    count = rating_copy_page(offset, count, rows, ranks, &n, &generation);
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

static void *rating_load(void *data) {
    rating_job_t *pending = data;
    rating_order_t *result = &pending->order;
    result->loaded = metadata_db_load_rating_order(&result->ranks, result->sections,
                                                   &result->count, &result->generation);
    atomic_store_explicit(&pending->done, true, memory_order_release);
    return NULL;
}

static void rating_land(lv_timer_t *timer) {
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;
    /* A busy database says nothing about the result: retry on the next tick. */
    int32_t generation;
    if (!metadata_db_try_get_generation(&generation)) return;
    pthread_join(job->thread, NULL);
    bool valid = job->lifetime == lifetime && all_songs_rating_mode() && all_songs_list &&
        job->order.loaded && job->order.revision == remote_state_revision() &&
        generation == job->order.generation;
    if (valid) {
        bool first = !order.loaded;
        pthread_mutex_lock(&order_mutex);
        free(order.ranks);
        order = job->order;
        pthread_mutex_unlock(&order_mutex);
        job->order.ranks = NULL;
        rearm_library_list(all_songs_list, all_songs_fetch_page, all_songs_paged_total());
        if (first && !library_search_is_active(SEARCH_BINDING_ALL_SONGS)) {
            lv_obj_update_layout(all_songs_list);
            lv_obj_scroll_to_y(all_songs_list, 0, LV_ANIM_OFF);
        }
        refresh_now_playing_indicators();
    }
    free(job->order.ranks);
    free(job);
    job = NULL;
    lv_timer_delete(timer);
}

static void rating_schedule(void) {
    if (job || library_rescan_active || !metadata_db_try_rating_order_available()) return;
    rating_job_t *pending = calloc(1, sizeof(*pending));
    if (!pending) return;
    pending->lifetime = lifetime;
    pending->order.revision = remote_state_revision();
    atomic_init(&pending->done, false);
    lv_timer_t *timer = lv_timer_create(rating_land, 25, NULL);
    if (!timer) { free(pending); return; }
    if (pthread_create(&pending->thread, NULL, rating_load, pending)) {
        lv_timer_delete(timer);
        free(pending);
        return;
    }
    job = pending;
}

static void rating_poll(lv_timer_t *timer) {
    (void)timer;
    if (all_songs_rating_mode() && lv_screen_active() == all_songs_screen &&
        (!all_songs_rating_active() || order.revision != remote_state_revision())) rating_schedule();
}

void all_songs_rating_teardown(void) {
    lifetime++;
    if (refresh_timer) lv_timer_delete(refresh_timer);
    refresh_timer = NULL;
    pthread_mutex_lock(&order_mutex);
    free(order.ranks);
    order = (rating_order_t){0};
    rating_enabled = false;
    pthread_mutex_unlock(&order_mutex);
}

void all_songs_rating_start(void) {
    pthread_mutex_lock(&order_mutex);
    rating_enabled = true;
    pthread_mutex_unlock(&order_mutex);
    if (!refresh_timer) refresh_timer = lv_timer_create(rating_poll, 250, NULL);
    rating_schedule();
}

void all_songs_rearm(void) {
    if (all_songs_rating_mode()) all_songs_rating_start();
    else all_songs_rating_teardown();
    rearm_library_list(all_songs_list, all_songs_fetch_page, all_songs_paged_total());
}
