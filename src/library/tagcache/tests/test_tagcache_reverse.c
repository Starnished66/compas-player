#include "src/library/remote/remote_state.h"
#include "src/library/tagcache/core/tagcache_internal.h"
#include <assert.h>
#include "src/library/metadata/db/metadata_db_internal.h"

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;
static int album_ratings[4];
static unsigned test_rating_revision;
static int rating_order_reads;
void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
void metadata_db_copy_song(const tagcache_song_t *src, song_row_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->id = src->id;
    snprintf(dst->path, sizeof(dst->path), "%s", src->path);
    snprintf(dst->tags.genre, sizeof(dst->tags.genre), "%s", src->genre);
    snprintf(dst->tags.artist, sizeof(dst->tags.artist), "%s", src->artist);
    snprintf(dst->tags.album, sizeof(dst->tags.album), "%s", src->album);
    snprintf(dst->tags.album_artist, sizeof(dst->tags.album_artist), "%s", src->album_artist);
}
void metadata_db_copy_group(const tagcache_group_t *src, group_row_t *dst) {
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist);
}
remote_state_rating_t *remote_state_list_prefix(const char *prefix, int *count) {
    rating_order_reads++;
    *count = 4;
    remote_state_rating_t *rows = calloc(4, sizeof(*rows));
    for (int i = 0; i < 4; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%sArtist\t%c", prefix, 'A' + i);
        rows[i].path = strdup(path);
        rows[i].rating = album_ratings[i] ? 6 - album_ratings[i] : 0;
    }
    return rows;
}
int rating_writer_effective_stars(const char *path, int stored) { (void)path; return stored; }
unsigned remote_state_revision(void) { return test_rating_revision; }

static void check_rating_order(void) {
    album_ratings[0] = 2;
    album_ratings[1] = 5;
    album_ratings[2] = 5;
    album_ratings[3] = 0;
    int *ranks = NULL, sections[6], count = 0;
    int32_t generation;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[5] == 2 && sections[2] == 1 && sections[0] == 1);
    assert(ranks[0] == 1 && ranks[1] == 2 && ranks[2] == 0 && ranks[3] == 3);
    group_row_t page[2];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 1, 2, page) == 2);
    assert(strcmp(page[0].name, "C") == 0 && strcmp(page[1].name, "A") == 0);
    free(ranks);

    album_ratings[0] = album_ratings[1] = album_ratings[2] = album_ratings[3] = 0;
    test_rating_revision++;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[0] == 4);
    assert(ranks[0] == 0 && ranks[1] == 1 && ranks[2] == 2 && ranks[3] == 3);
    free(ranks);
}

static void check_rating_page_cache(void) {
    group_row_t page[2];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 0, 2, page) == 2);
    int reads = rating_order_reads;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 2, 2, page) == 2);
    assert(rating_order_reads == reads);
    album_ratings[3] = 5;
    test_rating_revision++;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 0, 2, page) == 2);
    assert(!strcmp(page[0].name, "D") && rating_order_reads == reads + 1);
    metadata_db_db_incarnation++;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 2, 2, page) == 2);
    assert(rating_order_reads == reads + 2);
}


