#include "duration_prune.h"
#include "duration_cache.h"
#include "../db/metadata_db.h"
#include "../../../core/process/worker_thread.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct { uint64_t * keys; size_t count, cap; } key_set_t;

static bool collect(const char * path, int32_t mtime, int32_t size, void * user) {
    key_set_t * set = user;
    if (set->count == set->cap) {
        size_t cap = set->cap ? set->cap * 2 : 4096;
        uint64_t * grown = realloc(set->keys, cap * sizeof(*grown));
        if (!grown) return false;
        set->keys = grown;
        set->cap = cap;
    }
    set->keys[set->count++] = duration_cache_key(path, mtime, size);
    return true;
}

static int cmp_key(const void * a, const void * b) {
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return x < y ? -1 : x > y;
}

static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static bool running, again; /* both under request_lock: a request can never fall between a walk's last check and its exit */

#define DURATION_PRUNE_STACK (128u * 1024u)

static void * prune_worker(void * arg) {
    (void) arg;
    worker_lower_priority(WORKER_NICE_IDLE);
    for (;;) {
        pthread_mutex_lock(&request_lock);
        again = false;
        pthread_mutex_unlock(&request_lock);
        /* Taken before the walk: a card change meanwhile makes the prune a no-op. */
        uint32_t epoch = duration_cache_epoch();
        key_set_t set = { 0 };
        /* A walk cut short (card removed, generation changed) keeps everything. */
        if (metadata_db_walk_song_identities(collect, &set) && set.count > 0) {
            qsort(set.keys, set.count, sizeof(*set.keys), cmp_key);
            (void) duration_cache_prune(epoch, set.keys, set.count);
        }
        free(set.keys);
        pthread_mutex_lock(&request_lock);
        if (!again) { running = false; pthread_mutex_unlock(&request_lock); return NULL; }
        pthread_mutex_unlock(&request_lock);
    }
}

void duration_prune_request(void) {
    pthread_mutex_lock(&request_lock);
    if (running) { again = true; pthread_mutex_unlock(&request_lock); return; }
    running = true;
    pthread_mutex_unlock(&request_lock);
    pthread_t thread;
    if (worker_thread_create(&thread, DURATION_PRUNE_STACK, prune_worker, NULL) == 0) { pthread_detach(thread); return; }
    pthread_mutex_lock(&request_lock);
    running = false;
    pthread_mutex_unlock(&request_lock);
}
