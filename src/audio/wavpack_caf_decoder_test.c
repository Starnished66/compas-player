#include "caf_decoder.h"
#include "wavpack_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct decoder_api {
    void *(*open)(const char *);
    unsigned int (*channels)(const void *);
    unsigned int (*rate)(const void *);
    uint64_t (*frames)(const void *);
    decoder_read_result_t (*read)(void *, uint64_t, int32_t *);
    bool (*seek)(void *, uint64_t);
    void (*close)(void *);
    bool (*is_alac)(const void *);
} decoder_api_t;

static void *wv_open(const char *path) { return wavpack_open_file(path); }
static unsigned int wv_channels(const void *dec) { return wavpack_get_channels(dec); }
static unsigned int wv_rate(const void *dec) { return wavpack_get_sample_rate(dec); }
static uint64_t wv_frames(const void *dec) { return wavpack_get_total_pcm_frame_count(dec); }
static decoder_read_result_t wv_read(void *dec, uint64_t n, int32_t *out) { return wavpack_read_pcm_frames_s32(dec, n, out); }
static bool wv_seek(void *dec, uint64_t n) { return wavpack_seek_to_pcm_frame(dec, n); }
static void wv_close(void *dec) { wavpack_close(dec); }
static bool wv_alac(const void *dec) { (void)dec; return false; }
static void *caf_open_test(const char *path) { return caf_open_file(path); }
static unsigned int caf_channels_test(const void *dec) { return caf_get_channels(dec); }
static unsigned int caf_rate_test(const void *dec) { return caf_get_sample_rate(dec); }
static uint64_t caf_frames_test(const void *dec) { return caf_get_total_pcm_frame_count(dec); }
static decoder_read_result_t caf_read_test(void *dec, uint64_t n, int32_t *out) { return caf_read_pcm_frames_s32(dec, n, out); }
static bool caf_seek_test(void *dec, uint64_t n) { return caf_seek_to_pcm_frame(dec, n); }
static void caf_close_test(void *dec) { caf_close(dec); }
static bool caf_alac_test(const void *dec) { return caf_is_alac(dec); }

static unsigned int tag_mask;
static void tags_seen(void *user, const char *key, const char *value) {
    (void)user;
    if (!key || !value) return;
    if (strcmp(key, "Title") == 0 || strcmp(key, "TITLE") == 0) {
        if (strcmp(value, "Format test") == 0) tag_mask |= 1U;
    } else if (strcmp(key, "Artist") == 0 || strcmp(key, "ARTIST") == 0) {
        if (strcmp(value, "Compas") == 0) tag_mask |= 2U;
    } else if (strcmp(key, "Album") == 0 || strcmp(key, "ALBUM") == 0) {
        if (strcmp(value, "Pipeline") == 0) tag_mask |= 4U;
    } else if (strcmp(key, "Track") == 0 || strcmp(key, "TRACKNUMBER") == 0) {
        if (strcmp(value, "3") == 0) tag_mask |= 8U;
    }
}

