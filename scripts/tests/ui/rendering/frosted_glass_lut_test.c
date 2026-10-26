#include "../../../../src/ui/effects/frosted_glass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x8f312a71u;
static uint32_t next_random(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static uint64_t hash_rgb565(const uint8_t *bytes, size_t pixels) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t pixel;
        memcpy(&pixel, bytes + i * 2, sizeof(pixel));
        hash ^= (uint8_t) pixel;
        hash *= 1099511628211ull;
        hash ^= (uint8_t) (pixel >> 8);
        hash *= 1099511628211ull;
    }
    return hash;
}

typedef struct {
    unsigned calls;
    unsigned cancel_on_call;
} cancel_after_t;

static bool cancel_on_requested_check(void * user_data) {
    cancel_after_t * state = user_data;
    state->calls++;
    return state->calls >= state->cancel_on_call;
}

static int compare_case(const char *name, int sw, int sh, int ow, int oh,
                       int radius, int passes, int num, int den, bool mirror,
                       uint64_t expected_hash) {
    size_t count = (size_t) sw * sh;
    uint16_t *source = malloc(count * sizeof(*source));
    if (!source) return 0;
    for (size_t i = 0; i < count; i++) source[i] = (uint16_t) next_random();
    uint8_t *actual = frosted_glass_blur_rgb565(source, sw, sh, ow, oh,
        radius, passes, num, den, mirror);
    uint8_t *cancelable = frosted_glass_blur_rgb565_cancelable(source, sw, sh, ow, oh,
        radius, passes, num, den, mirror, NULL, NULL);
    int ok = actual && cancelable &&
        memcmp(actual, cancelable, (size_t) ow * oh * 2) == 0 &&
        hash_rgb565(actual, (size_t) ow * oh) == expected_hash;
    if (!ok) fprintf(stderr, "%s: output hash differs or allocation failed\n", name);
    free(source); free(actual); free(cancelable);
    return ok;
}

static int verify_cancelled_at(unsigned cancel_on_call, int blur_passes) {
    uint16_t source = 0x7bef;
    cancel_after_t state = { .cancel_on_call = cancel_on_call };
    uint8_t *result = frosted_glass_blur_rgb565_cancelable(&source, 1, 1, 17, 23,
        3, blur_passes, 1, 2, false, cancel_on_requested_check, &state);
    bool cancelled = result == NULL;
    free(result);
    return cancelled && state.calls == cancel_on_call;
}

int main(void) {
    int ok = 1;
    ok &= compare_case("square/upscale", 360, 360, 480, 480, 12, 2, 1, 2, false, 0x362400b3edd89253ull);
    ok &= compare_case("landscape/crop", 640, 360, 480, 800, 18, 3, 3, 4, false, 0xa3024ab5ad52637full);
    ok &= compare_case("portrait/mirror", 360, 640, 480, 800, 8, 1, 1, 1, true, 0x6cd23554f1b30b7bull);
    ok &= compare_case("downscale", 1200, 800, 240, 400, 2, 1, 7, 5, false, 0xaef38970abeaa70bull);
    ok &= compare_case("fallback-bilinear", 37, 61, 113, 89, 5, 2, 2, 3, true, 0x49dc8a228953b6d0ull);
    ok &= compare_case("extreme-darken", 80, 120, 17, 23, 0, 0, 255, 1, false, 0x670f5f1e45870068ull);
    ok &= compare_case("INT_MAX-darken-platform-semantics", 80, 120, 17, 23, 0, 0, 2147483647, 1, false, 0x10907d192dc2bd6aull);
    ok &= compare_case("zero-darken", 80, 120, 17, 23, 0, 0, 0, 9, false, 0x46211a429e4b96ddull);
    ok &= compare_case("single-pixel-output", 1, 1, 1, 1, 64, 16, 1, 2, false, 0x07c92f07b491efa9ull);
    ok &= compare_case("R1-cover-480x800", 640, 480, 480, 800, 24, 3, 1, 2, false, 0x496a57b9851ceb35ull);
    /* For this tiny source, the deterministic callback sequence is: initial
     * check, 100 fallback-downsample rows, then blur rows/passes and upscale.
     * Cancellation at these points must stop work and return no partial image. */
    ok &= verify_cancelled_at(1, 1);   /* before allocation/work */
    ok &= verify_cancelled_at(2, 1);   /* first downsample row */
    ok &= verify_cancelled_at(102, 1); /* first blur row */
    ok &= verify_cancelled_at(122, 0); /* first upscale row */
    return ok ? 0 : 1;
}
