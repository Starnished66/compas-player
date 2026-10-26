#include "audio_helpers.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_frame_conversion(void) {
    uint64_t start = 0, end = 0;
    assert(audio_ab_loop_frames(0.25, 0.75, 8, 16, &start, &end));
    assert(start == 2 && end == 6);
    assert(!audio_ab_loop_frames(0.75, 0.25, 8, 16, &start, &end));
    assert(!audio_ab_loop_frames(0.0, 2.1, 8, 16, &start, &end));
    assert(!audio_ab_loop_frames(0.0, 1.0, 8, 16, NULL, &end));
    assert(!audio_ab_loop_frames(0.0, 0x1p64, 1, UINT64_MAX, &start, &end));
    assert(!audio_ab_loop_frames(0.0, INFINITY, 1, UINT64_MAX, &start, &end));
    assert(!audio_ab_loop_frames(NAN, 1.0, 8, 16, &start, &end));
}

static void test_exact_chunk_boundary_and_wrap(void) {
    /* Marker values equal source frame numbers. The requested chunk crosses
     * B=6, so only markers 2..5 may be emitted before the worker seeks A=2. */
    const int16_t source[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    int16_t emitted[10];
    size_t count = 0;
    uint64_t cursor = 2;
    uint64_t n = audio_ab_loop_read_frames(cursor, 6, 8);
    assert(n == 4);
    for (uint64_t i = 0; i < n; ++i) emitted[count++] = source[cursor + i];
    cursor += n;
    assert(cursor == 6);

    /* At the next audio-thread boundary, the decoder seeks to A without
     * changing output state, and the next bounded chunk starts at marker 2. */
    if (audio_ab_loop_read_frames(cursor, 6, 8) == 0) cursor = 2;
    n = audio_ab_loop_read_frames(cursor, 6, 8);
    assert(n == 4);
    for (uint64_t i = 0; i < n; ++i) emitted[count++] = source[cursor + i];
    const int16_t expected[] = {2, 3, 4, 5, 2, 3, 4, 5};
    assert(count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < count; ++i) assert(emitted[i] == expected[i]);
}

int main(void) {
    test_frame_conversion();
    test_exact_chunk_boundary_and_wrap();
    puts("audio A-B loop frame-boundary tests passed");
    return 0;
}
