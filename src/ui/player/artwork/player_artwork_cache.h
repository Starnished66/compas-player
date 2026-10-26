#ifndef PLAYER_ARTWORK_CACHE_H
#define PLAYER_ARTWORK_CACHE_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>
#include "../../../core/system/runtime/board_config.h"
typedef struct {
    bool valid;
    dev_t dev;
    ino_t ino;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    off_t size;
} cover_file_id_t;
typedef struct {
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
    uint32_t tint_rgb;
    int tint_weight;
} player_frost_params_t;
#define REFLECTION_WIDTH BOARD_SCREEN_WIDTH
/* compute_reflection_bytes() keeps the existing 60x100 bounded work image
 * and delegates its crop/blur/upscale pipeline to frosted_glass.c. */
#define REFLECTION_BLUR_RADIUS 5
#define REFLECTION_BLUR_PASSES 3
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 2
/* A light theme's frost is 80% its screen color over the blurred artwork. */
#define REFLECTION_LIGHT_TINT_WEIGHT 205

typedef bool (*player_artwork_cache_stale_policy_fn)(unsigned int generation);
bool cover_file_id_same(const cover_file_id_t * a, const cover_file_id_t * b);
size_t player_reflection_bytes(void);
size_t player_raster_cache_memory_floor(void);
void player_reflection_cache_clear(void);
uint8_t * player_reflection_cache_get(const cover_file_id_t * file, const player_frost_params_t * params);
void player_reflection_cache_put(const cover_file_id_t * file, const player_frost_params_t * params,
                                 const uint8_t * pixels, unsigned int expected_epoch,
                                 unsigned int generation, player_artwork_cache_stale_policy_fn stale_policy);
unsigned int player_reflection_cache_epoch(void);
/* get returns a caller-owned copy. clear bumps the epoch and frees entries.
 * For nonzero generations, put invokes stale_policy once under the cache mutex
 * after the epoch check. The callback must be synchronous, nonblocking, and
 * must not reenter this cache; it is never retained. */
#endif
