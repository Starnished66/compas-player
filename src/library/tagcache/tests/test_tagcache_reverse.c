#include "src/library/tagcache/core/tagcache_internal.h"
#include <assert.h>
#include "src/library/metadata/db/metadata_db_internal.h"

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation;
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
void metadata_db_copy_group(const tagcache_group_t *src, group_row_t *dst) {
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist);
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
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
    puts("tagcache reverse: ok");
    return 0;
}
