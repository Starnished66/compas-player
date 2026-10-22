#include "dsd_decoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_all(dsd_decoder_t *d, uint64_t frames, int32_t *out) {
    uint64_t done = 0;
    while (done < frames) {
        uint64_t ask = frames - done;
        if (ask > 733) ask = 733;
        decoder_read_result_t r = dsd_read_pcm_frames_s32(d, ask, out + done * dsd_get_channels(d));
        if (!r.frames || r.status == DECODER_READ_FATAL_ERROR || r.frames > ask) return 0;
        done += r.frames;
    }
    return 1;
}

static int check_pair(const char *source, const char *packed) {
    dsd_decoder_t *a = dsd_open_file(source), *b = dsd_open_file(packed);
    if (!a || !b) {
        fprintf(stderr, "FAIL open pair %s / %s\n", source, packed);
        dsd_close(a); dsd_close(b); return 1;
    }
    unsigned ch = dsd_get_channels(a), rate = dsd_get_source_sample_rate(a);
    uint64_t pcm_frames = dsd_get_total_pcm_frame_count(a);
    if (ch != 2 || dsd_get_channels(b) != ch || dsd_get_source_sample_rate(b) != rate ||
        dsd_get_total_pcm_frame_count(b) != pcm_frames || pcm_frames < 64) {
        fprintf(stderr, "FAIL metadata parity (%s / %s)\n", source, packed);
        dsd_close(a); dsd_close(b); return 1;
    }
    int32_t *pcm_a = calloc((size_t)pcm_frames * ch, sizeof(*pcm_a));
    int32_t *pcm_b = calloc((size_t)pcm_frames * ch, sizeof(*pcm_b));
    if (!pcm_a || !pcm_b || !read_all(a, pcm_frames, pcm_a) || !read_all(b, pcm_frames, pcm_b) ||
        memcmp(pcm_a, pcm_b, (size_t)pcm_frames * ch * sizeof(*pcm_a)) != 0) {
        fprintf(stderr, "FAIL PCM samples differ (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    /* Seeking well past the start checks the filter-history warmup path. */
    uint64_t seek = pcm_frames / 2 + 7, out_a = 0, out_b = 0;
    int32_t seek_a[73 * 2], seek_b[73 * 2];
    uint64_t count = pcm_frames - seek < 41 ? pcm_frames - seek : 41;
    if (!dsd_seek_to_pcm_frame(a, seek) || !dsd_seek_to_pcm_frame(b, seek) ||
        dsd_read_pcm_frames_s32(a, count, seek_a).frames != count ||
        dsd_read_pcm_frames_s32(b, count, seek_b).frames != count ||
        memcmp(seek_a, seek_b, (size_t)count * ch * sizeof(*seek_a)) != 0 ||
        memcmp(seek_a, pcm_a + seek * ch, (size_t)count * ch * sizeof(*seek_a)) != 0) {
        fprintf(stderr, "FAIL PCM seek/warmup parity (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    const uint64_t later_seek = 3072;
    if (pcm_frames > later_seek + 64 &&
        (!dsd_seek_to_pcm_frame(a, later_seek) || !dsd_seek_to_pcm_frame(b, later_seek) ||
         dsd_read_pcm_frames_s32(a, 64, seek_a).frames != 64 ||
         dsd_read_pcm_frames_s32(b, 64, seek_b).frames != 64 ||
         memcmp(seek_a, pcm_a + later_seek * ch, 64 * ch * sizeof(*seek_a)) != 0 ||
         memcmp(seek_b, pcm_b + later_seek * ch, 64 * ch * sizeof(*seek_b)) != 0)) {
        fprintf(stderr, "FAIL clean-context PCM seek at frame %llu (%s / %s)\n",
                (unsigned long long)later_seek, source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    if (!dsd_seek_to_pcm_frame(a, pcm_frames) || !dsd_seek_to_pcm_frame(b, pcm_frames) ||
        dsd_read_pcm_frames_s32(a, 1, seek_a).status != DECODER_READ_EOF ||
        dsd_read_pcm_frames_s32(b, 1, seek_b).status != DECODER_READ_EOF) {
        fprintf(stderr, "FAIL PCM exact EOF seek (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    if (!dsd_set_dop(a, true, 0, &out_a) || !dsd_set_dop(b, true, 0, &out_b) ||
        out_a != 0 || out_b != 0 || dsd_get_dop_sample_rate(a) != rate / 16 ||
        dsd_get_dop_sample_rate(b) != rate / 16) {
        fprintf(stderr, "FAIL DoP mode setup (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    uint64_t dop_frames = dsd_get_total_pcm_frame_count(a);
    int32_t dop_a[1024 * 2], dop_b[1024 * 2];
    if (dsd_get_total_pcm_frame_count(b) != dop_frames || dop_frames < 32) {
        fprintf(stderr, "FAIL DoP frame count (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    if (!dsd_seek_to_pcm_frame(a, dop_frames) || !dsd_seek_to_pcm_frame(b, dop_frames) ||
        dsd_read_pcm_frames_s32(a, 1, dop_a).status != DECODER_READ_EOF ||
        dsd_read_pcm_frames_s32(b, 1, dop_b).status != DECODER_READ_EOF) {
        fprintf(stderr, "FAIL DoP exact EOF seek (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    if (!dsd_seek_to_pcm_frame(a, 0) || !dsd_seek_to_pcm_frame(b, 0)) {
        fprintf(stderr, "FAIL DoP rewind after EOF (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    for (uint64_t pos = 0; pos < dop_frames;) {
        uint64_t n = dop_frames - pos > 1024 ? 1024 : dop_frames - pos;
        if (dsd_read_pcm_frames_s32(a, n, dop_a).frames != n ||
            dsd_read_pcm_frames_s32(b, n, dop_b).frames != n ||
            memcmp(dop_a, dop_b, (size_t)n * ch * sizeof(*dop_a)) != 0) {
            fprintf(stderr, "FAIL DoP frame/marker parity at %llu (%s / %s)\n",
                    (unsigned long long)pos, source, packed);
            free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
        }
        for (uint64_t i = 0; i < n * ch; ++i) {
            unsigned marker = ((uint32_t)dop_a[i] >> 16) & 255U;
            uint64_t frame = pos + i / ch;
            if ((marker != 0x05 && marker != 0xfa) || marker != ((frame & 1) ? 0xfa : 0x05)) {
                fprintf(stderr, "FAIL DoP marker at frame %llu\n", (unsigned long long)frame);
                free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
            }
        }
        pos += n;
    }
    uint64_t dop_seek = dop_frames / 2, target_a = 0, target_b = 0;
    int32_t small_a[19 * 2], small_b[19 * 2];
    uint64_t n = dop_frames - dop_seek < 19 ? dop_frames - dop_seek : 19;
    if (!dsd_seek_to_pcm_frame(a, dop_seek) || !dsd_seek_to_pcm_frame(b, dop_seek) ||
        dsd_read_pcm_frames_s32(a, n, small_a).frames != n ||
        dsd_read_pcm_frames_s32(b, n, small_b).frames != n ||
        memcmp(small_a, small_b, (size_t)n * ch * sizeof(*small_a)) != 0) {
        fprintf(stderr, "FAIL DoP seek parity (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    /* Switching back at a corresponding position must preserve PCM parity. */
    if (!dsd_set_dop(a, false, dop_seek, &target_a) || !dsd_set_dop(b, false, dop_seek, &target_b) ||
        target_a != target_b || target_a >= pcm_frames ||
        dsd_read_pcm_frames_s32(a, 16, small_a).frames != 16 ||
        dsd_read_pcm_frames_s32(b, 16, small_b).frames != 16 ||
        memcmp(small_a, small_b, 16 * ch * sizeof(*small_a)) != 0) {
        fprintf(stderr, "FAIL DoP-to-PCM transition parity (%s / %s)\n", source, packed);
        free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b); return 1;
    }
    printf("PASS DSD/WavPack %u Hz, stereo, %llu PCM and %llu DoP frames: %s\n", rate,
           (unsigned long long)pcm_frames, (unsigned long long)dop_frames, packed);
    free(pcm_a); free(pcm_b); dsd_close(a); dsd_close(b);
    return 0;
}

static int check_damaged(const char *path) {
    dsd_decoder_t *d = dsd_open_file(path);
    if (!d) {
        fprintf(stderr, "FAIL damaged WavPack rejected at open before read testing: %s\n", path);
        return 1;
    }
    unsigned ch = dsd_get_channels(d);
    uint64_t total = dsd_get_total_pcm_frame_count(d), done = 0;
    int32_t scratch[509 * 8];
    decoder_read_status_t status = DECODER_READ_OK;
    for (unsigned attempts = 0; attempts < 10000 && done < total && status == DECODER_READ_OK; ++attempts) {
        decoder_read_result_t r = dsd_read_pcm_frames_s32(d, 509, scratch);
        if (r.frames > 509 || ch == 0 || ch > 8 || (!r.frames && r.status == DECODER_READ_OK)) {
            fprintf(stderr, "FAIL damaged WavPack made no bounded progress: %s\n", path);
            dsd_close(d); return 1;
        }
        done += r.frames;
        status = r.status;
    }
    dsd_close(d);
    if (!done || status != DECODER_READ_FATAL_ERROR) {
        fprintf(stderr, "FAIL damaged WavPack did not report a fatal read (read %llu/%llu, status %d): %s\n",
                (unsigned long long)done, (unsigned long long)total, (int)status, path);
        return 1;
    }
    d = dsd_open_file(path);
    if (!d) {
        fprintf(stderr, "FAIL damaged WavPack could not be reopened for failed-seek checks: %s\n", path);
        return 1;
    }
    total = dsd_get_total_pcm_frame_count(d);
    done = 0;
    status = DECODER_READ_OK;
    for (unsigned attempts = 0; attempts < 10000 && done < total && status == DECODER_READ_OK; ++attempts) {
        decoder_read_result_t r = dsd_read_pcm_frames_s32(d, 509, scratch);
        done += r.frames;
        status = r.status;
    }
    bool duration_preserved = total == dsd_get_total_pcm_frame_count(d);
    bool seek_zero_fails = !dsd_seek_to_pcm_frame(d, 0);
    bool seek_eof_fails = !dsd_seek_to_pcm_frame(d, total);
    decoder_read_result_t after_failed_seek = dsd_read_pcm_frames_s32(d, 1, scratch);
    dsd_close(d);
    if (!duration_preserved || !seek_zero_fails || !seek_eof_fails ||
        after_failed_seek.status != DECODER_READ_FATAL_ERROR) {
        fprintf(stderr, "FAIL damaged WavPack state after fatal read (duration=%d seek0=%d seekEOF=%d status=%d): %s\n",
                duration_preserved, seek_zero_fails, seek_eof_fails,
                (int)after_failed_seek.status, path);
        return 1;
    }
    printf("PASS damaged WavPack preserves duration and rejects seeks after fatal read: %s\n", path);
    printf("PASS damaged WavPack reports fatal read after %llu frames: %s\n",
           (unsigned long long)done, path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 6 || ((argc - 4) % 2) != 0) {
        fprintf(stderr, "usage: %s PCM.wv truncated.wv corrupt.wv DSD-source WV [DSD-source WV ...]\n", argv[0]); return 2;
    }
    int failed = check_damaged(argv[2]) | check_damaged(argv[3]);
    for (int i = 4; i < argc; i += 2) failed |= check_pair(argv[i], argv[i + 1]);
    dsd_decoder_t *pcm_as_dsd = dsd_open_file(argv[1]);
    if (pcm_as_dsd) {
        fprintf(stderr, "FAIL PCM WavPack was accepted as DSD\n");
        dsd_close(pcm_as_dsd);
        return 1;
    }
    puts("PASS PCM WavPack rejected by DSD decoder");
    return failed ? 1 : 0;
}
