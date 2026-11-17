#include "../album_rating.h"
#include "../metadata_db_internal.h"
#include "../../../remote/remote_state.h"
#include "../../../playlists/rating_writer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation = 1;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;

void metadata_db_mutex_init(void) { pthread_mutex_init(&metadata_db_mutex, NULL); }
static const char *catalog_album_names[] = { "Alpha", "Beta", "Charlie", "Delta" };
static const char *catalog_album_artists[] = { "A", "A", "B", "C" };
static int catalog_album_count = 4;
static int32_t catalog_generation = 17;
static int fail_album_index = -1, change_generation_at = -1, change_incarnation_at = -1;
static int mutate_generation_on_group_count_call = -1;
int32_t tagcache_generation(void) { return catalog_generation; }
int tagcache_group_count(int kind) {
    assert(kind == TAGCACHE_GROUP_ALBUM);
    if (mutate_generation_on_group_count_call > 0 && --mutate_generation_on_group_count_call == 0)
        catalog_generation++;
    return catalog_album_count;
}
bool tagcache_group_at(int kind, int index, tagcache_group_t *out) {
    assert(kind == TAGCACHE_GROUP_ALBUM);
    if (!out || index < 0 || index >= catalog_album_count || index >= 4 || index == fail_album_index) return false;
    *out = (tagcache_group_t){ .name = catalog_album_names[index],
                               .album_artist = catalog_album_artists[index],
                               .song_count = 1, .first_song_id = index + 1 };
    if (index == change_generation_at) { catalog_generation++; change_generation_at = -1; }
    if (index == change_incarnation_at) { metadata_db_db_incarnation++; change_incarnation_at = -1; }
    return true;
}
void metadata_db_copy_group(const tagcache_group_t *src, group_row_t *dst) {
    snprintf(dst->name, sizeof(dst->name), "%s", src->name ? src->name : "");
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist ? src->album_artist : "");
    dst->song_count = src->song_count;
    dst->first_song_id = src->first_song_id;
}
bool tagcache_album_group_at_sorted(int sort, int rank, tagcache_group_t *out) {
    (void)sort; (void)rank; (void)out; assert(false); return false;
}
int tagcache_group_album_offset(int kind, const char *artist, const char *album, const char *album_artist) {
    (void)kind; (void)artist; (void)album; (void)album_artist; return -1;
}
bool tagcache_song_by_path(const char *path, tagcache_song_t *song) {
    (void)path; (void)song;
    return false;
}
void tagcache_set_rating(const char *path, int32_t value) { (void)path; (void)value; assert(false); }
int tagcache_set_rating_durable_begin(const char *path, int32_t value) {
    (void)path; (void)value; assert(false); return 0;
}
bool tagcache_numeric_wait_durable(int waiter, int timeout) {
    (void)waiter; (void)timeout; assert(false); return false;
}

static void test_keys(void) {
    char key[64];
    assert(!album_rating_key(NULL, "a", key, sizeof(key)));
    assert(!album_rating_key("", "a", key, sizeof(key)));
    assert(!album_rating_key("a", "", key, sizeof(key)));
    assert(!album_rating_key("a", "b", NULL, 64));
    assert(!album_rating_key("a", "b", key, 0));
    assert(!album_rating_key("a", "b", key, 11));
    assert(album_rating_key("a", "b", key, 12));
    assert(!strcmp(key, "album://a\tb"));
    assert(album_rating_key("a", "b   ", key, sizeof(key)));
    assert(!strcmp(key, "album://a\tb"));
    assert(album_rating_key("a\tb\n", "c\rd\te", key, sizeof(key)));
    assert(!strcmp(key, "album://a b \tc d e"));
}

static void write_album(const char *artist, const char *album, int stars) {
    char key[ALBUM_RATING_KEY_SIZE];
    assert(album_rating_key(artist, album, key, sizeof(key)));
    metadata_db_album_rating_set(artist, album, stars);
    assert(metadata_db_album_rating_get(artist, album) == stars);
    /* Drain the queued edit through the same ordered writer. */
    assert(rating_writer_write_now(key, stars));
}

static void test_round_trip(void) {
    write_album("Z", "High", 5);
    write_album("A", "Second", 5);
    write_album("A", "First", 5);
    write_album("A", "Low", 1);
    write_album("A", "Two", 2);
    write_album("A", "Three", 3);
    write_album("A", "Four", 4);
    assert(remote_state_set_rating("album://A\tLegacy", 1));
    write_album("Gone", "Cleared", 4);
    write_album("Gone", "Cleared", 0);
    assert(remote_state_set_rating("remote://song", 1));
    assert(remote_state_set_rating("album://malformed", 1));
    remote_state_drop();
    assert(metadata_db_album_rating_get("A", "Low") == 1);
    assert(metadata_db_album_rating_get("Gone", "Cleared") == 0);
    rated_album_t *albums;
    int count;
    metadata_db_load_rated_albums(&albums, &count);
    assert(count == 8);
    const char *names[] = {"First", "Legacy", "Second", "High", "Four", "Three", "Two", "Low"};
    const int stars[] = {5, 5, 5, 5, 4, 3, 2, 1};
    for (int i = 0; i < count; i++) {
        assert(!strcmp(albums[i].album, names[i]));
        assert(albums[i].stars == stars[i]);
    }
    metadata_db_free_rated_albums(albums, count);
}

