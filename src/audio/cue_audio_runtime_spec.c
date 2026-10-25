#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "cue_parser.h"
#include "file_browser.h"
#include "metadata.h"
#include <SDL.h>

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The application entry point is in main.c, which this runtime spec excludes. */
void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void) {}
bool main_loop_discard_pointer_input(void) { return false; }

extern int __real_SDL_QueueAudio(SDL_AudioDeviceID, const void *, Uint32);
extern Uint32 __real_SDL_GetQueuedAudioSize(SDL_AudioDeviceID);
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool capture_enabled;
static bool stall_after_first_chunk;
static int16_t captured[44100 * 2 * 4];
static size_t captured_samples;

int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 length) {
    pthread_mutex_lock(&capture_mutex);
    if (capture_enabled) {
        size_t count = length / sizeof(int16_t);
        if (count > sizeof(captured) / sizeof(captured[0]) - captured_samples)
            count = sizeof(captured) / sizeof(captured[0]) - captured_samples;
        memcpy(captured + captured_samples, data, count * sizeof(int16_t));
        captured_samples += count;
    }
    pthread_mutex_unlock(&capture_mutex);
    return __real_SDL_QueueAudio(device, data, length);
}

Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID device) {
    pthread_mutex_lock(&capture_mutex);
    bool stall = stall_after_first_chunk && captured_samples > 0;
    pthread_mutex_unlock(&capture_mutex);
    return stall ? UINT32_MAX : __real_SDL_GetQueuedAudioSize(device);
}

static long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pause_ms(long ms) {
    struct timespec delay = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&delay, NULL);
}

static bool wait_format(const char *path, audio_current_format_info_t *info, int timeout_ms) {
    long long end = now_ms() + timeout_ms;
    do {
        if (audio_get_current_format_info(info) && strcmp(info->path, path) == 0) return true;
        pause_ms(5);
    } while (now_ms() < end);
    return false;
}

static bool wait_event(bool (*consume)(void), int timeout_ms) {
    long long end = now_ms() + timeout_ms;
    do {
        if (consume()) return true;
        pause_ms(5);
    } while (now_ms() < end);
    return false;
}

static void make_wav(const char *path, const int16_t *samples, uint32_t frames) {
    enum { RATE = 44100, CHANNELS = 2 };
    uint32_t data_size = frames * CHANNELS * (uint32_t)sizeof(int16_t);
    unsigned char header[44] = {0};
    memcpy(header, "RIFF", 4);
    uint32_t riff_size = 36 + data_size;
    memcpy(header + 4, &riff_size, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(header + 16, &fmt_size, 4);
    uint16_t pcm = 1, channels = CHANNELS, align = CHANNELS * 2, bits = 16;
    memcpy(header + 20, &pcm, 2);
    memcpy(header + 22, &channels, 2);
    uint32_t rate = RATE, byte_rate = RATE * align;
    memcpy(header + 24, &rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &align, 2);
    memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_size, 4);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fwrite(samples, sizeof(int16_t), (size_t)frames * CHANNELS, file) == (size_t)frames * CHANNELS);
    assert(fclose(file) == 0);
}

static void write_cue(const char *path, const char *source_path, const char *second_path) {
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file,
        "PERFORMER \"Cue Performer\"\n"
        "TITLE \"Runtime Album\"\n"
        "FILE \"%s\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    TITLE \"Opening\"\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    TITLE \"Middle\"\n"
        "    INDEX 01 00:00:30\n"
        "  TRACK 03 AUDIO\n"
        "    TITLE \"Closing\"\n"
        "    INDEX 01 00:01:15\n"
        "FILE \"%s\" WAVE\n"
        "  TRACK 04 AUDIO\n"
        "    TITLE \"Second File\"\n"
        "    INDEX 01 00:00:00\n", source_path, second_path);
    assert(fclose(file) == 0);
}

