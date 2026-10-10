#define _POSIX_C_SOURCE 200809L
#include "../src/audio/audio.c"

#include <assert.h>
#include <sys/stat.h>

void install_thread_crash_altstack(void) {}

static void put_u16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }
static void put_u32(FILE *f, uint32_t v) {
    put_u16(f, (uint16_t)v); put_u16(f, (uint16_t)(v >> 16));
}

static void write_marker_wav(const char *path, unsigned rate, unsigned frames,
                             unsigned bits, int16_t marker) {
    FILE *f = fopen(path, "wb"); assert(f);
    unsigned bytes_per_sample = bits / 8;
    unsigned data_bytes = frames * bytes_per_sample;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16); put_u16(f, 1); put_u16(f, 1); put_u32(f, rate);
    put_u32(f, rate * bytes_per_sample); put_u16(f, (uint16_t)bytes_per_sample); put_u16(f, (uint16_t)bits);
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    for (unsigned i = 0; i < frames; ++i) {
        if (bits == 16) put_u16(f, (uint16_t)marker);
        else { /* 24-bit marker, left-aligned representation */
            int32_t sample = (int32_t)marker << 8;
            fputc(sample & 255, f); fputc((sample >> 8) & 255, f); fputc((sample >> 16) & 255, f);
        }
    }
    fclose(f);
}

static void write_ramp_wav(const char *path, unsigned rate, unsigned frames) {
    FILE *f = fopen(path, "wb"); assert(f);
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + frames * 2); fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16); put_u16(f, 1); put_u16(f, 1); put_u32(f, rate);
    put_u32(f, rate * 2); put_u16(f, 2); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, frames * 2);
    for (unsigned i = 0; i < frames; ++i) put_u16(f, (uint16_t)(i + 1));
    fclose(f);
}

