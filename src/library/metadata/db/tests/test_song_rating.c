#include "../song_rating.h"
#include "../metadata_db_internal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation;
static int32_t order_generation = 42;
static atomic_bool yield_probe, released, acquired;
static bool probe_test, change_generation;
static int probe_id;
static int missing_path_id;
static bool change_incarnation;
static atomic_uint stats_revision;
static int stats_change;
static bool stats_changed, persistent_staleness;
static int stale_batches, held_discards;
uint32_t tagcache_stats_revision(void) { return atomic_load(&stats_revision); }
int32_t tagcache_title_rank_of_path(const char *path) { (void)path; return 0; }
/* Writers need the guard, so churn lands only where it is really released;
 * held_discards stands in for the numeric worker, which never takes it. */
static __thread int db_depth;
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    int result = __real_pthread_mutex_lock(mutex);
    if (mutex == &metadata_db_mutex && !result) db_depth++;
    return result;
}
int __real_pthread_mutex_trylock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_trylock(pthread_mutex_t *mutex) {
    int result = __real_pthread_mutex_trylock(mutex);
    if (mutex == &metadata_db_mutex && !result) db_depth++;
    return result;
}
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex == &metadata_db_mutex) db_depth--;
    if (mutex == &metadata_db_mutex && persistent_staleness && atomic_exchange(&yield_probe, false) &&
        (db_depth == 0 || held_discards-- > 0)) {
        atomic_fetch_add(&stats_revision, 1);
        stale_batches++;
    }
    int result = __real_pthread_mutex_unlock(mutex);
    if (mutex == &metadata_db_mutex && atomic_exchange(&yield_probe, false)) {
        atomic_store(&released, true);
        while (!atomic_load(&acquired)) sched_yield();
    }
    return result;
}
static void *between_batches(void *unused) {
    (void)unused;
    while (!atomic_load(&released)) sched_yield();
    pthread_mutex_lock(&metadata_db_mutex);
    if (change_generation) order_generation++;
    if (change_incarnation) metadata_db_db_incarnation++;
    if (stats_change) {
        stats_changed = true;
        atomic_fetch_add(&stats_revision, 2);
    }
    pthread_mutex_unlock(&metadata_db_mutex);
    atomic_store(&acquired, true);
    return NULL;
}
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
    if ((fields & TAGCACHE_FIELD_PATH) && id == missing_path_id) return false;
    if (probe_test && id == probe_id && !atomic_load(&released)) atomic_store(&yield_probe, true);
    if (persistent_staleness && id == 64) atomic_store(&yield_probe, true);
    *out = (tagcache_song_t) {
        .id = id,
        .rating = slots[id - 1].rating,
        .playcount = id % 3 + 1,
        .last_played = id % 5 + 1,
        .path = fields & TAGCACHE_FIELD_PATH ? slots[id - 1].path : NULL,
    };
    if (stats_change == 1) out->rating = (stats_changed ? id == 200 : id == 1) ? 1 : 0;
    if (stats_change == 2) {
        out->playcount = id == 1 ? 10 + stats_changed : id == 200 ? 9 + stats_changed : 0;
        out->last_played = id == 200 ? 20 : 10;
    }
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

bool tagcache_song_by_id(int32_t id, tagcache_song_t *out) {
    return tagcache_song_fields_by_id(id, TAGCACHE_FIELD_PATH, out);
}

