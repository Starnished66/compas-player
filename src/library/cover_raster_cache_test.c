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
    stale = a; stale.file.st_dev++; bool stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    stale = a; stale.file.st_ino += 10; stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    stale = a; stale.file.st_size++; stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    stale = a; stale.file.st_mtim.tv_sec++; stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    stale = a; stale.file.st_ctim.tv_sec++; stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    stale = a; stale.height = 0; stale_touch = cover_raster_cache_touch(&stale); assert(!stale_touch);
    cover_raster_cache_put(&c, pixels);
    assert(!cover_raster_cache_get(&b)); // A was just used, B is LRU
    copy = cover_raster_cache_get(&a); assert(copy && copy[0] == 1234); free(copy);
    assert(cover_raster_cache_bytes() == 480u * 480u * 4u);

    /* The warm probe itself updates LRU without allocating or copying pixels. */
    cover_raster_cache_clear();
    cover_raster_cache_put(&a, pixels); cover_raster_cache_put(&b, pixels);
    bool touched = cover_raster_cache_touch(&a); assert(touched);
    cover_raster_cache_put(&c, pixels);
    touched = cover_raster_cache_touch(&b); assert(!touched);
    touched = cover_raster_cache_touch(&a); assert(touched);

    /* Measure the production 350x350 path: get/free copies 245,000 bytes;
     * touch reports zero cache allocations and zero copied bytes. */
    cover_raster_cache_clear();
    cover_raster_key_t player_size = key(10, 350);
    cover_raster_cache_put(&player_size, pixels);
    cover_raster_cache_test_stats_reset();
    copy = cover_raster_cache_get(&player_size); assert(copy); free(copy);
    cover_raster_cache_test_stats_t get_stats = cover_raster_cache_test_stats();
    assert(get_stats.malloc_calls == 1 && get_stats.malloc_bytes == 350u * 350u * 2u);
    assert(get_stats.memcpy_calls == 1 && get_stats.memcpy_bytes == 350u * 350u * 2u);
    cover_raster_cache_test_stats_reset();
    touched = cover_raster_cache_touch(&player_size); assert(touched);
    cover_raster_cache_test_stats_t touch_stats = cover_raster_cache_test_stats();
    assert(touch_stats.malloc_calls == 0 && touch_stats.malloc_bytes == 0);
    assert(touch_stats.memcpy_calls == 0 && touch_stats.memcpy_bytes == 0);
    printf("350x350 probe: get malloc=%zuB memcpy=%zuB; touch malloc=%zuB memcpy=%zuB\n",
           get_stats.malloc_bytes, get_stats.memcpy_bytes,
           touch_stats.malloc_bytes, touch_stats.memcpy_bytes);

    cover_raster_cache_clear();
    cover_raster_key_t large = key(4, 600); cover_raster_cache_put(&large, pixels);
    assert(cover_raster_cache_bytes() == 600u * 600u * 2u);
    assert(cover_raster_cache_bytes() <= 1024u * 1024u);
    stale = key(5, 1000); cover_raster_cache_put(&stale, pixels);
    assert(!cover_raster_cache_get(&stale));
    cover_raster_cache_clear(); assert(cover_raster_cache_bytes() == 0);
    free(pixels); puts("Cover raster ownership, invalidation, LRU and budget tests passed");
}
