/* Real libopusfile decode/seek regression test. Pass valid generated fixtures
 * on persistent storage (device tests must run from SD or flashed firmware). */
#define _GNU_SOURCE
#include "ogg_probe.h"
#include "opus_decoder.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_READ_FRAMES 512U
/* libopusfile seeks with decoder preroll. Discard 250 ms after nonzero seeks
 * so the comparison checks timeline position after the codec state settles. */
#define PCM_COMPARE_TOLERANCE 32
#define OPUS_SEEK_PREROLL_FRAMES 12000U

static void compare_pcm(const char * path, uint64_t target, const int16_t * actual,
                        const int16_t * expected, size_t samples) {
    int max_difference = 0;
    size_t first_exceeded = SIZE_MAX;
    for (size_t i = 0; i < samples; i++) {
        int difference = (int) actual[i] - (int) expected[i];
        if (difference < 0) difference = -difference;
        if (difference > max_difference) max_difference = difference;
        if (difference > PCM_COMPARE_TOLERANCE && first_exceeded == SIZE_MAX) first_exceeded = i;
    }
    if (first_exceeded != SIZE_MAX)
        fprintf(stderr, "%s seek %llu first delta>%d at sample %zu: got %d expected %d; max delta %d over %zu samples\n",
                path, (unsigned long long) target, PCM_COMPARE_TOLERANCE, first_exceeded,
                actual[first_exceeded], expected[first_exceeded], max_difference, samples);
    assert(max_difference <= PCM_COMPARE_TOLERANCE);
}

static void test_seek_matches_sequential(const char * path, opus_decoder_wrap_t * dec, const int16_t * pcm,
                                         uint64_t total, unsigned int channels, uint64_t target) {
    bool seek_succeeded = opus_seek_to_pcm_frame(dec, target);
    if (!seek_succeeded)
        fprintf(stderr, "%s seek failed at %llu (total %llu)\n", path,
                (unsigned long long) target, (unsigned long long) total);
    assert(seek_succeeded);
    if (target >= total) {
        int16_t sample[2] = {0};
        decoder_read_result_t read = opus_read_pcm_frames_s16(dec, 1, sample);
        assert(read.frames == 0 && read.status == DECODER_READ_EOF);
        return;
    }

    uint64_t compare_target = target;
    if (target > 0) {
        int16_t preroll[OPUS_SEEK_PREROLL_FRAMES * 2];
        decoder_read_result_t preroll_read = opus_read_pcm_frames_s16(
            dec, OPUS_SEEK_PREROLL_FRAMES, preroll);
        assert(preroll_read.status == DECODER_READ_OK &&
               preroll_read.frames == OPUS_SEEK_PREROLL_FRAMES);
        compare_target += OPUS_SEEK_PREROLL_FRAMES;
    }

    uint64_t frames = total - compare_target;
    if (frames > TEST_READ_FRAMES) frames = TEST_READ_FRAMES;
    assert(frames > 0);
    int16_t actual[TEST_READ_FRAMES * 2];
    decoder_read_result_t read = opus_read_pcm_frames_s16(dec, frames, actual);
    assert(read.frames == frames && read.status == DECODER_READ_OK);
    compare_pcm(path, compare_target, actual, pcm + (size_t) compare_target * channels,
                (size_t) frames * channels);
}

static void test_fixture(const char * path) {
    unsigned int expected_channels = strstr(path, "mono-silk") ? 1U : 2U;
    uint64_t expected_frames = strstr(path, "chained") ? 192000U : 96000U;
    assert(ogg_detect_codec(path) == OGG_CODEC_OPUS);

    opus_decoder_wrap_t * dec = opus_open_file(path);
    assert(dec);
    unsigned int channels = opus_get_channels(dec);
    uint64_t total = opus_get_total_pcm_frame_count(dec);
    assert(channels == expected_channels);
    assert(opus_get_sample_rate(dec) == 48000);
    assert(total == expected_frames);
    assert(total <= SIZE_MAX / channels / sizeof(int16_t));

    size_t sample_count = (size_t) total * channels;
    int16_t * pcm = malloc(sample_count * sizeof(*pcm));
    assert(pcm);

    uint64_t frames_read = 0;
    while (frames_read < total) {
        uint64_t request = total - frames_read;
        if (request > 4096) request = 4096;
        decoder_read_result_t read = opus_read_pcm_frames_s16(
            dec, request, pcm + (size_t) frames_read * channels);
        assert(read.status == DECODER_READ_OK);
        assert(read.frames > 0 && read.frames <= request);
        frames_read += read.frames;
    }
    assert(frames_read == total);

    /* Asking once more after the exact advertised duration confirms EOS
     * trimming, including Opus's final packet padding. */
    int16_t tail[2] = {0};
    decoder_read_result_t eos = opus_read_pcm_frames_s16(dec, 1, tail);
    assert(eos.frames == 0 && eos.status == DECODER_READ_EOF);

    test_seek_matches_sequential(path, dec, pcm, total, channels, 0);
    test_seek_matches_sequential(path, dec, pcm, total, channels, total / 2);
    test_seek_matches_sequential(path, dec, pcm, total, channels, total);
    test_seek_matches_sequential(path, dec, pcm, total, channels, total + 1000);

    free(pcm);
    opus_close(dec);
}

static void * decode_files(void * arg) {
    char ** paths = arg;
    for (unsigned round = 0; round < 2; round++) {
        for (char ** path = paths; *path; path++) {
            if (ogg_detect_codec(*path) == OGG_CODEC_OPUS) test_fixture(*path);
        }
    }
    return NULL;
}

int main(int argc, char ** argv) {
    assert(argc > 1);
    bool saw_opus_suffix = false;
    bool saw_ogg_suffix = false;
    bool saw_vorbis = false;
    for (int i = 1; i < argc; i++) {
        const char * suffix = strrchr(argv[i], '.');
        if (suffix && strcmp(suffix, ".opus") == 0) saw_opus_suffix = true;
        if (suffix && strcmp(suffix, ".ogg") == 0) saw_ogg_suffix = true;
        ogg_codec_t codec = ogg_detect_codec(argv[i]);
        if (strstr(argv[i], "vorbis.ogg")) {
            assert(codec == OGG_CODEC_VORBIS);
            saw_vorbis = true;
        } else {
            assert(codec == OGG_CODEC_OPUS);
        }
    }
    assert(saw_opus_suffix && saw_ogg_suffix && saw_vorbis);

    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, 256 * 1024) == 0);
    pthread_t workers[2];
    for (unsigned i = 0; i < 2; i++)
        assert(pthread_create(&workers[i], &attr, decode_files, argv + 1) == 0);
    assert(pthread_attr_destroy(&attr) == 0);
    for (unsigned i = 0; i < 2; i++) assert(pthread_join(workers[i], NULL) == 0);
    puts("Opus duration, seek, channel, concurrent decode, and Ogg probe tests passed");
    return 0;
}
