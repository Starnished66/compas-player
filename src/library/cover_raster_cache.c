#include "cover_raster_cache.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_BUDGET (1024u * 1024u)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    cover_raster_key_t key;
    uint16_t *pixels;
    size_t bytes;
    uint64_t used;
} entries[2];
static uint64_t serial;

static size_t raster_bytes(const cover_raster_key_t *key) {
    if (!key || key->width <= 0 || key->height <= 0 ||
        (size_t) key->width > CACHE_BUDGET / 2 / (size_t) key->height) return 0;
    return (size_t) key->width * (size_t) key->height * 2;
}
static bool equal(const cover_raster_key_t *a, const cover_raster_key_t *b) {
    return a->width == b->width && a->height == b->height &&
        a->file.st_dev == b->file.st_dev && a->file.st_ino == b->file.st_ino &&
        a->file.st_size == b->file.st_size &&
        a->file.st_mtim.tv_sec == b->file.st_mtim.tv_sec &&
        a->file.st_mtim.tv_nsec == b->file.st_mtim.tv_nsec &&
        a->file.st_ctim.tv_sec == b->file.st_ctim.tv_sec &&
        a->file.st_ctim.tv_nsec == b->file.st_ctim.tv_nsec;
}
static void drop(unsigned i) {
    free(entries[i].pixels);
    memset(&entries[i], 0, sizeof(entries[i]));
}
uint16_t *cover_raster_cache_get(const cover_raster_key_t *key) {
    if (!raster_bytes(key)) return NULL;
    uint16_t *copy = NULL;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 2; ++i) {
        if (entries[i].pixels && equal(key, &entries[i].key)) {
            copy = malloc(entries[i].bytes);
            if (copy) {
                memcpy(copy, entries[i].pixels, entries[i].bytes);
                entries[i].used = ++serial;
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return copy;
}
void cover_raster_cache_put(const cover_raster_key_t *key, const uint16_t *pixels) {
    size_t bytes = raster_bytes(key);
    if (!bytes || !pixels) return;
    pthread_mutex_lock(&lock);
    unsigned victim = entries[0].used <= entries[1].used ? 0 : 1;
    for (unsigned i = 0; i < 2; ++i)
        if (entries[i].pixels && equal(key, &entries[i].key)) victim = i;
    /* Free before allocation so retained cache storage never exceeds budget. */
    drop(victim);
    if (entries[1 - victim].bytes > CACHE_BUDGET - bytes) drop(1 - victim);
    uint16_t *copy = malloc(bytes);
    if (copy) {
        memcpy(copy, pixels, bytes);
        entries[victim].key = *key;
        entries[victim].pixels = copy;
        entries[victim].bytes = bytes;
        entries[victim].used = ++serial;
    }
    pthread_mutex_unlock(&lock);
}
void cover_raster_cache_clear(void) {
    pthread_mutex_lock(&lock);
    drop(0); drop(1);
    pthread_mutex_unlock(&lock);
}
size_t cover_raster_cache_bytes(void) {
    pthread_mutex_lock(&lock);
    size_t bytes = entries[0].bytes + entries[1].bytes;
    pthread_mutex_unlock(&lock);
    return bytes;
}