static void wait_worker(void) {
    for (unsigned i = 0; i < 5000; ++i) {
        pthread_mutex_lock(&audio_mutex);
        bool done = !ab_switch_worker_active;
        pthread_mutex_unlock(&audio_mutex);
        if (done) return;
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    assert(!"A/B preparation worker timed out");
}

static uint64_t wav_cursor(decoder_t *d) {
    drwav_uint64 frame = 0;
    assert(d->type == DECODER_WAV && drwav_get_cursor_in_pcm_frames(d->as.wav, &frame) == DRWAV_SUCCESS);
    return frame;
}

static void install_current(const char *path, decoder_t *primary) {
    assert(decoder_open(primary, path));
    pthread_mutex_lock(&audio_mutex);
    free(active_path); active_path = strdup(path); assert(active_path);
    have_current = true;
    paused = stop_requested = restart_requested = seek_pending = false;
    crossfade_enabled = ab_loop_enabled = false;
    applied_playback_speed = requested_playback_speed = 1.0;
    playback_generation = 71;
    current_total_frames = primary->total_frames;
    current_sample_rate = primary->sample_rate;
    memset(&current_format_info, 0, sizeof(current_format_info));
    current_format_info.valid = true;
    current_format_info.codec = primary->type == DECODER_MP3 ? AUDIO_CODEC_MP3 :
                                primary->type == DECODER_FLAC ? AUDIO_CODEC_FLAC : AUDIO_CODEC_PCM;
    current_format_info.source_bit_depth = primary->source_bit_depth;
    current_format_info.source_sample_rate = primary->source_sample_rate;
    current_format_info.channels = primary->channels;
    current_format_info.output_sample_rate = primary->sample_rate;
    current_format_info.seekable = true;
    size_t path_len = strlen(path); assert(path_len < sizeof(current_format_info.path));
    memcpy(current_format_info.path, path, path_len + 1);
    ab_switch_requested = ab_switch_ready = ab_switch_select_b = false;
    ab_switch_worker_active = false;
    ab_switch_result = NULL;
    ab_switch_generation = playback_generation;
    ab_switch_epoch = 100;
    atomic_store_explicit(&ab_switch_cancel_token, 0u, memory_order_relaxed);
    pthread_mutex_unlock(&audio_mutex);
}

static void wait_for_result_or_reject(void);

static uint64_t mp3_cursor(decoder_t *d) {
    assert(d->type == DECODER_MP3);
    return d->as.mp3->currentPCMFrame;
}

static uint64_t lossless_cursor(decoder_t *d) {
    if (d->type == DECODER_WAV) return wav_cursor(d);
    assert(d->type == DECODER_FLAC);
    return d->as.flac->currentPCMFrame;
}

static void test_mp3_switch(const char *lossless_path, const char *mp3_path) {
    decoder_t primary = {0};
    install_current(lossless_path, &primary);
    decoder_t probe = {0}; assert(decoder_open(&probe, mp3_path));
    assert(probe.type == DECODER_MP3 && probe.source_bit_depth == 0);
    assert(probe.sample_rate == primary.sample_rate && probe.channels == primary.channels);
    assert(probe.total_frames == primary.total_frames);
    uint64_t encoder_delay = probe.as.mp3->delayInPCMFrames;
    decoder_close(&probe);
    assert(audio_prepare_ab_switch(mp3_path));
    wait_for_result_or_reject();
    ab_switch_job_t *active = NULL;
    assert(decoder_read_s16(&primary, 1234, (int16_t[2468]){0}).frames == 1234);
    ab_switch_adopt(&active, &primary, 1234, playback_generation);
    assert(active && active->decoder.type == DECODER_MP3);
    assert(mp3_cursor(&active->decoder) == 1234 + encoder_delay);
    assert(lossless_cursor(&primary) == 1234);
    assert(audio_select_ab_source(true));
    assert(mp3_cursor(&active->decoder) == 1234 + encoder_delay && lossless_cursor(&primary) == 1234);
    int16_t a[512] = {0}, b[512] = {0};
    assert(decoder_read_s16(&primary, 256, a).frames == 256);
    ab_switch_mix_source(&active, a, b, 256, primary.channels);
    assert(active && mp3_cursor(&active->decoder) == 1490 + encoder_delay);
    assert(audio_select_ab_source(false));
    assert(mp3_cursor(&active->decoder) == 1490 + encoder_delay); /* selection did not move either decoder */
    assert(lossless_cursor(&primary) == 1490);
    audio_clear_ab_switch();
    ab_switch_free_job(active);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void test_mp3_index_cancels_with_ab_epoch(const char *mp3_path) {
    drmp3 scan = {0};
    assert(drmp3_init_file(&scan, mp3_path, NULL));
    drmp3_seek_point points[2] = {{0}};
    unsigned token = atomic_load_explicit(&ab_switch_cancel_token, memory_order_relaxed);
    audio_clear_ab_switch();
    bool cancelled = false;
    uint64_t generation = atomic_load_explicit(&mp3_index_active_generation, memory_order_relaxed);
    drmp3_uint32 count = mp3_build_seek_points_cancellable(&scan,
        drmp3_get_pcm_frame_count(&scan), 2, generation, points, &cancelled,
        &ab_switch_cancel_token, token);
    assert(cancelled && count == 0);
    drmp3_uninit(&scan);
}

static void wait_for_result_or_reject(void) {
    wait_worker();
    pthread_mutex_lock(&audio_mutex);
    bool ready = ab_switch_result != NULL;
    bool requested = ab_switch_requested;
    pthread_mutex_unlock(&audio_mutex);
    assert(ready || !requested);
}

static void test_seamless_switch(const char *a_path, const char *b_path) {
    decoder_t primary = {0};
    install_current(a_path, &primary);
    int16_t pre[64];
    assert(decoder_read_s16(&primary, 64, pre).frames == 64);
    assert(audio_prepare_ab_switch(b_path));
    wait_for_result_or_reject();
    ab_switch_job_t *active = NULL;
    ab_switch_adopt(&active, &primary, 64, playback_generation);
    assert(active && active->decoder.type == DECODER_WAV);
    assert(wav_cursor(&primary) == 64 && wav_cursor(&active->decoder) == 64);
    bool preparing, ready, selected;
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(!preparing && ready && !selected);

    int16_t a[16] = {0}, alternate[16] = {0};
    assert(decoder_read_s16(&primary, 8, a).frames == 8);
    assert(a[0] == 1000);
    ab_switch_mix_source(&active, a, alternate, 8, 1);
    assert(active && a[0] == 1000 && wav_cursor(&active->decoder) == 72);

    assert(audio_select_ab_source(true));
    /* Selecting a source is a flag update; it does not seek or read either decoder. */
    assert(wav_cursor(&primary) == 72 && wav_cursor(&active->decoder) == 72);
    memset(a, 0, sizeof(a));
    assert(decoder_read_s16(&primary, 8, a).frames == 8);
    ab_switch_mix_source(&active, a, alternate, 8, 1);
    assert(active && a[0] == 7000 && wav_cursor(&primary) == 80 && wav_cursor(&active->decoder) == 80);

    assert(audio_select_ab_source(false));
    memset(a, 0, sizeof(a));
    assert(decoder_read_s16(&primary, 8, a).frames == 8);
    ab_switch_mix_source(&active, a, alternate, 8, 1);
    assert(active && a[0] == 1000 && wav_cursor(&primary) == 88 && wav_cursor(&active->decoder) == 88);

    /* End B at its real EOF. The mix helper drops it and leaves primary PCM intact. */
    assert(decoder_seek(&active->decoder, active->decoder.total_frames));
    memset(a, 0, sizeof(a));
    assert(decoder_read_s16(&primary, 8, a).frames == 8);
    int16_t before[8]; memcpy(before, a, sizeof(before));
    ab_switch_mix_source(&active, a, alternate, 8, 1);
    assert(active == NULL && memcmp(a, before, sizeof(before)) == 0);
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(!preparing && !ready && !selected);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void assert_rejected_candidate_untouched(const char *a_path, const char *candidate_path) {
    decoder_t primary = {0};
    install_current(a_path, &primary);
    int16_t seed[23]; assert(decoder_read_s16(&primary, 23, seed).frames == 23);
    uint64_t before = wav_cursor(&primary);
    assert(audio_prepare_ab_switch(candidate_path));
    wait_for_result_or_reject();
    ab_switch_job_t *active = NULL;
    ab_switch_adopt(&active, &primary, before, playback_generation);
    assert(active == NULL && wav_cursor(&primary) == before);
    bool preparing, ready, selected;
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(!preparing && !ready && !selected);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void test_stale_epoch(const char *a_path, const char *b_path) {
    decoder_t primary = {0}; install_current(a_path, &primary);
    uint64_t before = wav_cursor(&primary);
    assert(audio_prepare_ab_switch(b_path)); wait_for_result_or_reject();
    pthread_mutex_lock(&audio_mutex); assert(ab_switch_result); pthread_mutex_unlock(&audio_mutex);
    audio_clear_ab_switch(); /* invalidate a completed but not-yet-adopted result */
    ab_switch_job_t *active = NULL;
    ab_switch_adopt(&active, &primary, before, playback_generation);
    assert(active == NULL && wav_cursor(&primary) == before);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void test_stale_generation(const char *a_path, const char *b_path) {
    decoder_t primary = {0}; install_current(a_path, &primary);
    uint64_t generation = playback_generation;
    assert(audio_prepare_ab_switch(b_path)); wait_for_result_or_reject();
    pthread_mutex_lock(&audio_mutex);
    assert(ab_switch_result);
    playback_generation++;
    pthread_mutex_unlock(&audio_mutex);
    ab_switch_job_t *active = NULL;
    ab_switch_adopt(&active, &primary, 0, generation);
    assert(active == NULL && wav_cursor(&primary) == 0);
    bool preparing, ready, selected;
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(!preparing && !ready && !selected);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex);
    have_current = false; stop_requested = false; restart_requested = false;
    playback_generation = generation; free(active_path); active_path = NULL;
    pthread_mutex_unlock(&audio_mutex);
}

static void test_read_error_falls_back_to_a(const char *a_path, const char *b_path) {
    decoder_t primary = {0}; install_current(a_path, &primary);
    assert(audio_prepare_ab_switch(b_path)); wait_for_result_or_reject();
    ab_switch_job_t *active = NULL; ab_switch_adopt(&active, &primary, 0, playback_generation);
    assert(active);
    active->decoder.type = (decoder_type_t)99; /* decoder_read_s16 returns FATAL_ERROR */
    int16_t a[8] = {0}, alternate[8] = {0};
    assert(decoder_read_s16(&primary, 8, a).frames == 8);
    int16_t expected[8]; memcpy(expected, a, sizeof(expected));
    ab_switch_mix_source(&active, a, alternate, 8, 1);
    assert(active == NULL && memcmp(a, expected, sizeof(a)) == 0);
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void test_sample_accurate_loop(const char *path) {
    decoder_t primary = {0}; install_current(path, &primary);
    assert(primary.sample_rate == 44100 && primary.total_frames == 4410);
    assert(audio_set_ab_loop(0.001, 0.002));
    double start = 0.0, end = 0.0;
    assert(audio_get_ab_loop(&start, &end) && start == 0.001 && end == 0.002);
    assert(!audio_set_ab_loop(0.0, 0.101));
    audio_set_crossfade_enabled(true);
    assert(!audio_get_ab_loop(&start, &end));
    assert(!audio_set_ab_loop(0.001, 0.002));
    audio_set_crossfade_enabled(false);
    assert(audio_set_ab_loop(0.001, 0.002));
    assert(audio_get_ab_loop(&start, &end));

    audio_toggle_pause();
    assert(!audio_get_ab_loop(&start, &end)); /* pause clears the active loop */
    audio_toggle_pause();
    pthread_mutex_lock(&audio_mutex); stop_requested = true; pthread_mutex_unlock(&audio_mutex);
    assert(!audio_set_ab_loop(0.001, 0.002));
    pthread_mutex_lock(&audio_mutex); stop_requested = false; restart_requested = true; pthread_mutex_unlock(&audio_mutex);
    assert(!audio_set_ab_loop(0.001, 0.002));
    pthread_mutex_lock(&audio_mutex); restart_requested = false; seek_pending = true; pthread_mutex_unlock(&audio_mutex);
    assert(!audio_set_ab_loop(0.001, 0.002));
    pthread_mutex_lock(&audio_mutex); seek_pending = false; applied_playback_speed = 1.25; pthread_mutex_unlock(&audio_mutex);
    assert(!audio_set_ab_loop(0.001, 0.002));
    pthread_mutex_lock(&audio_mutex); applied_playback_speed = 1.0; requested_playback_speed = 1.25;
    const char *slash = strrchr(path, '/'); assert(slash);
    size_t dir_len = (size_t)(slash - path); assert(dir_len < sizeof(speed_directory));
    memcpy(speed_directory, path, dir_len); speed_directory[dir_len] = '\0';
    pthread_mutex_unlock(&audio_mutex);
    assert(!audio_set_ab_loop(0.001, 0.002)); /* requested directory tempo blocks the loop */
    pthread_mutex_lock(&audio_mutex); speed_directory[0] = '\0'; requested_playback_speed = 1.0; pthread_mutex_unlock(&audio_mutex);
    assert(audio_set_ab_loop(0.001, 0.002));
    assert(audio_get_ab_loop(&start, &end));

    uint64_t loop_start = 0, loop_end = 0;
    bool valid_frames = audio_ab_loop_frames(start, end, primary.sample_rate, primary.total_frames,
                                              &loop_start, &loop_end);
    if (!valid_frames) fprintf(stderr, "loop frame conversion failed: %.17g..%.17g @ %u / %llu\n",
                               start, end, primary.sample_rate, (unsigned long long)primary.total_frames);
    assert(valid_frames);
    assert(loop_start == 44 && loop_end == 88);
    assert(decoder_seek(&primary, loop_start));
    int16_t first_pass[44], second_pass[44];
    uint64_t count = audio_ab_loop_read_frames(loop_start, loop_end, 32);
    assert(count == 32);
    assert(decoder_read_s16(&primary, count, first_pass).frames == count);
    assert(wav_cursor(&primary) == 76 && first_pass[0] == 45 && first_pass[31] == 76);
    count = audio_ab_loop_read_frames(wav_cursor(&primary), loop_end, 32);
    assert(count == 12);
    int16_t tail[12];
    assert(decoder_read_s16(&primary, count, tail).frames == count);
    assert(wav_cursor(&primary) == loop_end && tail[0] == 77 && tail[11] == 88);
    assert(audio_ab_loop_read_frames(loop_end, loop_end, 32) == 0);
    assert(decoder_seek(&primary, loop_start));
    assert(wav_cursor(&primary) == loop_start);
    assert(decoder_read_s16(&primary, 44, second_pass).frames == 44);
    int16_t original[44]; memcpy(original, first_pass, 32 * sizeof(int16_t));
    memcpy(original + 32, tail, sizeof(tail));
    assert(memcmp(original, second_pass, sizeof(original)) == 0);
    audio_clear_ab_loop();
    assert(!audio_get_ab_loop(&start, &end));
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex); have_current = false; free(active_path); active_path = NULL; pthread_mutex_unlock(&audio_mutex);
}

static void test_seek_invalidates_prepared_switch(const char *a_path, const char *b_path) {
    decoder_t primary = {0}; install_current(a_path, &primary);
    assert(audio_prepare_ab_switch(b_path)); wait_for_result_or_reject();
    ab_switch_job_t *active = NULL;
    ab_switch_adopt(&active, &primary, 0, playback_generation);
    assert(active);
    bool preparing, ready, selected;
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(ready && !preparing && !selected);

    audio_seek(0.001); /* Public request invalidates the A/B epoch synchronously. */
    pthread_mutex_lock(&audio_mutex);
    assert(seek_pending && seek_pending_frame == 44);
    uint64_t requested_frame = seek_pending_frame;
    pthread_mutex_unlock(&audio_mutex);

    /* Model the audio owner's decoder seek/command consumption boundary. */
    assert(decoder_seek(&primary, requested_frame));
    pthread_mutex_lock(&audio_mutex);
    seek_pending = false;
    pthread_mutex_unlock(&audio_mutex);
    ab_switch_adopt(&active, &primary, requested_frame, playback_generation);
    assert(active == NULL && wav_cursor(&primary) == requested_frame);
    audio_get_ab_switch(&preparing, &ready, &selected);
    assert(!preparing && !ready && !selected);
    assert(!audio_select_ab_source(true));
    decoder_close(&primary);
    pthread_mutex_lock(&audio_mutex);
    have_current = false; stop_requested = false; restart_requested = false;
    seek_pending = false; free(active_path); active_path = NULL;
    pthread_mutex_unlock(&audio_mutex);
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 4);
    char a[PATH_MAX], b[PATH_MAX], rate[PATH_MAX], count[PATH_MAX], depth[PATH_MAX], loop[PATH_MAX];
    snprintf(a, sizeof(a), "%s/a.wav", argv[1]); snprintf(b, sizeof(b), "%s/b.wav", argv[1]);
    snprintf(rate, sizeof(rate), "%s/rate.wav", argv[1]); snprintf(count, sizeof(count), "%s/count.wav", argv[1]);
    snprintf(depth, sizeof(depth), "%s/depth.wav", argv[1]);
    snprintf(loop, sizeof(loop), "%s/loop.wav", argv[1]);
    write_marker_wav(a, 44100, 128, 16, 1000); write_marker_wav(b, 44100, 128, 16, 7000);
    write_marker_wav(rate, 48000, 128, 16, 4000); write_marker_wav(count, 44100, 120, 16, 5000);
    write_marker_wav(depth, 44100, 128, 24, 6000);
    write_ramp_wav(loop, 44100, 4410);
    test_seamless_switch(a, b);
    assert_rejected_candidate_untouched(a, rate);
    assert_rejected_candidate_untouched(a, count);
    assert_rejected_candidate_untouched(a, depth);
    test_stale_epoch(a, b);
    test_stale_generation(a, b);
    test_seek_invalidates_prepared_switch(a, b);
    test_read_error_falls_back_to_a(a, b);
    test_sample_accurate_loop(loop);
    if (argc == 4) {
        char wav[PATH_MAX], flac[PATH_MAX];
        snprintf(wav, sizeof(wav), "%s/source.wav", argv[1]);
        snprintf(flac, sizeof(flac), "%s/source.flac", argv[1]);
        test_mp3_switch(wav, argv[2]);
        test_mp3_switch(flac, argv[3]);
        test_mp3_index_cancels_with_ab_epoch(argv[2]);
    }
    puts("Native A/B switch and sample-accurate loop decoder paths: PASS");
    return 0;
}
