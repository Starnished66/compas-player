#include "../song_rating.h"
#include "../metadata_db_internal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool metadata_db_db_ready = true;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;

void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

bool metadata_db_query_try_lock(void) {
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init);
    if (pthread_mutex_trylock(&metadata_db_mutex)) return false;
    metadata_query_depth++;
    return true;
}

void metadata_db_query_unlock(void) {
    metadata_query_depth--;
    pthread_mutex_unlock(&metadata_db_mutex);
}

typedef struct {
    const char *path;
    int32_t rating;
} slot_t;

static const slot_t *slots;
static int slot_count;

int32_t tagcache_slot_count(void) { return slot_count; }

bool tagcache_song_fields_by_id(int32_t id, unsigned fields, tagcache_song_t *out) {
    if (id < 1 || id > slot_count) return false;
    *out = (tagcache_song_t) {
        .id = id,
        .rating = slots[id - 1].rating,
        .path = fields & TAGCACHE_FIELD_PATH ? slots[id - 1].path : NULL,
    };
    return true;
}

int tagcache_cmp_ascii(const char *a, const char *b) { return strcmp(a, b); }

static void check_favorites(const slot_t *rows, int n, const char *const *expected, int count) {
    slots = rows;
    slot_count = n;
    char **paths = NULL;
    int actual = -1;
    metadata_db_load_favorite_songs(&paths, &actual, NULL);
    assert(actual == count);
    for (int i = 0; i < actual; i++) {
        assert(strcmp(paths[i], expected[i]) == 0);
        free(paths[i]);
    }
    free(paths);
}

static void test_parallel_stars(void) {
    const slot_t rows[] = {{"/low", 5}, {"/high", 1}, {"/mid", 3}, {"/two", 4}};
    slots = rows;
    slot_count = 4;
    char **paths = NULL;
    int *stars = NULL, count = 0;
    metadata_db_load_favorite_songs(&paths, &count, &stars);
    assert(count == 2 && stars);
    assert(stars[0] == 5 && stars[1] == 3);
    assert(!strcmp(paths[0], "/high") && !strcmp(paths[1], "/mid"));
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    free(stars);
    slot_count = 0;
    metadata_db_load_favorite_songs(&paths, &count, &stars);
    assert(!paths && !stars && !count);
}

static void test_favorites(void) {
    const slot_t mixed[] = {
        {"/a-low", 5}, {"/z-high", 1}, {"/unrated", 0},
        {"/z-mid", 2}, {"/a-high", 1}, {"/z-low", 5}, {"/a-mid", 2},
    };
    const char *expected[] = {
        "/a-high", "/z-high", "/a-mid", "/z-mid",
    };
    check_favorites(mixed, 7, expected, 4);
    const slot_t legacy[] = {
        {"/z-favorite", 1}, {"/a-unrated", 0}, {"/a-favorite", 1}, {"/z-unrated", 0},
    };
    const char *legacy_expected[] = {"/a-favorite", "/z-favorite"};
    check_favorites(legacy, 4, legacy_expected, 2);
}

int32_t tagcache_live_count(void) { return slot_count; }
static int32_t order_generation = 42;
static int missing_rank = -1, change_rank = -1;
int32_t tagcache_generation(void) { return order_generation; }
static bool title_ranks_indexed = true;
bool tagcache_title_ranks_indexed(void) { return title_ranks_indexed; }
bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    if (rank == missing_rank) return false;
    if (rank == change_rank) order_generation++;
    return tagcache_song_fields_by_id(rank + 1, fields, out);
}

static void test_rating_order(void) {
    const slot_t rows[] = {{"a",0},{"b",1},{"c",3},{"d",0},{"e",5},{"f",2},{"g",1}};
    const int expected[] = {1,6,5,2,4,0,3}, buckets[] = {2,1,0,1,1,2};
    slots = rows;
    slot_count = 7;
    int *ranks, sections[6], count;
    int32_t generation;
    assert(metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(count == 7 && generation == 42);
    assert(!memcmp(ranks, expected, sizeof(expected)));
    assert(!memcmp(sections, buckets, sizeof(buckets)));
    free(ranks);
    slot_count = 0;
    assert(metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(!ranks && !count);
    for (int i = 0; i < 6; i++) assert(!sections[i]);
    metadata_db_db_ready = false;
    assert(!metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(!ranks && !count);
    metadata_db_db_ready = true;
    slot_count = 7;
    title_ranks_indexed = false;
    assert(!metadata_db_try_rating_order_available());
    assert(!metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(!ranks && !count);
    title_ranks_indexed = true;
    assert(metadata_db_try_rating_order_available());
    slot_count = 0;
}

static void test_rating_order_pages(void) {
    slot_t rows[260];
    for (int i = 0; i < 260; i++) rows[i] = (slot_t){"song", i % 6};
    slots = rows;
    slot_count = 260;
    int *ranks, sections[6], count;
    int32_t generation;
    assert(metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(count == 260);
    for (int i = 1; i < count; i++) {
        int previous = song_rating_stars(rows[ranks[i-1]].rating);
        int current = song_rating_stars(rows[ranks[i]].rating);
        assert(previous >= current);
        if (previous == current) assert(ranks[i-1] < ranks[i]);
    }
    free(ranks);
    missing_rank = 140;
    assert(!metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(!ranks && !count);
    for (int i = 0; i < 6; i++) assert(!sections[i]);
    missing_rank = -1;
    change_rank = 127;
    assert(!metadata_db_load_rating_order(&ranks, sections, &count, &generation));
    assert(!ranks && !count);
    change_rank = -1;
    order_generation = 42;
}

bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t *out) {
    return tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_PATH, out);
}
void metadata_db_copy_song(const tagcache_song_t *song, song_row_t *row) {
    memset(row, 0, sizeof(*row));
    snprintf(row->path, sizeof(row->path), "%s", song->path);
}
static void test_rank_page(void) {
    const slot_t rows[] = {{"first", 1}, {"second", 0}, {"third", 3}};
    slots = rows;
    slot_count = 3;
    int ranks[] = {2,0,1};
    song_row_t page[3];
    assert(metadata_db_get_songs_at_title_ranks(ranks, 3, page) == 3);
    assert(!strcmp(page[0].path, "third") && !strcmp(page[1].path, "first"));
    assert(!strcmp(page[2].path, "second"));
    missing_rank = 0;
    assert(!metadata_db_get_songs_at_title_ranks(ranks, 3, page));
    missing_rank = -1;
    assert(!metadata_db_get_songs_at_title_ranks(ranks, 0, page));
}

int main(void) {
    for (int s = 0; s <= 5; s++) {
        assert(song_rating_stars(song_rating_stored(s)) == s);
        assert(song_rating_stored(song_rating_stars(s)) == s);
        assert((song_rating_stored(s) != 0) == (s != 0));
    }
    assert(song_rating_stars(0) == 0);
    assert(song_rating_stars(1) == 5);
    assert(song_rating_stars(7) == 1);
    assert(song_rating_stored(9) == 1);
    assert(song_rating_stars(INT32_MIN) == 0);
    assert(song_rating_stars(INT32_MAX) == 1);
    assert(song_rating_stored(INT_MIN) == 0);
    assert(song_rating_stored(INT_MAX) == 1);
    test_rating_order();
    test_rating_order_pages();
    test_rank_page();
    test_favorites();
    test_parallel_stars();
    puts("song rating tests passed");
    return 0;
}