static void test_batch_holders(void) {
    slot_t rows[260];
    char names[260][16];
    for (int i = 0; i < 260; i++) {
        snprintf(names[i], sizeof(names[i]), "song%03d", i);
        rows[i] = (slot_t) {names[i], 1};
    }
    slots = rows;
    slot_count = 260;
    alarm(20);
    for (int kind = 0; kind < 4; kind++) {
        for (int change = 0; change < 9; change++) {
            stats_change = change == 4 ? 1 : change == 5 ? 2 : 0;
            stats_changed = false;
            /* 6: churn on every release; 7: plus one discard while held (reruns);
             * 8: two discards while held (fails cleanly, never mixed). */
            persistent_staleness = change >= 6;
            held_discards = change - 6;
            bool fails = change == 8;
            stale_batches = 0;
            change_generation = change == 1 || change == 2;
            change_incarnation = change == 3;
            probe_id = change == 2 ? 260 : 64;
            atomic_store(&released, false);
            atomic_store(&acquired, false);
            probe_test = true;
            pthread_t holder;
            if (!persistent_staleness) assert(!pthread_create(&holder, NULL, between_batches, NULL));
            char **paths = NULL;
            int count = -1;
            if (kind == 0) metadata_db_load_favorite_songs(&paths, &count, NULL);
            if (kind == 1) metadata_db_load_top_played_songs(stats_change == 2 ? 1 : 20, &paths, &count);
            if (kind == 2) metadata_db_load_recently_played_songs(20, &paths, &count);
            if (kind == 3) {
                song_row_t page[20];
                int total = -1;
                count = metadata_db_get_recently_played_page(0, 20, page, NULL, &total);
                assert(fails ? count == -1 && total == 0 : count == 20 && total == 260);
                if (change < 4) {
                    for (int i = 0; i < count; i++) assert(!strcmp(page[i].path, names[3 + 5 * i]));
                }
            } else {
                int expected = kind == 0 ? (stats_change == 1 ? 1 : 260) :
                               kind == 1 && stats_change == 2 ? 1 : 20;
                assert(fails ? count == 0 && !paths : count == expected);
                if (kind == 0 && stats_change == 1) assert(!strcmp(paths[0], "song199"));
                if (kind == 1 && stats_change == 2) assert(!strcmp(paths[0], "song000"));
                for (int i = 0; i < count; i++) {
                    if (change < 4) {
                        int id = kind == 0 ? i + 1 : kind == 2 ? 4 + 5 * i :
                                 i < 17 ? 14 + 15 * i : 8 + 15 * (i - 17);
                        assert(!strcmp(paths[i], names[id - 1]));
                    }
                    free(paths[i]);
                }
                free(paths);
            }
            /* At least three batched tries go stale, then one per held discard. */
            if (persistent_staleness) assert(stale_batches >= 3 + change - 6);
            else {
                pthread_join(holder, NULL);
                assert(atomic_load(&acquired));
            }
            probe_test = false;
        }
    }
    persistent_staleness = false;
    stats_change = 0;
    alarm(0);
    order_generation = 42;
}

static void test_played_results(void) {
    slot_t rows[620];
    char names[620][16];
    for (int i = 0; i < 620; i++) {
        snprintf(names[i], sizeof(names[i]), "%d", i + 1);
        rows[i] = (slot_t) {names[i], 1};
    }
    slots = rows;
    slot_count = 620;
    for (int recent = 0; recent < 2; recent++) {
        char **paths = NULL;
        int count;
        if (recent) metadata_db_load_recently_played_songs(700, &paths, &count);
        else metadata_db_load_top_played_songs(700, &paths, &count);
        assert(count == (recent ? 500 : 620));
        assert(atoi(paths[0]) == (recent ? 4 : 14));
        char **limited;
        int limited_count;
        if (recent) metadata_db_load_recently_played_songs(20, &limited, &limited_count);
        else metadata_db_load_top_played_songs(20, &limited, &limited_count);
        assert(limited_count == 20);
        for (int i = 0; i < limited_count; i++) {
            assert(!strcmp(limited[i], paths[i]));
            free(limited[i]);
        }
        free(limited);
        for (int i = 1; i < count; i++) {
            int a = atoi(paths[i - 1]), b = atoi(paths[i]);
            assert(recent || a % 3 >= b % 3);
            if (recent || a % 3 == b % 3) {
                assert(a % 5 >= b % 5);
                if (a % 5 == b % 5) assert(a < b);
            }
        }
        if (recent) {
            song_row_t page[7];
            int64_t stamps[7];
            int total;
            assert(metadata_db_get_recently_played_page(493, 7, page, stamps, &total) == 7);
            assert(total == 500);
            for (int i = 0; i < 7; i++) {
                assert(!strcmp(page[i].path, paths[493 + i]));
                assert(stamps[i] == atoi(page[i].path) % 5 + 1);
            }
            assert(metadata_db_get_recently_played_page(500, 7, page, stamps, &total) == 0);
            assert(total == 500);
        }
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths);
    }
    missing_path_id = 14;
    char **paths;
    int count;
    metadata_db_load_top_played_songs(20, &paths, &count);
    assert(!paths && count == 0);
    metadata_db_load_favorite_songs(&paths, &count, NULL);
    assert(!paths && count == 0);
    missing_path_id = 4;
    song_row_t page[2];
    int total = 0;
    assert(metadata_db_get_recently_played_page(0, 2, page, NULL, &total) == -1);
    assert(total == 500);
    missing_path_id = 0;
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
    test_batch_holders();
    test_played_results();
    puts("song rating tests passed");
    return 0;
}