static int check_file(const char *kind, const char *path, int expect_alac, int check_float) {
    const decoder_api_t wv = { wv_open, wv_channels, wv_rate, wv_frames, wv_read, wv_seek, wv_close, wv_alac };
    const decoder_api_t caf = { caf_open_test, caf_channels_test, caf_rate_test, caf_frames_test, caf_read_test, caf_seek_test, caf_close_test, caf_alac_test };
    const decoder_api_t *api = strcmp(kind, "wv") == 0 ? &wv : strcmp(kind, "caf") == 0 ? &caf : NULL;
    if (!api) return 2;
    void *dec = api->open(path);
    if (!dec) {
        fprintf(stderr, "FAIL %s: could not open %s\n", kind, path);
        return 1;
    }
    unsigned int channels = api->channels(dec);
    unsigned int rate = api->rate(dec);
    uint64_t frames = api->frames(dec);
    if (!channels || channels > 8 || !rate || !frames || api->is_alac(dec) != (expect_alac != 0)) {
        fprintf(stderr, "FAIL %s: invalid stream metadata (%u ch, %u Hz, %llu frames, ALAC=%d)\n",
                kind, channels, rate, (unsigned long long)frames, api->is_alac(dec));
        api->close(dec);
        return 1;
    }
    if (frames > SIZE_MAX / channels / sizeof(int32_t)) {
        api->close(dec);
        return 1;
    }
    int32_t *all = malloc((size_t)frames * channels * sizeof(*all));
    if (!all) {
        api->close(dec);
        return 1;
    }
    uint64_t done = 0;
    while (done < frames) {
        uint64_t ask = frames - done;
        if (ask > 997) ask = 997;
        decoder_read_result_t read = api->read(dec, ask, all + (size_t)done * channels);
        if (!read.frames || read.status == DECODER_READ_FATAL_ERROR || read.frames > ask) {
            fprintf(stderr, "FAIL %s: bad sequential read at frame %llu (got %llu status %d)\n", kind,
                    (unsigned long long)done, (unsigned long long)read.frames, (int)read.status);
            free(all); api->close(dec); return 1;
        }
        done += read.frames;
    }
    if (api->read(dec, 1, all).status != DECODER_READ_EOF) {
        fprintf(stderr, "FAIL %s: EOF not reported\n", kind);
        free(all); api->close(dec); return 1;
    }
    if (!api->seek(dec, frames)) {
        fprintf(stderr, "FAIL %s: seek to exact EOF failed\n", kind);
        free(all); api->close(dec); return 1;
    }
    decoder_read_result_t at_eof = api->read(dec, 1, all);
    if (at_eof.frames != 0 || at_eof.status != DECODER_READ_EOF) {
        fprintf(stderr, "FAIL %s: exact EOF seek returned PCM\n", kind);
        free(all); api->close(dec); return 1;
    }
    uint64_t offset = frames / 3;
    uint64_t count = frames - offset;
    if (count > 73) count = 73;
    int32_t seek_samples[73 * 8];
    if (!api->seek(dec, offset)) {
        fprintf(stderr, "FAIL %s: seek to %llu failed\n", kind, (unsigned long long)offset);
        free(all); api->close(dec); return 1;
    }
    decoder_read_result_t sought = api->read(dec, count, seek_samples);
    if (sought.frames != count || memcmp(seek_samples, all + (size_t)offset * channels,
                                         (size_t)count * channels * sizeof(int32_t)) != 0) {
        fprintf(stderr, "FAIL %s: seek read differs from sequential read\n", kind);
        free(all); api->close(dec); return 1;
    }
    if (check_float) {
        int saw_nonzero = 0, saw_min = 0, saw_max = 0;
        for (size_t i = 0; i < (size_t)frames * channels; ++i) {
            if (all[i] < -8388608 || all[i] > 8388607) {
                fprintf(stderr, "FAIL %s: converted float outside S24 range (%d)\n", kind, all[i]);
                free(all); api->close(dec); return 1;
            }
            saw_nonzero |= all[i] != 0;
            saw_min |= all[i] == -8388608;
            saw_max |= all[i] == 8388607;
        }
        if (!saw_nonzero || !saw_min || !saw_max) {
            fprintf(stderr, "FAIL %s: float conversion did not preserve signal and clip out-of-range samples\n", kind);
            free(all); api->close(dec); return 1;
        }
    }
    free(all);
    api->close(dec);
    printf("PASS %s %s (%u ch, %u Hz, %llu frames)\n", kind, path, channels, rate, (unsigned long long)frames);
    return 0;
}

static int compare_pcm(const char *kind, const char *a, const char *b) {
    void *left = NULL, *right = NULL;
    unsigned int channels_a, channels_b;
    uint64_t frames_a, frames_b;
    decoder_read_result_t (*read_a)(void *, uint64_t, int32_t *);
    decoder_read_result_t (*read_b)(void *, uint64_t, int32_t *);
    bool (*seek_a)(void *, uint64_t);
    void (*close_a)(void *);
    void (*close_b)(void *);
    if (strcmp(kind, "wv") == 0) {
        left = wavpack_open_file(a); right = wavpack_open_file(b);
        channels_a = wavpack_get_channels(left); channels_b = wavpack_get_channels(right);
        frames_a = wavpack_get_total_pcm_frame_count(left); frames_b = wavpack_get_total_pcm_frame_count(right);
        read_a = wv_read; read_b = wv_read; seek_a = wv_seek; close_a = wv_close; close_b = wv_close;
    } else {
        left = caf_open_file(a); right = caf_open_file(b);
        channels_a = caf_get_channels(left); channels_b = caf_get_channels(right);
        frames_a = caf_get_total_pcm_frame_count(left); frames_b = caf_get_total_pcm_frame_count(right);
        read_a = caf_read_test; read_b = caf_read_test; seek_a = caf_seek_test;
        close_a = caf_close_test; close_b = caf_close_test;
    }
    if (!left || !right || channels_a != channels_b || frames_a != frames_b) {
        fprintf(stderr, "FAIL %s: comparison streams have different metadata\n", kind);
        if (left) close_a(left);
        if (right) close_b(right);
        return 1;
    }
    if (!seek_a(left, 0) || !seek_a(right, 0)) {
        fprintf(stderr, "FAIL %s: comparison seek failed\n", kind);
        close_a(left); close_b(right); return 1;
    }
    int32_t left_samples[1024 * 8], right_samples[1024 * 8];
    uint64_t done = 0;
    while (done < frames_a) {
        uint64_t count = frames_a - done;
        if (count > 1024) count = 1024;
        decoder_read_result_t ra = read_a(left, count, left_samples);
        decoder_read_result_t rb = read_b(right, count, right_samples);
        if (ra.frames != count || rb.frames != count ||
            memcmp(left_samples, right_samples, (size_t)count * channels_a * sizeof(int32_t)) != 0) {
            fprintf(stderr, "FAIL %s: comparison samples differ at frame %llu\n", kind, (unsigned long long)done);
            close_a(left); close_b(right); return 1;
        }
        done += count;
    }
    close_a(left); close_b(right);
    return 0;
}

