#include "ape_decoder.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 6 || (strcmp(argv[5], "s16") && strcmp(argv[5], "s32"))) {
        fprintf(stderr, "usage: %s INPUT OUTPUT START_FRAME FRAME_COUNT s16|s32\n", argv[0]);
        return 2;
    }
    char *end;
    uint64_t start = strtoull(argv[3], &end, 10);
    if (*end) return 2;
    uint64_t count = strtoull(argv[4], &end, 10);
    if (*end || count > SIZE_MAX / (2 * sizeof(int32_t))) return 2;

    ape_decoder_t *decoder = ape_open_file(argv[1]);
    if (!decoder) { fprintf(stderr, "cannot open APE file\n"); return 3; }
    if (!ape_seek_to_pcm_frame(decoder, start)) { ape_close(decoder); fprintf(stderr, "seek failed\n"); return 4; }
    FILE *output = fopen(argv[2], "wb");
    if (!output) { perror("fopen"); ape_close(decoder); return 5; }
    uint64_t frames_left = count;
    int16_t samples16[4096 * 2];
    int32_t samples32[4096 * 2];
    while (frames_left) {
        uint64_t ask = frames_left < 4096 ? frames_left : 4096;
        decoder_read_result_t result = !strcmp(argv[5], "s16")
            ? ape_read_pcm_frames_s16(decoder, ask, samples16)
            : ape_read_pcm_frames_s32(decoder, ask, samples32);
        size_t sample_size = !strcmp(argv[5], "s16") ? sizeof(*samples16) : sizeof(*samples32);
        void *samples = !strcmp(argv[5], "s16") ? (void *)samples16 : (void *)samples32;
        if (result.frames && fwrite(samples, sample_size,
                                    (size_t)result.frames * ape_get_channels(decoder), output) !=
                             (size_t)result.frames * ape_get_channels(decoder)) {
            perror("fwrite"); fclose(output); ape_close(decoder); return 6;
        }
        frames_left -= result.frames;
        if (result.status == DECODER_READ_FATAL_ERROR || !result.frames) {
            fclose(output); ape_close(decoder);
            fprintf(stderr, "short read: wanted %" PRIu64 " more frames, status %d\n", frames_left, result.status);
            return 7;
        }
    }
    fclose(output);
    ape_close(decoder);
    return 0;
}
