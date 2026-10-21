#include "metadata_db.c"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_get(const char *path, int32_t *rating, int32_t *count, int32_t *played) {
    (void) path; (void) rating; (void) count; (void) played;
    return false;
}
bool remote_state_set_rating(const char *path, int32_t rating) {
    (void) path; (void) rating;
    return false;
}
void remote_state_add_play(const char *path, int32_t now) { (void) path; (void) now; }
bool remote_state_take(const char *path, int32_t *rating, int32_t *count, int32_t *played) {
    (void) path; (void) rating; (void) count; (void) played;
    return false;
}

typedef struct {
    const char *path;
    song_row_t row;
    atomic_bool done;
    bool found;
} cache_lookup_task_t;

static void *cache_lookup_thread(void *userdata) {
    cache_lookup_task_t *task = userdata;
    task->found = metadata_db_try_get_cached_song_by_path(task->path, &task->row);
    atomic_store(&task->done, true);
    return NULL;
}

static void assert_lookup_finishes_while_locked(pthread_mutex_t *mutex, const char *path,
                                                bool expected_found) {
    cache_lookup_task_t task = {.path = path};
    memset(&task.row, 0x6B, sizeof(task.row));
    atomic_init(&task.done, false);
    pthread_mutex_lock(mutex);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, cache_lookup_thread, &task) == 0);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&pause, NULL);
    bool finished = atomic_load(&task.done);
    pthread_mutex_unlock(mutex);
    assert(pthread_join(thread, NULL) == 0);
    assert(finished);
    assert(task.found == expected_found);
    if (!expected_found) {
        song_row_t expected;
        memset(&expected, 0x6B, sizeof(expected));
        assert(memcmp(&task.row, &expected, sizeof(expected)) == 0);
    }
}

static void put_song(const char *path, const char *title) {
    cached_tags_t tags = {0};
    snprintf(tags.title, sizeof(tags.title), "%s", title);
    snprintf(tags.artist, sizeof(tags.artist), "Cache Test Artist");
    tags.track_number = 1;
    tags.disc_number = 1;
    tags.release_year = 2026;
    (void) metadata_db_put(path, 1, 1, &tags);
}

int main(void) {
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH);
    metadata_db_begin_update();
    for (int i = 0; i < 33; i++) {
        char path[64], title[64];
        snprintf(path, sizeof(path), "music/cache-%02d.mp3", i);
        snprintf(title, sizeof(title), "Cache title %02d", i);
        put_song(path, title);
    }
    assert(metadata_db_end_update());

    song_row_t fetched;
    assert(metadata_db_get_song_by_path("music/cache-00.mp3", &fetched));
    song_row_t cached;
    memset(&cached, 0xA5, sizeof(cached));
    assert(metadata_db_try_get_cached_song_by_path("music/cache-00.mp3", &cached));
    assert(cached.id == fetched.id);
    assert(strcmp(cached.path, fetched.path) == 0);
    assert(strcmp(cached.tags.title, fetched.tags.title) == 0);
    strcpy(fetched.tags.title, "caller changed its copy");
    assert(metadata_db_try_get_cached_song_by_path("music/cache-00.mp3", &cached));
    assert(strcmp(cached.tags.title, "Cache title 00") == 0);

    /* Display metadata stays cached across numeric favorite and play-count
     * writes, which do not change the copied song row's tags. */
    metadata_db_song_favorite_set("music/cache-00.mp3", true);
    metadata_db_song_play_count_increment("music/cache-00.mp3");
    assert(metadata_db_try_get_cached_song_by_path("music/cache-00.mp3", &cached));
    assert(strcmp(cached.tags.title, "Cache title 00") == 0);

    /* The cache-only lookup finishes while either mutex is owned elsewhere.
     * Cache contention returns a miss; database contention has no effect. */
    assert_lookup_finishes_while_locked(&display_song_cache_mutex, "music/cache-00.mp3", false);
    assert_lookup_finishes_while_locked(&metadata_db_mutex, "music/cache-00.mp3", true);

    song_row_t unchanged;
    memset(&unchanged, 0x5A, sizeof(unchanged));
    song_row_t before = unchanged;
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-00.mp3/", &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-99.mp3", &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    char too_long[METADATA_DB_TEXT_MAX + 1];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    assert(!metadata_db_try_get_cached_song_by_path(too_long, &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    /* The cache is a 32-entry ring; touching the 33rd distinct row evicts the
     * oldest entry, while the latest row remains available. */
    for (int i = 1; i < 33; i++) {
        char path[64];
        snprintf(path, sizeof(path), "music/cache-%02d.mp3", i);
        assert(metadata_db_get_song_by_path(path, &fetched));
    }
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-00.mp3", &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    assert(metadata_db_try_get_cached_song_by_path("music/cache-32.mp3", &cached));

    /* Explicit open, tag mutation, targeted delete, scan completion, and
     * close each invalidate copied display snapshots. */
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-32.mp3", &unchanged));
    assert(metadata_db_get_song_by_path("music/cache-32.mp3", &fetched));
    metadata_db_begin_targeted_update();
    put_song("music/cache-32.mp3", "updated title");
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-32.mp3", &unchanged));
    assert(metadata_db_get_song_by_path("music/cache-32.mp3", &fetched));
    assert(metadata_db_try_get_cached_song_by_path("music/cache-32.mp3", &cached));
    assert(metadata_db_delete_target_path("music/cache-32.mp3"));
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-32.mp3", &unchanged));
    assert(metadata_db_end_update());

    /* A targeted scan that preserves a row still invalidates at successful
     * end-update. */
    metadata_db_begin_targeted_update();
    assert(metadata_db_get_song_by_path("music/cache-31.mp3", &fetched));
    assert(metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &cached));
    assert(metadata_db_end_update());
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &unchanged));

    assert(metadata_db_get_song_by_path("music/cache-31.mp3", &fetched));
    assert(metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &cached));
    assert(metadata_db_reload() == METADATA_DB_LOAD_SUCCESS_NORMAL);
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &unchanged));
    assert(metadata_db_get_song_by_path("music/cache-31.mp3", &fetched));
    assert(metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &cached));
    metadata_db_begin_update();
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    metadata_db_abort_update();
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &unchanged));
    assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    metadata_db_close();
    assert(!metadata_db_try_get_cached_song_by_path("music/cache-31.mp3", &unchanged));
    puts("metadata_db display cache spec passed");
    return 0;
}