static int check_truncated(const char *kind, const char *path) {
    void *dec = strcmp(kind, "wv") == 0 ? (void *)wavpack_open_file(path) : (void *)caf_open_file(path);
    if (!dec) {
        printf("PASS truncated %s is rejected safely\n", kind);
        return 0;
    }
    unsigned int channels = strcmp(kind, "wv") == 0 ? wavpack_get_channels(dec) : caf_get_channels(dec);
    uint64_t total = strcmp(kind, "wv") == 0 ? wavpack_get_total_pcm_frame_count(dec) : caf_get_total_pcm_frame_count(dec);
    decoder_read_result_t (*read_fn)(void *, uint64_t, int32_t *) = strcmp(kind, "wv") == 0 ? wv_read : caf_read_test;
    void (*close_fn)(void *) = strcmp(kind, "wv") == 0 ? wv_close : caf_close_test;
    int32_t scratch[509 * 8];
    if (channels < 1 || channels > 8) { close_fn(dec); return 1; }
    uint64_t frames = 0;
    decoder_read_status_t status = DECODER_READ_OK;
    for (unsigned int attempts = 0; attempts < 10000 && status == DECODER_READ_OK && frames < total; ++attempts) {
        decoder_read_result_t result = read_fn(dec, 509, scratch);
        if (result.frames > 509) { close_fn(dec); return 1; }
        frames += result.frames;
        status = result.status;
        if (!result.frames && status == DECODER_READ_OK) { close_fn(dec); return 1; }
    }
    if (status == DECODER_READ_OK && frames < total) {
        fprintf(stderr, "FAIL truncated %s read did not terminate\n", kind);
        close_fn(dec);
        return 1;
    }
    close_fn(dec);
    printf("PASS truncated %s bounded reads (%llu/%llu frames, status %d)\n", kind,
           (unsigned long long)frames, (unsigned long long)total, (int)status);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 10) {
        fprintf(stderr, "usage: %s wv-integer.wv wv-float.wv caf-pcm.caf caf-float.caf hybrid.wv alac.caf dsd.wv wv-truncated.caf caf-truncated.caf\n", argv[0]);
        return 2;
    }
    int failures = 0;
    failures += check_file("wv", argv[1], 0, 0);
    failures += check_file("wv", argv[2], 0, 1);
    failures += check_file("caf", argv[3], 0, 0);
    failures += check_file("caf", argv[4], 0, 1);
    failures += check_file("wv", argv[5], 0, 0);
    failures += check_file("caf", argv[6], 1, 0);
    failures += check_file("wv", argv[7], 0, 0);
    wavpack_decoder_t *dsd = wavpack_open_file(argv[7]);
    if (!dsd || wavpack_get_channels(dsd) != 2 || wavpack_get_sample_rate(dsd) != 352800 ||
        wavpack_get_bits_per_sample(dsd) != 24 || wavpack_get_total_pcm_frame_count(dsd) != 8192) {
        fprintf(stderr, "FAIL WavPack DSD-as-PCM metadata\n");
        ++failures;
    } else {
        puts("PASS WavPack DSD-as-PCM metadata and seek/read");
    }
    wavpack_close(dsd);
    if (compare_pcm("wv", argv[1], argv[5])) ++failures;
    else puts("PASS WavPack hybrid .wvc restores lossless samples");
    if (compare_pcm("caf", argv[3], argv[6])) ++failures;
    else puts("PASS CAF ALAC matches PCM reference");
    if (check_truncated("wv", argv[8])) ++failures;
    if (check_truncated("caf", argv[9])) ++failures;
    tag_mask = 0;
    wavpack_get_tags(argv[1], tags_seen, NULL);
    if ((tag_mask & 15U) != 15U) { fprintf(stderr, "FAIL WavPack metadata tags mask=%x\n", tag_mask); ++failures; }
    tag_mask = 0;
    caf_get_tags(argv[3], tags_seen, NULL);
    if ((tag_mask & 15U) != 15U) { fprintf(stderr, "FAIL CAF metadata tags mask=%x\n", tag_mask); ++failures; }
    return failures ? 1 : 0;
}
