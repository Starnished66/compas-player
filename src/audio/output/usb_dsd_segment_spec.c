/* Exercise production DSD timeline conversion through segmented decoders. */
#define AUDIO_DSD_RUNTIME_SPEC 1
#include "../engine/audio.c"

#include <assert.h>

void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void) {}
bool main_loop_discard_pointer_input(void) { return false; }

static decoder_t open_segment(const char *path, uint64_t start, uint64_t count) {
    decoder_t dec = {0};
    dec.type = DECODER_DSD;
    dec.as.dsd = dsd_open_file(path);
    assert(dec.as.dsd);
    dec.channels = dsd_get_channels(dec.as.dsd);
    dec.sample_rate = dsd_get_pcm_sample_rate(dec.as.dsd);
    dec.total_frames = dsd_get_total_pcm_frame_count(dec.as.dsd);
    dec.seekable = true;
    dec.segmented = true;
    dec.segment_start_frame = start;
    dec.segment_frame_count = count;
    assert(dec.channels == 2 && dec.sample_rate == 88200);
    assert(start + count <= dec.total_frames);
    assert(dsd_seek_to_pcm_frame(dec.as.dsd, start));
    return dec;
}

static void compare_read_to_reference(decoder_t *dec, dsd_decoder_t *reference,
                                      uint64_t absolute_start, uint64_t frames) {
    static int32_t actual[40000], expected[40000];
    assert(frames * 2 <= sizeof(actual) / sizeof(actual[0]));
    uint64_t reference_frame = 0;
    assert(dsd_set_dop(reference, true, absolute_start, &reference_frame));
    decoder_read_result_t result = decoder_read_s32(dec, frames, actual);
    decoder_read_result_t reference_result = dsd_read_pcm_frames_s32(reference, frames, expected);
    assert(result.frames == reference_result.frames);
    assert(result.frames == frames);
    assert(memcmp(actual, expected, (size_t)frames * 2 * sizeof(actual[0])) == 0);
}

static void test_segment_to_dop_and_clamp(const char *path) {
    decoder_t dec = open_segment(path, 137, 4099);
    assert(decoder_seek(&dec, 17));
    assert(dec.segment_read_cursor == 17);

    uint64_t frame = UINT64_MAX;
    assert(decoder_set_dop(&dec, true, dec.segment_read_cursor, &frame));
    assert(dec.sample_rate == 352800);
    assert(dec.segment_start_frame == 548);
    assert(dec.segment_frame_count == 16396);
    assert(frame == 68 && dec.segment_read_cursor == 68);
    assert(dec.total_frames == dec.segment_frame_count);

    dsd_decoder_t *reference = dsd_open_file(path);
    assert(reference);
    compare_read_to_reference(&dec, reference, 137 + 17, 16396 - 68);
    decoder_read_result_t eof = decoder_read_s32(&dec, 1, (int32_t[2]){0});
    assert(eof.frames == 0 && eof.status == DECODER_READ_EOF);
    dsd_close(reference);
    dsd_close(dec.as.dsd);
}

static void test_midtrack_fallback_to_pcm(const char *path) {
    decoder_t dec = open_segment(path, 5000, 1000);
    assert(decoder_seek(&dec, 123));
    assert(decoder_set_dop(&dec, true, dec.segment_read_cursor, NULL));
    assert(dec.segment_start_frame == 20000 && dec.segment_frame_count == 4000);

    dsd_decoder_t *dop_reference = dsd_open_file(path);
    assert(dop_reference);
    compare_read_to_reference(&dec, dop_reference, 5000 + 123, 7);
    dsd_close(dop_reference);
    assert(dec.segment_read_cursor == 499);

    uint64_t pcm_frame = UINT64_MAX;
    assert(decoder_set_dop(&dec, false, dec.segment_read_cursor, &pcm_frame));
    assert(dec.sample_rate == 88200);
    assert(dec.segment_start_frame == 5000);
    assert(dec.segment_frame_count == 1000);
    assert(pcm_frame == 124 && dec.segment_read_cursor == 124);
    assert(dec.total_frames == 1000);

    dsd_decoder_t *pcm_reference = dsd_open_file(path);
    assert(pcm_reference);
    assert(dsd_seek_to_pcm_frame(pcm_reference, 5124));
    int32_t actual[20], expected[20];
    decoder_read_result_t result = decoder_read_s32(&dec, 10, actual);
    decoder_read_result_t reference_result = dsd_read_pcm_frames_s32(pcm_reference, 10, expected);
    assert(result.frames == 10 && reference_result.frames == 10);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    dsd_close(pcm_reference);
    dsd_close(dec.as.dsd);
}

static void test_nonsegmented_conversion(const char *path) {
    decoder_t dec = {0};
    dec.type = DECODER_DSD;
    dec.as.dsd = dsd_open_file(path);
    assert(dec.as.dsd);
    dec.sample_rate = dsd_get_pcm_sample_rate(dec.as.dsd);
    dec.total_frames = dsd_get_total_pcm_frame_count(dec.as.dsd);
    dec.seekable = true;
    uint64_t frames_before = dec.total_frames;
    assert(decoder_set_dop(&dec, true, 500, NULL));
    assert(dec.sample_rate == 352800);
    assert(dec.total_frames == dsd_get_total_pcm_frame_count(dec.as.dsd));
    assert(dec.total_frames == frames_before * 4);
    assert(!dec.segmented && dec.segment_start_frame == 0 && dec.segment_read_cursor == 0);
    dsd_close(dec.as.dsd);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_segment_to_dop_and_clamp(argv[1]);
    test_midtrack_fallback_to_pcm(argv[1]);
    test_nonsegmented_conversion(argv[1]);
    puts("USB DSD segmented decoder spec passed");
    return 0;
}
