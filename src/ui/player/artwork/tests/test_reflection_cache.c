#include "../player_artwork_cache.h"
#include "../../../../library/artwork/coordinator/artwork_memory.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static size_t available = SIZE_MAX;
static unsigned policy_calls;
size_t system_get_mem_available_bytes(void) { return available; }
size_t artwork_reserve_bytes_for_priority(artwork_priority_t priority) {
    (void)priority;
    return 1024;
}
static bool stale(unsigned generation) {
    assert(generation == 7);
    policy_calls++;
    return true;
}

int main(void) {
    cover_file_id_t a = { .valid = true, .ino = 1 };
    cover_file_id_t b = { .valid = true, .ino = 2 };
    player_frost_params_t params = { .blur_radius = 5, .darken_den = 2 };
    unsigned epoch = player_reflection_cache_epoch();
    uint8_t * pixels = malloc(player_reflection_bytes());
    assert(pixels);
    player_reflection_cache_put(&a, &params, pixels, epoch, 0, NULL);
    assert(!player_reflection_cache_get(&b, &params));
    player_frost_params_t other = params;
    other.tint_weight = 1;
    assert(!player_reflection_cache_get(&a, &other));
    /* Taking an existing allocation works even under memory pressure. */
    available = 0;
    assert(player_reflection_cache_get(&a, &params) == pixels);
    assert(!player_reflection_cache_get(&a, &params));
    free(pixels);
    player_reflection_cache_put(&a, &params, malloc(1), epoch, 0, NULL);
    assert(!player_reflection_cache_get(&a, &params));
    available = SIZE_MAX;
    player_reflection_cache_put(&a, &params, malloc(1), epoch, 7, stale);
    assert(policy_calls == 1);
    assert(!player_reflection_cache_get(&a, &params));
    player_reflection_cache_clear();
    player_reflection_cache_put(&a, &params, malloc(1), epoch, 7, stale);
    assert(policy_calls == 1);
    assert(!player_reflection_cache_get(&a, &params));
    epoch = player_reflection_cache_epoch();
    player_reflection_cache_put(&a, &params, malloc(1), epoch, 0, NULL);
    pixels = malloc(1);
    player_reflection_cache_put(&b, &params, pixels, epoch, 0, NULL);
    assert(!player_reflection_cache_get(&a, &params));
    assert(player_reflection_cache_get(&b, &params) == pixels);
    free(pixels);
    player_reflection_cache_clear();
    puts("reflection cache ownership: PASS");
}