static void check_orders(int known) {
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    assert(count == 4);
    for (int sort = 0; sort < 4; sort++) {
        for (int row = 0; row < count; row++) {
            tagcache_group_t forward, reverse;
            int expected = sort == 2 ? (row < known ? known - 1 - row : row) : count - 1 - row;
            assert(tagcache_album_group_at_sorted(sort, expected, &forward));
            assert(tagcache_album_group_at_sorted(sort | TAGCACHE_ALBUM_SORT_REVERSE, row, &reverse));
            assert(strcmp(forward.name, reverse.name) == 0);
            int canonical = tagcache_group_index(TAGCACHE_GROUP_ALBUM, reverse.name, reverse.album_artist);
            int inverse = -1;
            assert(tagcache_album_group_sorted_rank(sort | TAGCACHE_ALBUM_SORT_REVERSE, canonical, &inverse));
            assert(inverse == row);
            metadata_db_album_sort_t order = (metadata_db_album_sort_t)(sort | TAGCACHE_ALBUM_SORT_REVERSE);
            group_row_t page[2];
            int got = metadata_db_get_albums_for_group_sorted(METADATA_DB_GROUP_ARTIST,
                                                               "Artist", order, row, 2, page);
            assert(got == (row == count - 1 ? 1 : 2));
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_get_album_for_group_sorted_offset(METADATA_DB_GROUP_ARTIST,
                      "Artist", order, reverse.name, reverse.album_artist) == row);
            assert(metadata_db_get_albums_for_group_sorted(METADATA_DB_GROUP_ALBUM_ARTIST,
                      "Artist", order, row, 2, page) == (row == count - 1 ? 1 : 2));
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_get_album_for_group_sorted_offset(METADATA_DB_GROUP_ALBUM_ARTIST,
                      "Artist", order, reverse.name, reverse.album_artist) == row);
            assert(metadata_db_get_albums_page_sorted(NULL, order, row, 1, page) == 1);
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_album_canonical_to_sorted(order, canonical) == row);
            assert(tagcache_album_group_sorted_offset(sort | TAGCACHE_ALBUM_SORT_REVERSE,
                                                       reverse.name, reverse.album_artist) == row);
        }
    }
}

typedef struct {
    const char *path;
    char title[64];
} committed_read_t;

static void *read_committed_title(void *arg) {
    committed_read_t *read = arg;
    tagcache_song_t song;
    if (tagcache_song_by_path(read->path, &song))
        snprintf(read->title, sizeof(read->title), "%s", song.title);
    return NULL;
}

int main(void) {
    char dir[] = "/tmp/compas-sort-XXXXXX";
    assert(mkdtemp(dir));
    assert(tagcache_open(dir));
    tagcache_begin_update_with_lock(NULL, NULL);
    const int years[] = {2000, -1, 1990, -1};
    for (int i = 0; i < 4; i++) {
        char path[64], album[16];
        snprintf(path, sizeof(path), "/music/%d.mp3", i);
        snprintf(album, sizeof(album), "%c", 'A' + i);
        tagcache_upsert_year_changed(path, 100 + i, 1000, "Song", "Artist", album,
                                     "Artist", "Genre", 1, 1, years[i], NULL);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    check_rating_order();
    check_rating_page_cache();
    check_orders(2);
    tagcache_close();
    assert(tagcache_open(dir));
    check_orders(2);
    /* Publishing an all-unknown year index must invalidate the cached boundary. */
    tagcache_begin_update_with_lock(NULL, NULL);
    for (int i = 0; i < 4; i++) {
        char path[64], album[16];
        snprintf(path, sizeof(path), "/music/%d.mp3", i);
        snprintf(album, sizeof(album), "%c", 'A' + i);
        tagcache_upsert_year_changed(path, 200 + i, 1000, "Song", "Artist", album,
                                     "Artist", "Genre", 1, 1, -1, NULL);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    check_orders(0);
    /* The worker's staging reader is thread-local. A concurrent UI query
     * must keep seeing the old committed generation until publication. */
    tagcache_begin_update_with_lock(NULL, NULL);
    tagcache_upsert_year_changed("/music/0.mp3", 300, 1000, "Updated Song", "Artist",
                                 "A", "Artist", "Genre", 1, 1, 2026, NULL);
    committed_read_t concurrent_read = { .path = "/music/0.mp3" };
    pthread_t reader;
    assert(pthread_create(&reader, NULL, read_committed_title, &concurrent_read) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(strcmp(concurrent_read.title, "Song") == 0);
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_song_t published_song;
    assert(tagcache_song_by_path("/music/0.mp3", &published_song));
    assert(strcmp(published_song.title, "Updated Song") == 0);
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
    puts("tagcache reverse: ok");
    return 0;
}
