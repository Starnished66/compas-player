#define _POSIX_C_SOURCE 200809L
#include "player_cover_prefetch.h"

#include "albumart.h"
#include "artwork_coordinator.h"
#include "cover_decode.h"
#include "cover_raster_cache.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PREFETCH_STACK_SIZE (256u * 1024u)
#define PREFETCH_MEMORY_HEADROOM (2u * 1024u * 1024u)
#define PREFETCH_FILE_MAX (4u * 1024u * 1024u)
#define PREFETCH_RASTER_MAX (1024u * 1024u)
#define PREFETCH_SOURCE_SIDE ALBUMART_PLAYER_CACHE_SIZE

typedef struct {
    char path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];
    int width, height;
    uint64_t generation;
} request_t;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static request_t *pending;
static uint64_t generation;
static bool worker_running;

extern void install_thread_crash_altstack(void) __attribute__((weak));

static bool request_cancelled(uint64_t token) {
    bool current;
    pthread_mutex_lock(&state_lock);
    current = token == generation;
    pthread_mutex_unlock(&state_lock);
    return !current;
}

static bool cancel_decode(void *user_data) {
    const request_t *r = user_data;
    /* Coordinator acquire calls this callback while holding coord_lock.
     * Keep it independent of the coordinator to avoid recursive locking;
     * cover_decode checks its priority separately at each decode stage. */
    return !r || request_cancelled(r->generation);
}

static bool memory_budget_ok(size_t additional_bytes) {
    const size_t reserve = artwork_reserve_bytes_for_priority(ARTWORK_PRIO_WARMER);
    if (reserve > SIZE_MAX - PREFETCH_MEMORY_HEADROOM) return false;
    size_t required = reserve + PREFETCH_MEMORY_HEADROOM;
    if (additional_bytes > SIZE_MAX - required) return false;
    required += additional_bytes;
    return system_get_mem_available_bytes() >= required;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool generated_bmp_header(const uint8_t *header, size_t size,
                                 int *width, int *height) {
    if (size < 54 || header[0] != 'B' || header[1] != 'M' ||
        le32(header + 14) < 40 || le32(header + 30) != 0 ||
        header[28] != 24 || header[29] != 0) return false;
    int32_t w = (int32_t)le32(header + 18);
    int32_t h = (int32_t)le32(header + 22);
    if (w != PREFETCH_SOURCE_SIDE || h != PREFETCH_SOURCE_SIDE) return false;
    if (width) *width = w;
    if (height) *height = h;
    return true;
}

/* The generated-cache resolver promises an app-generated BMP. Read its
 * fixed header without heap allocation and admit the decoder's full BMP
 * working set plus the raster-cache copy before allocating encoded bytes. */
static bool estimate_generated_bmp(const char *path, const struct stat *st,
                                   const request_t *r, size_t *decode_bytes,
                                   size_t *raster_copy_bytes) {
    uint8_t header[54];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);
    int width, height;
    if (got != sizeof(header) || !generated_bmp_header(header, got, &width, &height)) return false;
    size_t raster = (size_t)r->width * (size_t)r->height;
    if (raster > SIZE_MAX / 2) return false;
    size_t copy_bytes = raster * 2;
    size_t estimate = artwork_estimate_decode_bytes(ARTWORK_FORMAT_BMP,
        (size_t)st->st_size, (size_t)width, (size_t)height,
        (size_t)r->width, (size_t)r->height, 0, 0);
    if (estimate == SIZE_MAX || copy_bytes > SIZE_MAX - estimate) return false;
    *decode_bytes = estimate;
    *raster_copy_bytes = copy_bytes;
    return true;
}