static void make_segmented_source(const char *path) {
    enum { FRAMES = 44100 * 2 };
    int16_t *pcm = calloc((size_t)FRAMES * 2, sizeof(*pcm));
    assert(pcm);
    const int16_t levels[] = { -12000, 4000, 9000 };
    const uint32_t boundaries[] = { 0, 44100 * 2 / 5, 44100 * 6 / 5, FRAMES };
    for (size_t segment = 0; segment < 3; ++segment)
        for (uint32_t frame = boundaries[segment]; frame < boundaries[segment + 1]; ++frame) {
            pcm[frame * 2] = levels[segment];
            pcm[frame * 2 + 1] = levels[segment];
        }
    make_wav(path, pcm, FRAMES);
    free(pcm);
}

static void make_second_source(const char *path) {
    enum { FRAMES = 44100 * 3 / 5 };
    int16_t *pcm = malloc((size_t)FRAMES * 2 * sizeof(*pcm));
    assert(pcm);
    for (uint32_t i = 0; i < FRAMES * 2; ++i) pcm[i] = 12000;
    make_wav(path, pcm, FRAMES);
    free(pcm);
}

static void reference(char *out, size_t cap, const char *cue_path, unsigned ordinal) {
    assert(cue_reference_format(cue_path, ordinal, out, cap));
}

static void test_m3u_reference_roundtrip(const char *path, const char *first, const char *second) {
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "#EXTM3U\n%s\n%s\n", first, second);
    assert(fclose(file) == 0);
    char **paths = NULL;
    int count = 0;
    assert(file_browser_build_playlist_from_m3u(path, &paths, &count));
    assert(count == 2);
    assert(strcmp(paths[0], first) == 0);
    assert(strcmp(paths[1], second) == 0);
    for (int i = 0; i < count; ++i) free(paths[i]);
    free(paths);
}

static void stop_and_wait(void) {
    audio_stop();
    long long end = now_ms() + 5000;
    while (!audio_is_idle() && now_ms() < end) pause_ms(5);
    assert(audio_is_idle());
}

