#include "player_artwork_cache.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "artwork_priority.h"
#include "artwork_memory.h"
extern size_t system_get_mem_available_bytes(void);
static atomic_uint player_reflection_cache_epoch_value;

bool cover_file_id_same(const cover_file_id_t * a, const cover_file_id_t * b) {
    return a->valid && b->valid && a->dev == b->dev && a->ino == b->ino &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec &&
           a->ctime_sec == b->ctime_sec && a->ctime_nsec == b->ctime_nsec &&
           a->size == b->size;
}

size_t player_raster_cache_memory_floor(void) {
    return artwork_reserve_bytes_for_priority(ARTWORK_PRIO_WARMER) + 1024u * 1024u;
}
unsigned int player_reflection_cache_epoch(void) {
    return atomic_load_explicit(&player_reflection_cache_epoch_value, memory_order_relaxed);
}

#define PLAYER_REFLECTION_CACHE_ENTRIES 2
typedef struct {
    bool valid;
    cover_file_id_t file;
    int width, height;
    int blur_radius, blur_passes, darken_num, darken_den;
    uint32_t tint_rgb;
    int tint_weight;
    uint8_t * pixels;
} player_reflection_cache_entry_t;
static player_reflection_cache_entry_t player_reflection_cache[PLAYER_REFLECTION_CACHE_ENTRIES];
static unsigned player_reflection_cache_next;
static pthread_mutex_t player_reflection_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
size_t player_reflection_bytes(void) {
    return (size_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2;
}

void player_reflection_cache_clear(void) {
    pthread_mutex_lock(&player_reflection_cache_mutex);
    atomic_fetch_add_explicit(&player_reflection_cache_epoch_value, 1u, memory_order_relaxed);
    for (unsigned i = 0; i < PLAYER_REFLECTION_CACHE_ENTRIES; ++i) {
        free(player_reflection_cache[i].pixels);
        memset(&player_reflection_cache[i], 0, sizeof(player_reflection_cache[i]));
    }
    player_reflection_cache_next = 0;
    pthread_mutex_unlock(&player_reflection_cache_mutex);
}

uint8_t * player_reflection_cache_get(const cover_file_id_t * file,
                                              const player_frost_params_t * params) {
    if (!file || !file->valid) return NULL;
    pthread_mutex_lock(&player_reflection_cache_mutex);
    for (unsigned i = 0; i < PLAYER_REFLECTION_CACHE_ENTRIES; ++i) {
        player_reflection_cache_entry_t * entry = &player_reflection_cache[i];
        if (!entry->valid || !entry->pixels || !cover_file_id_same(&entry->file, file) ||
            entry->width != REFLECTION_WIDTH || entry->height != BOARD_SCREEN_HEIGHT ||
            entry->blur_radius != params->blur_radius || entry->blur_passes != params->blur_passes ||
            entry->darken_num != params->darken_num || entry->darken_den != params->darken_den ||
            entry->tint_rgb != params->tint_rgb || entry->tint_weight != params->tint_weight)
            continue;
        size_t bytes = player_reflection_bytes();
        if (system_get_mem_available_bytes() < bytes + player_raster_cache_memory_floor()) {
            pthread_mutex_unlock(&player_reflection_cache_mutex);
            return NULL;
        }
        uint8_t * copy = malloc(bytes);
        if (copy) memcpy(copy, entry->pixels, bytes);
        pthread_mutex_unlock(&player_reflection_cache_mutex);
        return copy;
    }
    pthread_mutex_unlock(&player_reflection_cache_mutex);
    return NULL;
}

void player_reflection_cache_put(const cover_file_id_t * file,
                                         const player_frost_params_t * params,
                                         const uint8_t * pixels,
                                         unsigned int expected_epoch,
                                         unsigned int generation,
                                         player_artwork_cache_stale_policy_fn stale_policy) {
    if (!file || !file->valid || !pixels) return;
    size_t bytes = player_reflection_bytes();
    if (system_get_mem_available_bytes() < bytes + player_raster_cache_memory_floor()) return;
    uint8_t * copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, pixels, bytes);
    pthread_mutex_lock(&player_reflection_cache_mutex);
    if (atomic_load_explicit(&player_reflection_cache_epoch_value, memory_order_relaxed) != expected_epoch ||
        (generation && stale_policy && stale_policy(generation))) {
        pthread_mutex_unlock(&player_reflection_cache_mutex);
        free(copy);
        return;
    }
    unsigned i = player_reflection_cache_next++ % PLAYER_REFLECTION_CACHE_ENTRIES;
    free(player_reflection_cache[i].pixels);
    player_reflection_cache[i] = (player_reflection_cache_entry_t) {
        .valid = true, .file = *file, .width = REFLECTION_WIDTH, .height = BOARD_SCREEN_HEIGHT,
        .blur_radius = params->blur_radius, .blur_passes = params->blur_passes,
        .darken_num = params->darken_num, .darken_den = params->darken_den,
        .tint_rgb = params->tint_rgb, .tint_weight = params->tint_weight, .pixels = copy
    };
    pthread_mutex_unlock(&player_reflection_cache_mutex);
}
