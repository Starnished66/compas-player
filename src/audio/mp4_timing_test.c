#include "aac_decoder.h"
#include "alac_decoder.h"
#include "mp4_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static int read_all_aac(aac_decoder_t *d, int16_t **out, uint64_t *frames, unsigned channels) {
    uint64_t cap = aac_get_total_pcm_frame_count(d);
    int16_t *pcm = calloc((size_t)(cap + 4096) * channels, sizeof(*pcm));
    if (!pcm) return 0;
    uint64_t n = 0;
    while (n < cap + 4096) {
        decoder_read_result_t r = aac_read_pcm_frames_s16(d, 1024, pcm + n * channels);
        n += r.frames;
        if (!r.frames && r.status != DECODER_READ_RECOVERABLE_ERROR) break;
    }
    *out = pcm; *frames = n;
    return 1;
}

static int read_all_alac(alac_decoder_t *d, int16_t **out, uint64_t *frames, unsigned channels) {
    uint64_t cap = alac_get_total_pcm_frame_count(d);
    int16_t *pcm = calloc((size_t)(cap + 4096) * channels, sizeof(*pcm));
    if (!pcm) return 0;
    uint64_t n = 0;
    while (n < cap + 4096) {
        decoder_read_result_t r = alac_read_pcm_frames_s16(d, 1024, pcm + n * channels);
        n += r.frames;
        if (!r.frames) break;
    }
    *out = pcm; *frames = n;
    return 1;
}

static int exercise_alac(const char *path, uint64_t expected_total, unsigned expected_rate,
                         const char *reference_path, uint64_t reference_offset) {
    alac_decoder_t *d = alac_open_file(path);
    if (!d) return 10;
    unsigned channels = alac_get_channels(d);
    uint64_t total = alac_get_total_pcm_frame_count(d), n = 0;
    if (total != expected_total || alac_get_sample_rate(d) != expected_rate) return 14;
    int16_t *all = NULL;
    if (!read_all_alac(d, &all, &n, channels) || n != total) return 11;
    if (reference_path) {
        alac_decoder_t *ref = alac_open_file(reference_path);
        if (!ref) return 15;
        unsigned ref_channels = alac_get_channels(ref);
        int16_t *reference = NULL;
        uint64_t reference_frames = 0;
        if (ref_channels != channels || !read_all_alac(ref, &reference, &reference_frames, channels) ||
            reference_offset + 512 > reference_frames || total < 512 ||
            memcmp(all, reference + reference_offset * channels, (size_t)512 * channels * sizeof(*all))) return 16;
        free(reference);
        alac_close(ref);
    }
    const uint64_t points[] = {0, 1, 4095, 4096, 8191, total / 2, total > 17 ? total - 17 : 0};
    int16_t buf[512 * 8];
    for (size_t i = 0; i < sizeof(points)/sizeof(points[0]); i++) {
        uint64_t at = points[i] < total ? points[i] : total;
        if (!alac_seek_to_pcm_frame(d, at)) return 12;
        decoder_read_result_t r = alac_read_pcm_frames_s16(d, 512, buf);
        uint64_t want = total - at < 512 ? total - at : 512;
        if (r.frames != want || memcmp(buf, all + at * channels, (size_t)want * channels * sizeof(*buf))) {
            fprintf(stderr, "ALAC seek mismatch at %llu got %llu expected %llu\n", (unsigned long long)at, (unsigned long long)r.frames, (unsigned long long)want);
            return 13;
        }
    }
    if (!alac_seek_to_pcm_frame(d, total) || alac_read_pcm_frames_s16(d, 1, buf).frames != 0 ||
        !alac_seek_to_pcm_frame(d, total + 100) || alac_read_pcm_frames_s16(d, 1, buf).frames != 0) return 17;
    uint64_t back = total / 2;
    if (!alac_seek_to_pcm_frame(d, back)) return 18;
    decoder_read_result_t back_read = alac_read_pcm_frames_s16(d, 32, buf);
    if (back_read.frames != 32 || memcmp(buf, all + back * channels, (size_t)32 * channels * sizeof(*buf))) return 19;
    free(all); alac_close(d); return 0;
}

static int exercise_aac(const char *path, uint64_t expected_frames) {
    aac_decoder_t *d = aac_open_file_mp4(path);
    if (!d) return 20;
    unsigned channels = aac_get_channels(d);
    uint64_t total = aac_get_total_pcm_frame_count(d), n = 0;
    if (total != expected_frames) {
        fprintf(stderr, "AAC gapless duration %" PRIu64 " expected source duration %" PRIu64 "\n", total, expected_frames);
        return 25;
    }
    int16_t *all = NULL;
    if (!read_all_aac(d, &all, &n, channels) || n != total) { fprintf(stderr, "AAC PCM frames %llu expected %llu\n", (unsigned long long)n, (unsigned long long)total); return 21; }
    const uint64_t points[] = {0, 17, 997, 1023, 1024, 2055, total / 2, total > 17 ? total - 17 : 0};
    int16_t buf[512 * 8];
    for (size_t i = 0; i < sizeof(points)/sizeof(points[0]); i++) {
        uint64_t at = points[i] < total ? points[i] : total;
        if (!aac_seek_to_pcm_frame(d, at)) { fprintf(stderr, "AAC seek failed at %llu\n", (unsigned long long)at); return 22; }
        decoder_read_result_t r = aac_read_pcm_frames_s16(d, 512, buf);
        uint64_t want = total - at < 512 ? total - at : 512;
        if (r.frames != want) return 23;
        /* Identical FAAD2 decoding after two-unit preroll should be very close;
         * this allows codec rounding while rejecting even a shifted waveform. */
        for (uint64_t j = 0; j < want * channels; j++) {
            int delta = (int)buf[j] - all[at * channels + j];
            if (delta < -64 || delta > 64) {
                fprintf(stderr, "AAC seek PCM mismatch at frame %llu sample %llu: delta %d\n",
                        (unsigned long long)at, (unsigned long long)j, delta);
                return 24;
            }
        }
    }
    if (!aac_seek_to_pcm_frame(d, total) || aac_read_pcm_frames_s16(d, 1, buf).frames != 0 ||
        !aac_seek_to_pcm_frame(d, total + 100) || aac_read_pcm_frames_s16(d, 1, buf).frames != 0 ||
        !aac_seek_to_pcm_frame(d, total / 2)) return 35;
    decoder_read_result_t back = aac_read_pcm_frames_s16(d, 32, buf);
    if (back.frames != 32) return 36;
    for (uint64_t j = 0; j < 32 * channels; j++) {
        int delta = (int)buf[j] - all[(total / 2) * channels + j];
        if (delta < -64 || delta > 64) return 37;
    }
    free(all); aac_close(d); return 0;
}