int main(int argc, char **argv) {
    assert(argc == 4);
    const char *wav = argv[1], *cue = argv[2], *second = argv[3];
    make_segmented_source(wav);
    make_second_source(second);
    write_cue(cue, wav, second);

    char middle[1200], closing[1200], second_ref[1200];
    reference(middle, sizeof(middle), cue, 2);
    reference(closing, sizeof(closing), cue, 3);
    reference(second_ref, sizeof(second_ref), cue, 4);

    track_metadata_t tags = {0};
    metadata_read_without_artwork(middle, &tags);
    assert(tags.has_title && strcmp(tags.title, "Middle") == 0);
    assert(tags.has_artist && strcmp(tags.artist, "Cue Performer") == 0);
    assert(tags.has_album && strcmp(tags.album, "Runtime Album") == 0);
    assert(tags.has_track_number && tags.track_number == 2);
    free(tags.picture_data);
    free(tags.lyrics);

    uint8_t cue_waveform[8], file_waveform[10];
    assert(audio_extract_waveform(middle, cue_waveform, sizeof(cue_waveform), NULL, NULL));
    assert(audio_extract_waveform(wav, file_waveform, sizeof(file_waveform), NULL, NULL));
    for (size_t i = 0; i < sizeof(cue_waveform); ++i) assert(cue_waveform[i] == 255);
    assert(file_waveform[2] > 0 && file_waveform[2] < 255);
    bool file_waveform_has_peak = false;
    for (size_t i = 0; i < sizeof(file_waveform); ++i)
        if (file_waveform[i] == 255) file_waveform_has_peak = true;
    assert(file_waveform_has_peak);
    char m3u[1200];
    snprintf(m3u, sizeof(m3u), "%s.runtime.m3u", cue);
    test_m3u_reference_roundtrip(m3u, middle, closing);

    audio_init();
    audio_set_volume(1.0f);
    audio_play_file_at(wav, 0, false, 0, false, 0);
    audio_current_format_info_t info;
    assert(wait_format(wav, &info, 5000));
    assert(fabs(audio_get_duration_seconds() - 2.0) < 0.002);
    stop_and_wait();

    pthread_mutex_lock(&capture_mutex);
    captured_samples = 0;
    capture_enabled = true;
    stall_after_first_chunk = true;
    pthread_mutex_unlock(&capture_mutex);
    audio_play_file_at(middle, 0, false, 0, false, 0);
    assert(wait_format(middle, &info, 5000));
    assert(fabs(audio_get_duration_seconds() - 0.8) < 0.002);
    audio_toggle_pause();
    assert(audio_is_paused());
    pause_ms(50); /* release the bounded queue wait in the wrapped SDL backend */
    assert(audio_get_position_seconds() > 0.05 && audio_get_position_seconds() < 0.15);
    audio_seek(0.25);
    pause_ms(100);
    assert(fabs(audio_get_position_seconds() - 0.25) < 0.02);
    pthread_mutex_lock(&capture_mutex);
    capture_enabled = false;
    stall_after_first_chunk = false;
    size_t sample_count = captured_samples;
    bool saw_middle = false, saw_outside = false;
    for (size_t i = 0; i < sample_count; ++i) {
        int value = captured[i];
        if (value >= 3500 && value <= 4500) saw_middle = true;
        if (value >= 8500 || value <= -8500) saw_outside = true;
    }
    pthread_mutex_unlock(&capture_mutex);
    assert(saw_middle);
    assert(!saw_outside); /* no neighboring CUE audio leaked into this virtual track */
    stop_and_wait();

    pthread_mutex_lock(&capture_mutex);
    captured_samples = 0;
    capture_enabled = true;
    stall_after_first_chunk = false;
    pthread_mutex_unlock(&capture_mutex);
    audio_play_file_at(middle, 0, false, 0, false, 0);
    assert(wait_event(audio_consume_track_finished, 2500));
    pthread_mutex_lock(&capture_mutex);
    capture_enabled = false;
    sample_count = captured_samples;
    saw_middle = false;
    saw_outside = false;
    for (size_t i = 0; i < sample_count; ++i) {
        int value = captured[i];
        if (value >= 3500 && value <= 4500) saw_middle = true;
        if (value >= 8500 || value <= -8500) saw_outside = true;
    }
    pthread_mutex_unlock(&capture_mutex);
    assert(sample_count == (size_t)(35280 * 2));
    assert(saw_middle && !saw_outside);
    stop_and_wait();

    pthread_mutex_lock(&capture_mutex);
    stall_after_first_chunk = true;
    pthread_mutex_unlock(&capture_mutex);
    audio_play_file_at(middle, 0, false, 0, false, 0);
    audio_set_next_track(closing, false, 0, false, 0);
    assert(wait_format(middle, &info, 5000));
    assert(fabs(audio_get_duration_seconds() - 0.8) < 0.002);
    pthread_mutex_lock(&capture_mutex);
    stall_after_first_chunk = false;
    pthread_mutex_unlock(&capture_mutex);
    assert(wait_event(audio_consume_track_advanced, 2500));
    assert(wait_event(audio_consume_track_finished, 2500));
    assert(!audio_is_playing());
    assert(audio_consume_error_ex(NULL) == AUDIO_ERROR_NONE);

    pthread_mutex_lock(&capture_mutex);
    stall_after_first_chunk = true;
    pthread_mutex_unlock(&capture_mutex);
    audio_play_file_at(second_ref, 0, false, 0, false, 0);
    assert(wait_format(second_ref, &info, 5000));
    assert(fabs(audio_get_duration_seconds() - 0.6) < 0.002);
    stop_and_wait();
    puts("CUE audio runtime spec passed");
    return 0;
}