static bool local_path(const char *path) {
    struct stat st;
    if (!path || !path[0] || strncmp(path, "//", 2) == 0 || strstr(path, "://")) return false;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void process_request(const request_t *r) {
    if (request_cancelled(r->generation) || !memory_budget_ok(0) || !local_path(r->path)) return;

    albumart_info_t info;
    memset(&info, 0, sizeof(info));
    memcpy(info.path, r->path, strlen(r->path) + 1);
    memcpy(info.artist, r->artist, strlen(r->artist) + 1);
    memcpy(info.album, r->album, strlen(r->album) + 1);
    memcpy(info.albumartist, r->album_artist, strlen(r->album_artist) + 1);

    char cache_path[PATH_MAX];
    if (!albumart_generated_cache_fresh(&info, PREFETCH_SOURCE_SIDE, PREFETCH_SOURCE_SIDE,
                                        cache_path, sizeof(cache_path))) return;
    struct stat before;
    if (stat(cache_path, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || (uint64_t)before.st_size > PREFETCH_FILE_MAX) return;

    cover_raster_key_t key = { .file = before, .width = r->width, .height = r->height };
    size_t decode_bytes, raster_copy_bytes;
    if (!estimate_generated_bmp(cache_path, &before, r, &decode_bytes, &raster_copy_bytes) ||
        !memory_budget_ok(decode_bytes + raster_copy_bytes) ||
        request_cancelled(r->generation)) return;
    uint16_t *hit = cover_raster_cache_get(&key);
    if (hit) { free(hit); return; }

    uint8_t *encoded = NULL;
    uint32_t encoded_size = 0;
    if (!memory_budget_ok(decode_bytes + raster_copy_bytes) || request_cancelled(r->generation)) return;
    if (albumart_load_file_ex(cache_path, &encoded, &encoded_size, PREFETCH_FILE_MAX,
                              ARTWORK_PRIO_THUMBNAIL) != ALBUMART_LOAD_OK) {
        free(encoded);
        return;
    }
    int actual_width, actual_height;
    if (!generated_bmp_header(encoded, encoded_size, &actual_width, &actual_height)) {
        free(encoded);
        return;
    }
    size_t actual_decode_bytes = artwork_estimate_decode_bytes(ARTWORK_FORMAT_BMP,
        encoded_size, (size_t)actual_width, (size_t)actual_height,
        (size_t)r->width, (size_t)r->height, 0, 0);
    if (actual_decode_bytes == SIZE_MAX || actual_decode_bytes < encoded_size ||
        actual_decode_bytes - encoded_size > SIZE_MAX - raster_copy_bytes) {
        free(encoded);
        return;
    }
    /* MemAvailable now reflects the resident encoded buffer. Account for the
     * remaining decoder workspace/output and the eventual cache copy. */
    size_t decode_remaining = actual_decode_bytes - encoded_size;
    if (!memory_budget_ok(decode_remaining + raster_copy_bytes) || request_cancelled(r->generation)) {
        free(encoded);
        return;
    }
    uint16_t *pixels = NULL;
    cover_decode_result_t decoded = cover_decode_to_rgb565_ex(
        encoded, encoded_size, r->width, r->height, ARTWORK_PRIO_THUMBNAIL,
        cancel_decode, (void *)r, &pixels);
    free(encoded);
    if (decoded != COVER_DECODE_OK || !pixels || request_cancelled(r->generation) ||
        !memory_budget_ok(raster_copy_bytes)) {
        free(pixels);
        return;
    }

    struct stat after;
    if (stat(cache_path, &after) != 0 ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        !albumart_generated_cache_fresh(&info, PREFETCH_SOURCE_SIDE, PREFETCH_SOURCE_SIDE,
                                        cache_path, sizeof(cache_path))) {
        free(pixels);
        return;
    }

    /* Serialize the final generation check and insertion with cancel(). A
     * caller clearing the raster cache after cancel() cannot be raced by a
     * late insertion from this worker. */
    pthread_mutex_lock(&state_lock);
    if (r->generation == generation && memory_budget_ok(raster_copy_bytes))
        cover_raster_cache_put(&key, pixels);
    pthread_mutex_unlock(&state_lock);
    free(pixels);
}

static void *worker_main(void *unused) {
    (void)unused;
    if (install_thread_crash_altstack) install_thread_crash_altstack();
    for (;;) {
        pthread_mutex_lock(&state_lock);
        request_t *r = pending;
        pending = NULL;
        if (!r) {
            worker_running = false;
            pthread_cond_broadcast(&state_cond);
            pthread_mutex_unlock(&state_lock);
            return NULL;
        }
        pthread_mutex_unlock(&state_lock);
        process_request(r);
        free(r);
    }
}

#ifdef PLAYER_COVER_PREFETCH_TEST
bool player_cover_prefetch_test_wait_idle(unsigned timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t nsec = (uint64_t)deadline.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
    deadline.tv_sec += (time_t)(nsec / 1000000000ULL);
    deadline.tv_nsec = (long)(nsec % 1000000000ULL);
    pthread_mutex_lock(&state_lock);
    int rc = 0;
    while (worker_running && rc == 0)
        rc = pthread_cond_timedwait(&state_cond, &state_lock, &deadline);
    bool idle = !worker_running;
    pthread_mutex_unlock(&state_lock);
    return idle;
}
#endif

static bool copy_field(char *dst, size_t capacity, const char *src) {
    if (!src) src = "";
    size_t len = strnlen(src, capacity);
    if (len >= capacity) return false;
    memcpy(dst, src, len + 1);
    return true;
}

bool player_cover_prefetch_submit(const char *path, const char *artist,
                                  const char *album, const char *album_artist,
                                  int width, int height) {
    if (width <= 0 || height <= 0 || width > MAX_PLAYER_COVER_SIDE ||
        height > MAX_PLAYER_COVER_SIDE || (size_t)width > PREFETCH_RASTER_MAX / 2 / (size_t)height)
        return false;
    request_t *r = calloc(1, sizeof(*r));
    if (!r) return false;
    /* albumart_info_t is the downstream resolver's fixed-size path record;
     * reject anything it could not carry without truncation. */
    if (!copy_field(r->path, sizeof(((albumart_info_t *)0)->path), path) || !r->path[0] ||
        !copy_field(r->artist, sizeof(r->artist), artist) ||
        !copy_field(r->album, sizeof(r->album), album) ||
        !copy_field(r->album_artist, sizeof(r->album_artist), album_artist)) {
        free(r);
        return false;
    }
    r->width = width;
    r->height = height;

    pthread_mutex_lock(&state_lock);
    r->generation = ++generation;
    free(pending);
    pending = r;
    if (!worker_running) {
        pthread_attr_t attr;
        int attr_rc = pthread_attr_init(&attr);
        bool attr_initialized = attr_rc == 0;
        if (attr_rc == 0) attr_rc = pthread_attr_setstacksize(&attr, PREFETCH_STACK_SIZE);
        if (attr_rc == 0) attr_rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (attr_rc != 0) {
            if (attr_initialized) pthread_attr_destroy(&attr);
            free(pending);
            pending = NULL;
            pthread_mutex_unlock(&state_lock);
            return false;
        }
        pthread_t thread;
        int rc = pthread_create(&thread, &attr, worker_main, NULL);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            free(pending);
            pending = NULL;
            pthread_mutex_unlock(&state_lock);
            return false;
        }
        worker_running = true;
    }
    pthread_mutex_unlock(&state_lock);
    return true;
}

void player_cover_prefetch_cancel(void) {
    pthread_mutex_lock(&state_lock);
    ++generation;
    free(pending);
    pending = NULL;
    pthread_mutex_unlock(&state_lock);
}
