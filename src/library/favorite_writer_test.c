/* Host tests for favorite_writer.c ordering: a synchronous (Remote Control)
 * write and the player's debounced heart taps on one path land in request
 * order, including while an earlier write is still in flight. The metadata
 * store is a fake whose writes take long enough to overlap. */
#include "favorite_writer.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool stored_value;
static int writes;
static bool writing;

static void store(bool value) {
    pthread_mutex_lock(&store_mutex);
    writing = true;
    pthread_mutex_unlock(&store_mutex);
    usleep(300 * 1000);
    pthread_mutex_lock(&store_mutex);
    stored_value = value;
    writes++;
    writing = false;
    pthread_mutex_unlock(&store_mutex);
}

void metadata_db_song_favorite_set(const char * path, bool is_favorite) { (void) path; store(is_favorite); }
bool metadata_db_song_favorite_set_durable(const char * path, bool is_favorite) {
    (void) path;
    store(is_favorite);
    return true;
}
bool metadata_db_song_favorite_is_set(const char * path) {
    (void) path;
    pthread_mutex_lock(&store_mutex);
    bool value = stored_value;
    pthread_mutex_unlock(&store_mutex);
    return value;
}

static bool is_writing(void) {
    pthread_mutex_lock(&store_mutex);
    bool value = writing;
    pthread_mutex_unlock(&store_mutex);
    return value;
}

static void wait_idle(void) {
    usleep(900 * 1000);
    while (is_writing()) usleep(10 * 1000);
}

typedef struct { bool value; bool result; } sync_call_t;
static void * sync_write(void * arg) {
    sync_call_t * call = arg;
    call->result = favorite_writer_write_now("/music/a.flac", call->value);
    return NULL;
}

static void test_sync_after_inflight_tap(void) {
    /* Tap (true) is being written when the remote asks for false: the
     * remote write waits and lands last. */
    writes = 0;
    favorite_writer_submit("/music/a.flac", true);
    while (!is_writing()) usleep(5 * 1000);
    sync_call_t call = { false, false };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, sync_write, &call) == 0);
    pthread_join(thread, NULL);
    assert(call.result && writes == 2 && !stored_value);
    assert(!favorite_writer_is_set("/music/a.flac"));
}

static void test_newer_tap_wins_over_waiting_sync(void) {
    /* Tap A in flight, remote B waiting, then tap C: C must be written after
     * B (here B is skipped as superseded), never B after C. */
    wait_idle();
    writes = 0;
    favorite_writer_submit("/music/a.flac", true);
    while (!is_writing()) usleep(5 * 1000);
    sync_call_t call = { false, false };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, sync_write, &call) == 0);
    usleep(50 * 1000);
    favorite_writer_submit("/music/a.flac", true);
    assert(favorite_writer_is_set("/music/a.flac"));
    pthread_join(thread, NULL);
    assert(call.result);
    wait_idle();
    assert(stored_value && favorite_writer_is_set("/music/a.flac"));
}

static void test_sync_supersedes_pending_tap(void) {
    /* A tap still inside its debounce window is replaced by a later remote write. */
    wait_idle();
    writes = 0;
    favorite_writer_submit("/music/a.flac", true);
    assert(favorite_writer_write_now("/music/a.flac", false));
    wait_idle();
    assert(writes == 1 && !stored_value);
}

int main(void) {
    favorite_writer_start();
    test_sync_after_inflight_tap();
    test_newer_tap_wins_over_waiting_sync();
    test_sync_supersedes_pending_tap();
    puts("favorite writer ordering: PASS");
    return 0;
}
