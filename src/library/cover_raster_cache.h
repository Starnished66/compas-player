#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

/* Two generated-cover rasters, at most 1 MiB total. Caller keeps ownership
 * of input pixels; get returns a separate owned copy, never an LVGL pointer.
 * Identity includes sub-second source-file changes and target dimensions. */
typedef struct {
    struct stat file;
    int width, height;
} cover_raster_key_t;
uint16_t *cover_raster_cache_get(const cover_raster_key_t *key);
/* Check a complete equal entry and update LRU without copying its pixels. */
bool cover_raster_cache_touch(const cover_raster_key_t *key);
void cover_raster_cache_put(const cover_raster_key_t *key, const uint16_t *pixels);
void cover_raster_cache_clear(void);
size_t cover_raster_cache_bytes(void);

#ifdef COVER_RASTER_CACHE_TEST
typedef struct {
    size_t malloc_calls, malloc_bytes, memcpy_calls, memcpy_bytes;
} cover_raster_cache_test_stats_t;
void cover_raster_cache_test_stats_reset(void);
cover_raster_cache_test_stats_t cover_raster_cache_test_stats(void);
#endif
