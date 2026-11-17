#include "../metadata_db_browse.c"
#include <assert.h>

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation = 1;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;
static int generation = 1, walks;
static bool fail_walk, incomplete;
void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
void tagcache_artist_primary(const char *raw, char *out, size_t size) {
    snprintf(out, size, "%s", raw);
}
int tagcache_cmp_ascii(const char *a, const char *b) { return strcasecmp(a, b); }
int32_t tagcache_generation(void) { return generation; }
int tagcache_title_order_version(void) { return 1; }
static int next_rank, fixture_count = 2;
bool tagcache_storage_current(void) { return true; }
void tagcache_query_begin(tagcache_query_t *q, int domain, const char *needle,
                         const char *artist, const char *album_artist, const char *album) {
    (void)q; (void)domain; (void)needle; (void)artist; (void)album_artist; (void)album;
    walks++; next_rank = 0;
}
bool tagcache_query_next(tagcache_query_t *q, int32_t *rank) {
    (void)q; if (next_rank >= fixture_count) return false; *rank = next_rank++; return true;
}
bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t *song) {
    if (fail_walk && rank == 1) return false;
    memset(song, 0, sizeof(*song)); song->id = rank + 1;
    song->genre = rank ? "rock" : "Rock";
    song->artist = song->album_artist = "Artist"; song->album = "Album";
    if (incomplete && rank == 1) { song->genre = ""; song->flags = FLAG_TAGS_INCOMPLETE; }
    return true;
}
bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *song) {
    (void)fields; return tagcache_song_at_title_rank(rank, song);
}
void metadata_db_copy_song(const tagcache_song_t *src, song_row_t *dst) {
    memset(dst, 0, sizeof(*dst)); dst->id = src->id;
    snprintf(dst->tags.genre, sizeof(dst->tags.genre), "%s", src->genre);
    snprintf(dst->tags.artist, sizeof(dst->tags.artist), "%s", src->artist);
    snprintf(dst->tags.album_artist, sizeof(dst->tags.album_artist), "%s", src->album_artist);
    snprintf(dst->tags.album, sizeof(dst->tags.album), "%s", src->album);
}
static bool replace_catalog(const song_row_t *song, void *user) {
    (void)song;
    int *visited = user;
    if (++*visited == 1) generation++;
    return true;
}
static void test_incomplete_retry(void) {
    generation++;
    incomplete = true;
    assert(metadata_db_get_genre_count() == 1 && !genre_cache_valid);
    assert(genre_groups_ensure(METADATA_DB_GROUP_ALBUM, "Rock", METADATA_DB_GROUP_ARTIST, NULL));
    assert(!genre_song_cache.valid && genre_song_cache.count == 1);
    group_row_t row;
    int total;
    assert(metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, "Rock",
        METADATA_DB_GROUP_ARTIST, NULL, 0, 1, &row, &total) == 1 && total == 1);
    incomplete = false;
    assert(metadata_db_get_genre_count() == 1 && genre_cache_valid);
    assert(genre_cache[0].song_count == 2);
    assert(genre_groups_ensure(METADATA_DB_GROUP_ALBUM, "Rock", METADATA_DB_GROUP_ARTIST, NULL));
    assert(genre_song_cache.valid);
}

static bool count_song(const song_row_t *song, void *user) {
    assert(song->id == 1);
    (*(int *)user)++;
    return true;
}

static void test_partial_walk(void) {
    for (int i = 0; i < 2; i++) {
        incomplete = i == 0; fail_walk = i == 1;
        int visited = 0;
        bool complete = true;
        assert(metadata_db_walk_genre_songs("Rock", 10, count_song, &visited, NULL, &complete));
        assert(visited == 1 && !complete);
    }
    incomplete = fail_walk = false;
}

int main(void) {
    assert(metadata_db_get_genre_count() == 1);
    assert(genre_cache[0].song_count == 2); /* case-insensitive genre identity */
    assert(metadata_db_get_genre_count() == 1 && walks == 1);
    generation++;
    assert(metadata_db_get_genre_count() == 1);
    assert(genre_cache_valid && genre_cache_count == 1);
    int previous = walks;
    assert(metadata_db_get_genre_count() == 1 && walks == previous);
    assert(metadata_db_get_genre_count() == 1 && genre_cache_valid);
    generation++;
    fail_walk = true;
    assert(metadata_db_get_genre_count() == 1);
    assert(genre_cache_count == 1); /* partial results remain visible */
    assert(!genre_cache_valid);
    previous = walks;
    fail_walk = false;
    assert(metadata_db_get_genre_count() == 1 && walks > previous);
    assert(genre_cache_valid && genre_cache[0].song_count == 2);
    test_incomplete_retry();
    test_partial_walk();
    fixture_count = 140;
    int visited = 0;
    assert(!metadata_db_walk_genre_songs(NULL, 140, replace_catalog, &visited, NULL, NULL));
    assert(visited > 0 && visited < 140); /* mutation between batches aborts */
    metadata_db_genre_cache_free();
    puts("Genre cache grouping, display casing, partial retry and failed walk passed");
}
