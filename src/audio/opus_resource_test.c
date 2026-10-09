#include "opus_decoder.h"
#include "opusfile_alloc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Real libogg/libopusfile resource regressions: oversized tag payload and
 * many tiny comments must fail both playback and partial metadata opens.
 * A normal file must still open after either failure releases its memory. */
static void test_allocator(void) {
    assert(compas_opusfile_calloc(SIZE_MAX, 2) == NULL && errno == ENOMEM);
    unsigned char * block = compas_opusfile_malloc(8U * 1024U * 1024U);
    assert(block);
    memset(block, 0x42, 32);
    /* Both allocations must be counted while resizing, and the original
     * remains usable when the combined peak exceeds the library budget. */
    assert(compas_opusfile_realloc(block, 9U * 1024U * 1024U) == NULL);
    for (int i = 0; i < 32; i++) assert(block[i] == 0x42);
    compas_opusfile_free(block);
    block = compas_opusfile_calloc(32, 1);
    assert(block);
    for (int i = 0; i < 32; i++) assert(block[i] == 0);
    block[0] = 0x42;
    block = compas_opusfile_realloc(block, 64);
    assert(block && block[0] == 0x42);
    compas_opusfile_free(block);
}

static void test_near_cap_open_failures(char **paths) {
    unsigned char *reserve = compas_opusfile_malloc(16U * 1024U * 1024U - 13312U);
    assert(reserve);

    /* Leave only the small margin from the original failure reproduction.
     * Both partial metadata open and full playback open must report failure
     * through their APIs rather than dereferencing a failed libogg buffer. */
    for (int i = 0; i < 3; i++) {
        assert(compas_opusfile_open(paths[i], true) == NULL);
        assert(compas_opusfile_open(paths[i], false) == NULL);
    }

    compas_opusfile_free(reserve);
}

int main(int argc, char ** argv) {
    if (argc == 4 && strcmp(argv[1], "--chained-failure") == 0) {
        for (int round = 0; round < 32; round++)
            assert(compas_opusfile_open(argv[2], false) == NULL);
        OggOpusFile * of = compas_opusfile_open(argv[3], false);
        assert(of);
        op_free(of);
        puts("Opus damaged chained-link cleanup test passed");
        return 0;
    }
    assert(argc == 5);
    test_allocator();
    test_near_cap_open_failures(argv + 1);
    /* A later chained link with valid tags but no audio packets fails while
     * finding its initial PCM offset.  Repeated opens must release those tags
     * so the allocator's process-wide budget remains available. */
    for (int round = 0; round < 32; round++)
        assert(compas_opusfile_open(argv[4], false) == NULL);
    for (int round = 0; round < 2; round++) {
        for (int i = 1; i <= 2; i++) {
            assert(compas_opusfile_open(argv[i], false) == NULL);
            assert(compas_opusfile_open(argv[i], true) == NULL);
            opus_decoder_wrap_t * dec = opus_open_file(argv[3]);
            assert(dec);
            int16_t pcm[256];
            decoder_read_result_t read = opus_read_pcm_frames_s16(dec, 128, pcm);
            assert(read.frames == 128 && read.status == DECODER_READ_OK);
            opus_close(dec);
        }
        OggOpusFile * of = compas_opusfile_open(argv[3], false);
        assert(of);
        op_free(of);
    }
    test_allocator();
    puts("Opus oversized-tag and comment-count resource tests passed");
    return 0;
}
