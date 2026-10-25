#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "metadata_db.h"
#include "tagcache.h"

/* The top-played query does not use either sidecar store. */
void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *path, int32_t *rating, int32_t *playcount, int32_t *last_played) {
    (void)path; (void)rating; (void)playcount; (void)last_played;
    return false;
}

typedef struct {
    int id;
    int count;
    int last;
} expected_hit_t;

static int compare_hits(const void *a, const void *b) {
    const expected_hit_t *x = a;
    const expected_hit_t *y = b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    if (x->last != y->last) return x->last < y->last ? 1 : -1;
    return x->id > y->id ? 1 : x->id < y->id ? -1 : 0;
}

static void make_song(int i, char *path, cached_tags_t *tags) {
    memset(tags, 0, sizeof(*tags));
    snprintf(path, TAGCACHE_PATH_MAX, "track-%05d.flac", i);
    snprintf(tags->title, sizeof(tags->title), "Song %05d", i);
    snprintf(tags->artist, sizeof(tags->artist), "Regression");
    snprintf(tags->album, sizeof(tags->album), "Top Played");
    snprintf(tags->album_artist, sizeof(tags->album_artist), "Regression");
    tags->track_number = i + 1;
    tags->disc_number = 1;
}

static void check_query(int limit, const expected_hit_t *expected, int played, int total) {
    char **paths = NULL;
    int count = 0;
    metadata_db_load_top_played_songs(limit, &paths, &count);
    int want = played < limit ? played : limit;
    assert(count == want);
    for (int i = 0; i < want; i++) {
        char path[TAGCACHE_PATH_MAX];
        snprintf(path, sizeof(path), "track-%05d.flac", expected[i].id);
        assert(strcmp(paths[i], path) == 0);
        free(paths[i]);
    }
    free(paths);
    (void)total;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(chdir(argv[1]) == 0);

    enum { total = 4096 };
    expected_hit_t *expected = calloc(total, sizeof(*expected));
    assert(expected != NULL);
    assert(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH);
    metadata_db_begin_update();
    int played = 0;
    for (int i = 0; i < total; i++) {
        char path[TAGCACHE_PATH_MAX];
        cached_tags_t tags;
        make_song(i, path, &tags);
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        fclose(file);
        metadata_db_put(path, 1, 1, &tags);
        int count = i % 19 == 0 ? 0 : (i % 11) + 1;
        int last = (i * 7) % 13;
        tagcache_overlay_stats(path, 0, count, last);
        if (count > 0) expected[played++] = (expected_hit_t) { i, count, last };
    }
    assert(metadata_db_end_update());
    qsort(expected, played, sizeof(*expected), compare_hits);

    check_query(1, expected, played, total);
    check_query(7, expected, played, total);
    check_query(97, expected, played, total);
    check_query(total + 100, expected, played, total);

    metadata_db_close();
    free(expected);
    return 0;
}
