/* Real libopus decode/seek stress test. Pass generated .opus fixture paths.
 * Fixtures and executable must reside on persistent storage on a device. */
#define _GNU_SOURCE
#include "opus_decoder.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void * decode_files(void * arg) {
    char ** paths = arg;
    int16_t * pcm = malloc(4096 * 2 * sizeof(*pcm));
    assert(pcm);
    for (unsigned round = 0; round < 5; round++) {
        for (char ** path = paths; *path; path++) {
            opus_decoder_wrap_t * dec = opus_open_file(*path);
            assert(dec);
            assert(opus_get_channels(dec) >= 1 && opus_get_channels(dec) <= 2);
            assert(opus_get_sample_rate(dec) == 48000);
            uint64_t total = opus_get_total_pcm_frame_count(dec);
            assert(total > 0);
            uint64_t frames = 0;
            for (;;) {
                decoder_read_result_t read = opus_read_pcm_frames_s16(dec, 4096, pcm);
                assert(read.status == DECODER_READ_OK || read.status == DECODER_READ_EOF);
                frames += read.frames;
                if (read.status == DECODER_READ_EOF) break;
                assert(read.frames > 0);
                /* The last packet can include at most 120 ms of padding. */
                assert(frames <= total + 5760);
            }
            assert(frames >= total);
            assert(opus_seek_to_pcm_frame(dec, total / 2));
            decoder_read_result_t read = opus_read_pcm_frames_s16(dec, 128, pcm);
            assert(read.status == DECODER_READ_OK && read.frames == 128);
            opus_close(dec);
        }
    }
    free(pcm);
    return NULL;
}

int main(int argc, char ** argv) {
    assert(argc > 1);
    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, 256 * 1024) == 0);
    pthread_t workers[2];
    for (unsigned i = 0; i < 2; i++)
        assert(pthread_create(&workers[i], &attr, decode_files, argv + 1) == 0);
    assert(pthread_attr_destroy(&attr) == 0);
    for (unsigned i = 0; i < 2; i++) assert(pthread_join(workers[i], NULL) == 0);
    puts("Opus concurrent decode/seek tests passed");
    return 0;
}
