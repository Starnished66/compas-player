#define _POSIX_C_SOURCE 200809L
#include "player_cover_prefetch.h"
#include "albumart.h"
#include "artwork_coordinator.h"
#include "cover_decode.h"
#include "cover_raster_cache.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cond = PTHREAD_COND_INITIALIZER;
static char fixture[256];
static bool hold_decode;
static bool decode_entered;
static unsigned decode_count;
static bool replace_file_during_decode;
static void pause_ms(long milliseconds) {
    struct timespec delay = { milliseconds / 1000,
        (milliseconds % 1000) * 1000000L };
    nanosleep(&delay, NULL);
}
extern bool player_cover_prefetch_test_wait_idle(unsigned timeout_ms);

static void put_le32(FILE *f, uint32_t v) {
    fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f);
    fputc((int)((v >> 16) & 255), f); fputc((int)((v >> 24) & 255), f);
}
static void write_bmp_fixture(void) {
    FILE *f = fopen(fixture, "wb"); assert(f);
    const uint32_t pixel_bytes = ALBUMART_PLAYER_CACHE_SIZE * ALBUMART_PLAYER_CACHE_SIZE * 3u;
    fputc('B', f); fputc('M', f); put_le32(f, 54u + pixel_bytes);
    put_le32(f, 0); put_le32(f, 54); put_le32(f, 40);
    put_le32(f, ALBUMART_PLAYER_CACHE_SIZE); put_le32(f, ALBUMART_PLAYER_CACHE_SIZE);
    fputc(1, f); fputc(0, f);
    fputc(24, f); fputc(0, f); put_le32(f, 0); put_le32(f, pixel_bytes);
    put_le32(f, 2835); put_le32(f, 2835); put_le32(f, 0); put_le32(f, 0);
    for (uint32_t i = 0; i < pixel_bytes; ++i) fputc(0, f);
    fclose(f);
}

bool audio_is_playing(void) { return false; }
bool albumart_generated_cache_fresh(const albumart_info_t *i, int w, int h,
                                    char *out, size_t n) {
    (void)i; assert(w == ALBUMART_PLAYER_CACHE_SIZE && h == ALBUMART_PLAYER_CACHE_SIZE);
    if (strlen(fixture) >= n) return false;
    strcpy(out, fixture); return true;
}
albumart_load_result_t albumart_load_file_ex(const char *p, uint8_t **out,
        uint32_t *size, uint32_t max, artwork_priority_t prio) {
    (void)prio;
    FILE *f = fopen(p, "rb"); if (!f) return ALBUMART_LOAD_TEMPORARY;
    assert(fseek(f, 0, SEEK_END) == 0); long n = ftell(f);
    if (n <= 0 || (uint64_t)n > max) { fclose(f); return ALBUMART_LOAD_INVALID; }
    rewind(f); *out = malloc((size_t)n);
    if (!*out) { fclose(f); return ALBUMART_LOAD_TEMPORARY; }
    if (fread(*out, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(*out); *out = NULL; return ALBUMART_LOAD_TEMPORARY;
    }
    fclose(f); *size = (uint32_t)n; return ALBUMART_LOAD_OK;
}
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t *d, uint32_t s,
        int w, int h, artwork_priority_t p, artwork_cancel_fn cb, void *u,
        uint16_t **out) {
    (void)d; (void)s; (void)p;
    pthread_mutex_lock(&test_lock);
    ++decode_count; decode_entered = true; pthread_cond_broadcast(&test_cond);
    pthread_mutex_unlock(&test_lock);
    artwork_acquire_result_t acquired = artwork_coordinator_acquire(
        ARTWORK_PRIO_THUMBNAIL, 0, 1500, cb, u);
    if (acquired != ARTWORK_ACQUIRE_OK) return COVER_DECODE_FAIL_CANCELLED;
    artwork_coordinator_release(ARTWORK_PRIO_THUMBNAIL);
    pthread_mutex_lock(&test_lock);
    while (hold_decode && !(cb && cb(u))) pthread_cond_wait(&test_cond, &test_lock);
    bool cancelled = cb && cb(u);
    if (replace_file_during_decode) {
        replace_file_during_decode = false;
        pthread_mutex_unlock(&test_lock);
        FILE *f = fopen(fixture, "ab"); assert(f); fputc(1, f); fclose(f);
        pthread_mutex_lock(&test_lock);
    }
    pthread_mutex_unlock(&test_lock);
    if (cancelled) return COVER_DECODE_FAIL_CANCELLED;
    *out = calloc((size_t)w * h, sizeof(uint16_t));
    return *out ? COVER_DECODE_OK : COVER_DECODE_FAIL_ALLOC;
}

