#include "gui_library_internal.h"
#include "../../../library/metadata/db/album_rating.h"
#include "../../../library/remote/remote_state.h"
#include "../../text/i18n/i18n.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

static lv_obj_t *favorite_screen, *favorite_list;
static group_row_t *favorite_groups;
static int favorite_count, favorite_rows;
static int *favorite_stars;
static compact_list_item_t *favorite_items;
static int *favorite_sources;
static lv_timer_t *favorite_timer;
static unsigned favorite_revision;

static void favorite_album_click(int index) {
    if (index < 0 || index >= favorite_rows || favorite_sources[index] < 0) return;
    index = favorite_sources[index];
    gui_library_open_album(favorite_groups[index].name, favorite_groups[index].album_artist);
}

static void favorite_album_menu(int index) {
    if (index < 0 || index >= favorite_rows || favorite_sources[index] < 0) return;
    index = favorite_sources[index];
    gui_library_open_album_menu(favorite_groups[index].name, favorite_groups[index].album_artist);
}

typedef struct {
    pthread_t thread;
    atomic_bool done;
    unsigned generation, revision;
    group_row_t *groups;
    compact_list_item_t *items;
    int count;
    int *stars;
} favorite_job_t;

static favorite_job_t *favorite_job;
static unsigned favorite_generation;
static lv_timer_t *job_timer;

static void *favorite_albums_load(void *data) {
    favorite_job_t *job = data;
    rated_album_t *rated = NULL;
    int count = 0;
    metadata_db_load_rated_albums(&rated, &count);
    job->groups = count ? calloc((size_t)count, sizeof(*job->groups)) : NULL;
    job->items = count ? calloc((size_t)count, sizeof(*job->items)) : NULL;
    job->stars = count ? calloc((size_t)count, sizeof(*job->stars)) : NULL;
    for (int i = 0; job->groups && job->items && job->stars && i < count; i++) {
        if (rated[i].stars < 3) continue;
        if (album_group_resolve(rated[i].album, rated[i].album_artist,
                                &job->groups[job->count])) {
            compact_list_page_row_t cover = {0};
            fill_album_cover_key(&cover, &job->groups[job->count]);
            job->stars[job->count] = rated[i].stars;
            job->items[job->count++].artwork_key = cover.artwork_key;
        }
    }
    metadata_db_free_rated_albums(rated, count);
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static void favorite_albums_sections(void) {
    int capacity = favorite_count + 3;
    compact_list_item_t *rows = calloc((size_t)capacity, sizeof(*rows));
    favorite_sources = malloc((size_t)capacity * sizeof(*favorite_sources));
    favorite_rows = 0;
    if (!rows || !favorite_sources) { free(rows); return; }
    for (int i = 0; i < favorite_count; i++) {
        if (!i || favorite_stars[i] != favorite_stars[i - 1]) {
            rows[favorite_rows] = (compact_list_item_t){
                .label = "", .is_section_header = true, .section_stars = favorite_stars[i]};
            favorite_sources[favorite_rows++] = -1;
        }
        rows[favorite_rows] = favorite_items[i];
        favorite_sources[favorite_rows++] = i;
    }
    compact_list_set_items(favorite_list, rows, favorite_rows);
    free(rows);
}

static void favorite_albums_apply(favorite_job_t *job) {
    compact_list_item_t *items = job->items;
    for (int i = 0; i < job->count; i++) {
        group_row_t *group = &job->groups[i];

        items[i] = (compact_list_item_t){
            .label = group->name, .subtitle = group->album_artist,
            .identity = group->first_song_id, .artwork_key = items[i].artwork_key,
            .trailing_asset = "navigation/more.png",
        };
    }
    compact_list_set_items(favorite_list, NULL, 0);
    free(favorite_groups);
    free(favorite_items);
    free(favorite_stars);
    free(favorite_sources);
    favorite_sources = NULL;
    favorite_groups = job->groups;
    favorite_items = job->items;
    favorite_stars = job->stars;
    favorite_count = job->count;
    job->groups = NULL;
    job->items = NULL;
    job->stars = NULL;
    favorite_albums_sections();

    favorite_revision = job->revision;
}

static void favorite_albums_job_poll(lv_timer_t *timer) {
    favorite_job_t *job = favorite_job;
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;
    pthread_join(job->thread, NULL);
    if (favorite_screen && job->generation == favorite_generation)
        favorite_albums_apply(job);
    free(job->stars);
    free(job->items);
    free(job->groups);
    free(job);
    favorite_job = NULL;
    job_timer = NULL;
    lv_timer_delete(timer);
}

static void favorite_albums_schedule(void) {
    if (favorite_job) return;
    favorite_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;
    atomic_init(&job->done, false);
    job->generation = favorite_generation;
    job->revision = remote_state_revision();
    job_timer = lv_timer_create(favorite_albums_job_poll, 25, NULL);
    if (!job_timer) { free(job); return; }
    if (pthread_create(&job->thread, NULL, favorite_albums_load, job)) {
        lv_timer_delete(job_timer);
        job_timer = NULL;
        free(job);
        return;
    }
    favorite_job = job;
}

static void favorite_albums_refresh(lv_event_t *e) {
    (void)e;
    ++favorite_generation;
    favorite_revision = remote_state_revision() - 1;
    favorite_albums_schedule();
}

static void favorite_albums_poll(lv_timer_t *timer) {
    (void)timer;
    if (lv_screen_active() == favorite_screen && favorite_revision != remote_state_revision())
        favorite_albums_schedule();
}

static void favorite_albums_deleted(lv_event_t *e) {
    (void)e;
    ++favorite_generation;
    if (favorite_timer) lv_timer_delete(favorite_timer);
    favorite_timer = NULL;
    library_thumbnails_forget_list(favorite_list);
    free(favorite_groups);
    free(favorite_stars);
    free(favorite_sources);
    favorite_sources = NULL;
    free(favorite_items);
    favorite_groups = NULL;
    favorite_stars = NULL;
    favorite_items = NULL;
    favorite_rows = 0;
    favorite_count = 0;
    favorite_screen = favorite_list = NULL;
}

void favorite_albums_teardown(void) {
    if (favorite_screen) lv_obj_delete(favorite_screen);
}

void show_favorite_albums(void) {
    if (!favorite_screen) {
        favorite_screen = build_compact_list_screen(TR("Top rated albums"), generic_back_cb,
            NULL, 0, favorite_album_click, favorite_album_menu, &favorite_list, NULL,
            LIST_ROW_WIDTH_WIDE, false, NULL);
        lv_obj_set_name(favorite_list, "top_rated_albums");
        compact_list_set_empty_heading(favorite_list, TR("No albums rated 3 stars or more yet."));
        compact_list_set_row_height(favorite_list, MUSIC_LIST_ROW_HEIGHT);
        library_thumbnails_bind_list(favorite_screen, favorite_list, LIBRARY_THUMB_LIST_ALBUMS);
        compact_list_set_trailing_click(favorite_list, favorite_album_menu);
        lv_obj_add_event_cb(favorite_screen, favorite_albums_refresh, LV_EVENT_SCREEN_LOAD_START, NULL);
        lv_obj_add_event_cb(favorite_screen, favorite_albums_deleted, LV_EVENT_DELETE, NULL);
        favorite_timer = lv_timer_create(favorite_albums_poll, 250, NULL);
        finalize_screen_navigation(favorite_screen);
    }
    nav_push(favorite_screen);
}
