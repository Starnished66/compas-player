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
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;

void metadata_db_mutex_init(void) { pthread_mutex_init(&metadata_db_mutex, NULL); }
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
    for (int i = 0; i < 2000 && remote_state_revision() == revision; i++) usleep(1000);
    assert(remote_state_revision() != revision);
    remote_state_drop();
    assert(metadata_db_album_rating_get("Async", "Album") == 2);
    revision = remote_state_revision();
    metadata_db_album_rating_set("Async", "Album", 0);
    for (int i = 0; i < 2000 && remote_state_revision() == revision; i++) usleep(1000);
    assert(remote_state_revision() != revision);
    rated_album_t *albums;
    int count;
    metadata_db_load_rated_albums(&albums, &count);
    assert(count == 8);
    metadata_db_free_rated_albums(albums, count);
}

int main(void) {
    char dir[] = "/tmp/compas-album-rating-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    test_keys();
    test_round_trip();
    test_async_write();
    remote_state_drop();
    assert(unlink(".compas/remote_state.tsv") == 0);
    assert(rmdir(".compas") == 0 && chdir("/") == 0 && rmdir(dir) == 0);
    puts("album rating keys and sidecar round trip passed");
    return 0;
}