static int exercise_adts(const char *path) {
    aac_decoder_t *d = aac_open_file(path);
    if (!d) return 30;
    unsigned channels = aac_get_channels(d);
    uint64_t total = aac_get_total_pcm_frame_count(d), n = 0;
    int16_t *all = NULL;
    if (!read_all_aac(d, &all, &n, channels) || n != total) {
        fprintf(stderr, "ADTS PCM frames %llu expected %llu\n", (unsigned long long)n, (unsigned long long)total);
        return 31;
    }
    const uint64_t points[] = {0, 17, 1023, 1024, total / 2, total > 17 ? total - 17 : 0};
    int16_t buf[512 * 8];
    for (size_t i = 0; i < sizeof(points)/sizeof(points[0]); i++) {
        uint64_t at = points[i] < total ? points[i] : total;
        if (!aac_seek_to_pcm_frame(d, at)) return 32;
        decoder_read_result_t r = aac_read_pcm_frames_s16(d, 512, buf);
        uint64_t want = total - at < 512 ? total - at : 512;
        if (r.frames != want) return 33;
        for (uint64_t j = 0; j < want * channels; j++) {
            int delta = (int)buf[j] - all[at * channels + j];
            if (delta < -64 || delta > 64) {
                fprintf(stderr, "ADTS seek PCM mismatch at frame %llu sample %llu: delta %d\n",
                        (unsigned long long)at, (unsigned long long)j, delta);
                return 34;
            }
        }
    }
    if (!aac_seek_to_pcm_frame(d, total) || aac_read_pcm_frames_s16(d, 1, buf).frames != 0 ||
        !aac_seek_to_pcm_frame(d, total + 100) || aac_read_pcm_frames_s16(d, 1, buf).frames != 0 ||
        !aac_seek_to_pcm_frame(d, total / 2)) return 35;
    decoder_read_result_t back = aac_read_pcm_frames_s16(d, 32, buf);
    if (back.frames != 32) return 36;
    for (uint64_t j = 0; j < 32 * channels; j++) {
        int delta = (int)buf[j] - all[(total / 2) * channels + j];
        if (delta < -64 || delta > 64) return 37;
    }
    free(all); aac_close(d); return 0;
}

static int check_smpb(const char *path, int expected_valid) {
    mp4_demux_t *d = mp4_demux_open(path);
    if (!d) return 40;
    uint64_t priming = 0, playable = 0;
    int valid = mp4_demux_get_gapless_trim(d, &priming, &playable);
    mp4_demux_close(d);
    if (valid != expected_valid || (valid && (priming == 0 || playable == 0))) return 41;
    if (valid && priming != 1024) return 42;
    return 0;
}

static int check_rejects_large_stts(const char *path) {
    mp4_demux_t *d = mp4_demux_open(path);
    if (d) { mp4_demux_close(d); return 43; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 12) return 2;
    int a = exercise_alac(argv[1], 136710, 44100, NULL, 0);
    if (a) { fprintf(stderr, "ALAC check failed: %d\n", a); return a; }
    int a2 = exercise_alac(argv[2], 136710, 44100, NULL, 0);
    if (a2) { fprintf(stderr, "non-native-timescale ALAC check failed: %d\n", a2); return a2; }
    int a3 = exercise_alac(argv[3], 9600, 96000, NULL, 0);
    if (a3) { fprintf(stderr, "96 kHz ALAC check failed: %d\n", a3); return a3; }
    int a4 = exercise_alac(argv[4], 131710, 44100, argv[1], 5000);
    if (a4) { fprintf(stderr, "large-priming ALAC check failed: %d\n", a4); return a4; }
    int b = exercise_aac(argv[5], strtoull(argv[6], NULL, 10));
    if (b) { fprintf(stderr, "AAC check failed: %d\n", b); return b; }
    int c = exercise_adts(argv[8]);
    if (c) { fprintf(stderr, "ADTS check failed: %d\n", c); return c; }
    int d = exercise_aac(argv[9], strtoull(argv[7], NULL, 10));
    if (d) { fprintf(stderr, "iTunSMPB AAC check failed: %d\n", d); return d; }
    int e = check_smpb(argv[9], 1);
    if (!e) e = check_smpb(argv[10], 0);
    if (!e) e = check_rejects_large_stts(argv[11]);
    if (e) { fprintf(stderr, "iTunSMPB metadata validation failed: %d\n", e); return e; }
    return 0;
}