static void await_decode(unsigned count) {
    pthread_mutex_lock(&test_lock);
    while (decode_count < count) pthread_cond_wait(&test_cond, &test_lock);
    pthread_mutex_unlock(&test_lock);
}
static void release_decode(void) {
    pthread_mutex_lock(&test_lock); hold_decode = false;
    pthread_cond_broadcast(&test_cond); pthread_mutex_unlock(&test_lock);
}
static void settle(void) {
    assert(player_cover_prefetch_test_wait_idle(3000));
}
static void reset_decode(void) {
    pthread_mutex_lock(&test_lock); hold_decode = false; decode_entered = false;
    decode_count = 0; replace_file_during_decode = false;
    pthread_mutex_unlock(&test_lock); cover_raster_cache_clear();
    write_bmp_fixture();
}
static bool submit(void) {
    return player_cover_prefetch_submit(fixture, "artist", "album", "", 16, 16);
}
static bool submit_dims(int width, int height) {
    return player_cover_prefetch_submit(fixture, "artist", "album", "", width, height);
}
int main(void) {
    assert(strlen(fixture) == 0);
    strcpy(fixture, "player-cover-prefetch-test-XXXXXX");
    int fd = mkstemp(fixture); assert(fd >= 0); close(fd);
    system_set_mock_mem_available(64u * 1024u * 1024u);
    reset_decode();

    /* Latest pending request replaces active work. Distinct raster keys make
     * a stale insertion from the cancelled 16px request observable. */
    pthread_mutex_lock(&test_lock); hold_decode = true; pthread_mutex_unlock(&test_lock);
    assert(submit_dims(16, 16)); await_decode(1);
    assert(submit_dims(8, 8)); release_decode(); settle();
    assert(decode_count == 2 && cover_raster_cache_bytes() == 8u * 8u * 2u);
    struct stat latest_st; assert(stat(fixture, &latest_st) == 0);
    cover_raster_key_t stale_key = { .file = latest_st, .width = 16, .height = 16 };
    cover_raster_key_t latest_key = { .file = latest_st, .width = 8, .height = 8 };
    uint16_t *stale_pixels = cover_raster_cache_get(&stale_key);
    uint16_t *latest_pixels = cover_raster_cache_get(&latest_key);
    assert(stale_pixels == NULL && latest_pixels != NULL);
    free(latest_pixels);

    /* Cancellation invalidates active decode and prevents stale insertion. */
    reset_decode(); pthread_mutex_lock(&test_lock); hold_decode = true; pthread_mutex_unlock(&test_lock);
    assert(submit()); await_decode(1); player_cover_prefetch_cancel(); release_decode();
    settle(); assert(cover_raster_cache_bytes() == 0);

    /* Memory floor avoids starting a decode. */
    reset_decode(); struct stat budget_st; assert(stat(fixture, &budget_st) == 0);
    size_t decode_estimate = artwork_estimate_decode_bytes(ARTWORK_FORMAT_BMP,
        (size_t)budget_st.st_size, ALBUMART_PLAYER_CACHE_SIZE,
        ALBUMART_PLAYER_CACHE_SIZE, 16, 16, 0, 0);
    size_t cache_copy = 16u * 16u * 2u;
    size_t exact_floor = artwork_reserve_bytes_for_priority(ARTWORK_PRIO_WARMER) +
        2u * 1024u * 1024u + decode_estimate + cache_copy;
    system_set_mock_mem_available(exact_floor - 1); assert(submit()); settle();
    assert(decode_count == 0 && cover_raster_cache_bytes() == 0);
    system_set_mock_mem_available(exact_floor); assert(submit()); settle();
    assert(decode_count == 1 && cover_raster_cache_bytes() == cache_copy);
    system_set_mock_mem_available(64u * 1024u * 1024u);

    /* Changed file identity after decode cannot populate the raster cache. */
    reset_decode(); pthread_mutex_lock(&test_lock); hold_decode = true;
    replace_file_during_decode = true; pthread_mutex_unlock(&test_lock);
    assert(submit()); await_decode(1); release_decode(); settle();
    assert(cover_raster_cache_bytes() == 0);

    /* A raster cache hit is freed and does not decode again. */
    reset_decode(); struct stat st; assert(stat(fixture, &st) == 0);
    cover_raster_key_t key = { .file = st, .width = 16, .height = 16 };
    uint16_t pixels[16 * 16] = { 0 }; cover_raster_cache_put(&key, pixels);
    assert(submit()); settle(); assert(decode_count == 0);
    player_cover_prefetch_cancel(); cover_raster_cache_clear(); unlink(fixture);

    /* Real coordinator contention: hold PLAYER while the decoder requests a
     * THUMBNAIL slot. Its cancellation callback runs under coord_lock. */
    reset_decode();
    assert(artwork_coordinator_acquire(ARTWORK_PRIO_PLAYER, 0, 100,
                                      NULL, NULL) == ARTWORK_ACQUIRE_OK);
    assert(submit()); await_decode(1); pause_ms(75);
    player_cover_prefetch_cancel();
    artwork_coordinator_release(ARTWORK_PRIO_PLAYER);
    settle(); assert(cover_raster_cache_bytes() == 0);
    cover_raster_cache_clear(); unlink(fixture);

    /* Production player size: resample the validated 480px generated source
     * into the requested 350px raster-cache entry. */
    reset_decode(); assert(submit_dims(350, 350)); settle();
    assert(cover_raster_cache_bytes() == 350u * 350u * 2u);
    player_cover_prefetch_cancel(); cover_raster_cache_clear(); unlink(fixture);
    puts("Player cover prefetch queue tests passed");
    return 0;
}