static void test_async_write(void) {
    unsigned revision = remote_state_revision();
    metadata_db_album_rating_set("Async", "Album", 3);
    metadata_db_album_rating_set("Async", "Album", 2);
    rating_writer_flush();
    for (int i = 0; i < 2000 && remote_state_revision() == revision; i++) usleep(1000);
    assert(remote_state_revision() != revision);
    remote_state_drop();
    assert(metadata_db_album_rating_get("Async", "Album") == 2);
    revision = remote_state_revision();
    metadata_db_album_rating_set("Async", "Album", 0);
    rating_writer_flush();
    for (int i = 0; i < 2000 && remote_state_revision() == revision; i++) usleep(1000);
    assert(remote_state_revision() != revision);
    rated_album_t *albums;
    int count;
    metadata_db_load_rated_albums(&albums, &count);
    assert(count == 8);
    metadata_db_free_rated_albums(albums, count);
}

static void test_catalog_rating_order(void) {
    write_album("A", "Beta", 5);
    write_album("B", "Charlie", 5);
    write_album("A", "Alpha", 4);
    int *ranks = NULL, sections[6], count = 0;
    int32_t generation = 0;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && generation == 17);
    assert(sections[5] == 2 && sections[4] == 1 && sections[0] == 1);
    assert(ranks[0] == 1 && ranks[1] == 2 && ranks[2] == 0 && ranks[3] == 3);
    free(ranks);

    write_album("A", "Alpha", 1);
    write_album("A", "Beta", 2);
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && ranks[0] == 2 && ranks[1] == 1 && ranks[2] == 0 && ranks[3] == 3);
    free(ranks);

    write_album("B", "Charlie", 0);
    write_album("A", "Beta", 0);
    write_album("A", "Alpha", 0);
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[0] == 4);
    assert(ranks[0] == 0 && ranks[1] == 1 && ranks[2] == 2 && ranks[3] == 3);
    free(ranks);

    int bad_sections[6] = {1, 1, 1, 1, 1, 1}, bad_count = 99;
    int32_t bad_generation = -1;
    fail_album_index = 1;
    assert(!metadata_db_load_album_rating_order(&ranks, bad_sections, &bad_count, &bad_generation));
    assert(ranks == NULL && bad_count == 0);
    for (int i = 0; i < 6; i++) assert(bad_sections[i] == 0);
    fail_album_index = -1;

    change_generation_at = 0;
    assert(!metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    change_incarnation_at = 0;
    assert(!metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));

    catalog_album_count = 0;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(ranks == NULL && count == 0);
    for (int i = 0; i < 6; i++) assert(sections[i] == 0);
    catalog_album_count = 4;
}

static void test_trimmed_catalog_order(void) {
    catalog_album_names[0] = "Alpha  ";
    write_album("A", "Alpha", 4);
    int *ranks = NULL, sections[6], count;
    int32_t generation;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[4] == 1 && ranks[0] == 0);
    free(ranks);
    catalog_album_names[0] = "Alpha";
}

static void test_legacy_spaces(void) {
    assert(remote_state_set_rating("album://Legacy\tSpaced   ", 2));
    assert(remote_state_set_rating("album://Legacy\tCollision ", 1));
    assert(remote_state_set_rating("album://Legacy\tCollision", 4));
    remote_state_drop();
    int32_t rating = 0;
    assert(remote_state_get("album://Legacy\tSpaced", &rating, NULL, NULL) && rating == 2);
    assert(remote_state_get("album://Legacy\tCollision", &rating, NULL, NULL) && rating == 4);
    assert(metadata_db_album_rating_get("Legacy", "Spaced") == 4);
    assert(metadata_db_album_rating_get("Legacy", "Collision") == 2);
    assert(!remote_state_get("album://Legacy\tSpaced   ", NULL, NULL, NULL));
    assert(!remote_state_get("album://Legacy\tCollision ", NULL, NULL, NULL));
    remote_state_drop();
    assert(remote_state_get("album://Legacy\tSpaced", &rating, NULL, NULL) && rating == 2);
}

int main(void) {
    char dir[] = "/tmp/compas-album-rating-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    test_keys();
    test_round_trip();
    test_async_write();
    test_catalog_rating_order();
    test_legacy_spaces();
    test_trimmed_catalog_order();
    remote_state_drop();
    assert(unlink(".compas/remote_state.tsv") == 0);
    assert(rmdir(".compas") == 0 && chdir("/") == 0 && rmdir(dir) == 0);
    puts("album rating keys and sidecar round trip passed");
    return 0;
}
