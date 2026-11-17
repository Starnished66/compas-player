#include "../track_metadata_worker.c"
#include <assert.h>
#include <unistd.h>

static atomic_bool entered, release_write;
void metadata_db_song_play_count_increment(const char *path) {
    assert(!strcmp(path, "queued.wav"));
    atomic_store(&entered, true);
    while (!atomic_load(&release_write)) usleep(1000);
}
static void *consume(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mutex);
    process_play_event();
    return NULL;
}
int main(void) {
    assert(track_metadata_history_ready());
    play_events[0] = strdup("queued.wav");
    play_count = 1;
    assert(!track_metadata_history_ready());
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, consume, NULL));
    while (!atomic_load(&entered)) usleep(1000);
    /* The ring is empty, but the database has not accepted the event yet. */
    assert(!track_metadata_history_ready());
    atomic_store(&release_write, true);
    pthread_join(thread, NULL);
    assert(track_metadata_history_ready());
    puts("Play-event barrier covers queued and in-flight events");
    return 0;
}
