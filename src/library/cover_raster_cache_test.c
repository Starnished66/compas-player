#define _POSIX_C_SOURCE 200809L
#include "cover_raster_cache.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static cover_raster_key_t key(unsigned ino, int size) {
    cover_raster_key_t k = {0};
    k.file.st_dev = 1; k.file.st_ino = ino; k.file.st_size = 42;
    k.file.st_mtim.tv_sec = 10; k.file.st_ctim.tv_sec = 11;
    k.width = k.height = size; return k;
}
int main(void) {
    cover_raster_key_t a = key(1, 480), b = key(2, 480), c = key(3, 480);
    uint16_t *pixels = calloc(600 * 600, 2); assert(pixels); pixels[0] = 1234;
    cover_raster_cache_put(&a, pixels); cover_raster_cache_put(&b, pixels);
    pixels[0] = 42;
    uint16_t *copy = cover_raster_cache_get(&a); assert(copy && copy[0] == 1234);
    copy[0] = 66; free(copy);
    cover_raster_key_t stale = a; stale.file.st_mtim.tv_nsec = 1;
    assert(!cover_raster_cache_get(&stale));
    stale = a; stale.file.st_ctim.tv_nsec = 1; assert(!cover_raster_cache_get(&stale));
    stale = a; stale.width = 320; assert(!cover_raster_cache_get(&stale));
    cover_raster_cache_put(&c, pixels);
    assert(!cover_raster_cache_get(&b)); // A was just used, B is LRU
    copy = cover_raster_cache_get(&a); assert(copy && copy[0] == 1234); free(copy);
    assert(cover_raster_cache_bytes() == 480u * 480u * 4u);
    cover_raster_key_t large = key(4, 600); cover_raster_cache_put(&large, pixels);
    assert(cover_raster_cache_bytes() == 600u * 600u * 2u);
    assert(cover_raster_cache_bytes() <= 1024u * 1024u);
    stale = key(5, 1000); cover_raster_cache_put(&stale, pixels);
    assert(!cover_raster_cache_get(&stale));
    cover_raster_cache_clear(); assert(cover_raster_cache_bytes() == 0);
    free(pixels); puts("Cover raster ownership, invalidation, LRU and budget tests passed");
}
