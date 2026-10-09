#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "metadata.h"
#include <SDL.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool gate_armed;
static bool blocked_entered, blocked_release, blocked_returned;

typedef enum { AUDIO_OPEN_GATE_NONE, AUDIO_OPEN_GATE_BEFORE, AUDIO_OPEN_GATE_AFTER } audio_open_gate_t;
static pthread_mutex_t output_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t output_gate_cond = PTHREAD_COND_INITIALIZER;
static audio_open_gate_t output_open_gate;
static bool output_open_entered, output_open_release;
static bool output_close_armed, output_close_entered, output_close_release;

/* The production definition lives in main.c, which is excluded to provide
 * this spec's entry point. */
void install_thread_crash_altstack(void) {}

extern SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int iscapture,
    const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes);
extern void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID device);

static void wait_gate_locked(bool *release) {
    while (!*release) pthread_cond_wait(&output_gate_cond, &output_gate_mutex);
}

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *device, int iscapture,
    const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) {
    pthread_mutex_lock(&output_gate_mutex);
    bool gate_before = output_open_gate == AUDIO_OPEN_GATE_BEFORE;
    if (gate_before) {
        output_open_entered = true;
        pthread_cond_broadcast(&output_gate_cond);
        wait_gate_locked(&output_open_release);
        output_open_gate = AUDIO_OPEN_GATE_NONE;
    }
    pthread_mutex_unlock(&output_gate_mutex);

    SDL_AudioDeviceID opened = __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained,
                                                          allowed_changes);
    pthread_mutex_lock(&output_gate_mutex);
    if (output_open_gate == AUDIO_OPEN_GATE_AFTER) {
        output_open_entered = true;
        pthread_cond_broadcast(&output_gate_cond);
        wait_gate_locked(&output_open_release);
        output_open_gate = AUDIO_OPEN_GATE_NONE;
    }
    pthread_mutex_unlock(&output_gate_mutex);
    return opened;
}

void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID device) {
    pthread_mutex_lock(&output_gate_mutex);
    if (output_close_armed) {
        output_close_entered = true;
        pthread_cond_broadcast(&output_gate_cond);
        wait_gate_locked(&output_close_release);
        output_close_armed = false;
    }
    pthread_mutex_unlock(&output_gate_mutex);
    __real_SDL_CloseAudioDevice(device);
}

static void arm_output_gate(audio_open_gate_t open_gate) {
    pthread_mutex_lock(&output_gate_mutex);
    output_open_gate = open_gate;
    output_open_entered = false;
    output_open_release = false;
    output_close_armed = true;
    output_close_entered = false;
    output_close_release = false;
    pthread_mutex_unlock(&output_gate_mutex);
}

static void wait_output_gate(bool closing) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&output_gate_mutex);
    bool *entered = closing ? &output_close_entered : &output_open_entered;
    while (!*entered) {
        int rc = pthread_cond_timedwait(&output_gate_cond, &output_gate_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "%s audio output gate timeout\n", closing ? "close" : "open");
            abort();
        }
    }
    pthread_mutex_unlock(&output_gate_mutex);
}

static void release_output_gate(bool closing) {
    pthread_mutex_lock(&output_gate_mutex);
    if (closing) output_close_release = true;
    else output_open_release = true;
    pthread_cond_broadcast(&output_gate_cond);
    pthread_mutex_unlock(&output_gate_mutex);
}

void __wrap_metadata_read_without_artwork(const char *path, track_metadata_t *out) {
    (void) path;
    pthread_mutex_lock(&gate_mutex);
    if (gate_armed && !blocked_entered) {
        blocked_entered = true;
        pthread_cond_broadcast(&gate_cond);
        while (!blocked_release) pthread_cond_wait(&gate_cond, &gate_mutex);
    }
    pthread_mutex_unlock(&gate_mutex);
    pthread_mutex_lock(&gate_mutex);
    blocked_returned = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);

    memset(out, 0, sizeof(*out));
    out->has_replaygain = true;
    out->replaygain_gain_db = -6.0;
    out->has_replaygain_peak = true;
    out->replaygain_peak = 0.5;
    out->has_replaygain_album = true;
    out->replaygain_album_gain_db = -3.0;
    out->has_replaygain_album_peak = true;
    out->replaygain_album_peak = 0.75;
}

static void gate_path(const char *path) {
    (void) path;
    pthread_mutex_lock(&gate_mutex);
    gate_armed = true;
    blocked_entered = false;
    blocked_release = false;
    blocked_returned = false;
    pthread_mutex_unlock(&gate_mutex);
}

static void wait_metadata_gate(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&gate_mutex);
    while (!blocked_entered) {
        int rc = pthread_cond_timedwait(&gate_cond, &gate_mutex, &deadline);
        if (rc == ETIMEDOUT) { fprintf(stderr, "metadata gate timeout\n"); abort(); }
    }
    pthread_mutex_unlock(&gate_mutex);
}

static void release_metadata_gate(void) {
    pthread_mutex_lock(&gate_mutex);
    blocked_release = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
}

static void wait_metadata_return(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&gate_mutex);
    while (!blocked_returned) {
        int rc = pthread_cond_timedwait(&gate_cond, &gate_mutex, &deadline);
        if (rc == ETIMEDOUT) { fprintf(stderr, "metadata return timeout\n"); abort(); }
    }
    pthread_mutex_unlock(&gate_mutex);
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wait_format(const char *path, audio_current_format_info_t *out, int timeout_ms) {
    long long end = monotonic_ms() + timeout_ms;
    do {
        if (audio_get_current_format_info(out) && strcmp(out->path, path) == 0) return true;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    } while (monotonic_ms() < end);
    return false;
}

static void wait_no_format(int timeout_ms) {
    long long end = monotonic_ms() + timeout_ms;
    audio_current_format_info_t info;
    do {
        if (!audio_get_current_format_info(&info) && !audio_is_playing()) return;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    } while (monotonic_ms() < end);
    assert(!audio_get_current_format_info(&info));
    assert(!audio_is_playing());
}

static void wait_idle(int timeout_ms) {
    long long end = monotonic_ms() + timeout_ms;
    do {
        if (audio_is_idle()) return;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    } while (monotonic_ms() < end);
    assert(audio_is_idle());
}

static void stop_and_wait(void) {
    audio_stop();
    wait_no_format(5000);
}

static audio_error_t wait_error(uint64_t *generation, int timeout_ms) {
    long long end = monotonic_ms() + timeout_ms;
    do {
        audio_error_t error = audio_consume_error_ex(generation);
        if (error != AUDIO_ERROR_NONE) return error;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&pause, NULL);
    } while (monotonic_ms() < end);
    return AUDIO_ERROR_NONE;
}

static void make_wav(const char *path) {
    enum { SAMPLE_RATE = 44100, CHANNELS = 2, SECONDS = 60 };
    uint32_t data_size = SAMPLE_RATE * CHANNELS * 2u * SECONDS;
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
    uint32_t sample_rate = SAMPLE_RATE;
    memcpy(header + 24, &sample_rate, 4);
    uint32_t byte_rate = SAMPLE_RATE * align;
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &align, 2);
    memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_size, 4);

    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    unsigned char silence[8192] = {0};
    uint32_t left = data_size;
    while (left) {
        size_t chunk = left < sizeof(silence) ? left : sizeof(silence);
        assert(fwrite(silence, 1, chunk, file) == chunk);
        left -= (uint32_t) chunk;
    }
    assert(fclose(file) == 0);
}

static void test_mode_and_pause_before_output(const char *wav) {
    gate_path(wav);
    long long start = monotonic_ms();
    audio_play_file_at_with_replaygain_mode(wav, 0.0, 1);
    assert(monotonic_ms() - start < 100);
    wait_metadata_gate();
    audio_current_format_info_t info;
    assert(!audio_get_current_format_info(&info));
    assert(audio_is_playing());
    audio_toggle_pause();
    assert(audio_is_paused());
    uint64_t generation = audio_get_playback_generation();
    release_metadata_gate();
    track_metadata_t startup_tags;
    assert(audio_wait_track_metadata(generation, &startup_tags) == AUDIO_TRACK_METADATA_READY);
    assert(startup_tags.has_replaygain && startup_tags.replaygain_gain_db == -6.0);
    assert(startup_tags.has_replaygain_album && startup_tags.replaygain_album_gain_db == -3.0);
    assert(wait_format(wav, &info, 5000));
    assert(audio_is_paused());
    assert(!audio_is_idle());
    assert(audio_get_position_seconds() == 0);
    struct timespec paused_interval = { .tv_sec = 0, .tv_nsec = 150000000 };
    nanosleep(&paused_interval, NULL);
    assert(audio_get_position_seconds() == 0);
    assert(info.replaygain_applied && fabs(info.replaygain_applied_db - (-6.0)) < 0.05);
    audio_toggle_pause();
    assert(!audio_is_paused());
}

static void test_stop_cancels_startup(const char *wav) {
    gate_path(wav);
    audio_play_file_at_with_replaygain_mode(wav, 0.0, 1);
    wait_metadata_gate();
    uint64_t canceled_generation = audio_get_playback_generation();
    assert(!audio_is_idle());
    audio_stop();
    assert(!audio_is_idle());
    for (int i = 0; i < 4; ++i) {
        track_metadata_t canceled_tags = { .has_title = true };
        assert(audio_wait_track_metadata(canceled_generation, &canceled_tags) == AUDIO_TRACK_METADATA_STOPPED);
        assert(!canceled_tags.has_title && !canceled_tags.picture_data && !canceled_tags.lyrics);
    }
    release_metadata_gate();
    wait_metadata_return();
    wait_no_format(5000);
    wait_idle(5000);
    track_metadata_t late_stopped_tags = { .has_title = true };
    assert(audio_wait_track_metadata(canceled_generation, &late_stopped_tags) == AUDIO_TRACK_METADATA_STOPPED);
    assert(!late_stopped_tags.has_title && !late_stopped_tags.picture_data && !late_stopped_tags.lyrics);
}

static void test_new_startup_resume_checkpoint(const char * wav, const char * next_wav) {
    audio_play_file_at(wav, 18.0, false, 0.0, false, 0.0);
    audio_current_format_info_t info;
    assert(wait_format(wav, &info, 5000));
    assert(audio_get_resume_position_seconds() > 17.0);

    gate_path(next_wav);
    audio_play_file_at_with_replaygain_mode(next_wav, 7.25, 1);
    wait_metadata_gate();
    assert(fabs(audio_get_resume_position_seconds() - 7.25) < 0.001);

    uint64_t canceled_generation = audio_get_playback_generation();
    audio_stop();
    assert(fabs(audio_get_resume_position_seconds() - 7.25) < 0.001);
    release_metadata_gate();
    wait_metadata_return();
    wait_idle(5000);
    assert(fabs(audio_get_resume_position_seconds() - 7.25) < 0.001);

    track_metadata_t stopped_tags = { .has_title = true };
    assert(audio_wait_track_metadata(canceled_generation, &stopped_tags) == AUDIO_TRACK_METADATA_STOPPED);
    assert(!stopped_tags.has_title && !stopped_tags.picture_data && !stopped_tags.lyrics);
}

static void test_pause_intent_during_restart(const char *wav, const char *next_wav) {
    audio_play_file_at(wav, 0.0, false, 0.0, false, 0.0);
    audio_current_format_info_t info;
    assert(wait_format(wav, &info, 5000));

    gate_path(next_wav);
    audio_play_file_at_with_replaygain_mode(next_wav, 0.0, 1);
    wait_metadata_gate();
    audio_toggle_pause();
    assert(audio_is_paused());
    release_metadata_gate();
    assert(wait_format(next_wav, &info, 5000));
    assert(audio_is_paused());
    assert(info.replaygain_applied && fabs(info.replaygain_applied_db - (-6.0)) < 0.05);
    audio_toggle_pause();
    assert(!audio_is_paused());
    stop_and_wait();
}

static void test_stop_old_playback_with_queued_startup(const char *wav, const char *next_wav) {
    audio_play_file_at(wav, 0.0, false, 0.0, false, 0.0);
    audio_current_format_info_t info;
    assert(wait_format(wav, &info, 5000));
    uint64_t replaced_generation = audio_get_playback_generation();

    gate_path(next_wav);
    audio_play_file_at_with_replaygain_mode(next_wav, 0.0, 1);
    wait_metadata_gate();
    uint64_t before_stop = audio_get_playback_generation();
    assert(!audio_is_idle());
    audio_stop();
    assert(audio_get_playback_generation() != before_stop);
    track_metadata_t replaced_tags = { .has_title = true };
    assert(audio_wait_track_metadata(replaced_generation, &replaced_tags) == AUDIO_TRACK_METADATA_REPLACED);
    assert(!replaced_tags.has_title);
    assert(audio_wait_track_metadata(before_stop, &replaced_tags) == AUDIO_TRACK_METADATA_STOPPED);
    assert(!audio_is_playing());
    assert(!audio_is_idle());
    release_metadata_gate();
    wait_metadata_return();
    wait_no_format(5000);
    wait_idle(5000);
    assert(audio_consume_error_ex(NULL) == AUDIO_ERROR_NONE);
}

static void test_latest_request_replaygain_and_next_guard(const char *wav, const char *latest_wav) {
    gate_path(wav);
    audio_play_file_at_with_replaygain_mode(wav, 0.0, 1);
    wait_metadata_gate();
    uint64_t stale_generation = audio_get_playback_generation();
    audio_play_file_at_with_replaygain_mode(latest_wav, 0.0, 2);
    uint64_t current_generation = audio_get_playback_generation();
    assert(current_generation != stale_generation);
    release_metadata_gate();

    track_metadata_t latest_tags;
    assert(audio_wait_track_metadata(current_generation, &latest_tags) == AUDIO_TRACK_METADATA_READY);
    assert(latest_tags.has_replaygain_album && latest_tags.replaygain_album_gain_db == -3.0);
    track_metadata_t stale_tags = { .has_title = true };
    assert(audio_wait_track_metadata(stale_generation, &stale_tags) == AUDIO_TRACK_METADATA_REPLACED);
    assert(!stale_tags.has_title && !stale_tags.picture_data && !stale_tags.lyrics);
    audio_current_format_info_t info;
    assert(wait_format(latest_wav, &info, 5000));
    assert(info.replaygain_applied && fabs(info.replaygain_applied_db - (-3.0)) < 0.05);
    assert(audio_set_next_track_for_generation(wav, false, 0.0, false, 0.0, current_generation));
    assert(!audio_set_next_track_for_generation(wav, false, 0.0, false, 0.0, stale_generation));
    audio_stop();
    assert(!audio_is_idle());
    assert(!audio_set_next_track_for_generation(wav, false, 0.0, false, 0.0, current_generation));
    wait_idle(5000);
}

static void test_failed_open_clears_startup(void) {
    uint64_t requested_generation = audio_get_playback_generation() + 1;
    audio_play_file_at_with_replaygain_mode("/no/such/track-metadata-worker.wav", 0.0, 1);
    uint64_t error_generation = 0;
    audio_error_t error = wait_error(&error_generation, 5000);
    assert(error != AUDIO_ERROR_NONE);
    assert(error_generation == requested_generation);
    assert(!audio_is_playing() && !audio_is_paused());
    audio_current_format_info_t info;
    assert(!audio_get_current_format_info(&info));
    assert(audio_consume_error_ex(NULL) == AUDIO_ERROR_NONE);
    wait_idle(5000);
}

static void test_stop_after_decoder_open_or_during_output_open(const char * wav,
                                                                audio_open_gate_t gate) {
    arm_output_gate(gate);
    audio_play_file_at(wav, 0.0, false, 0.0, false, 0.0);

    /* SDL output open is reached only after the WAV decoder has opened. The
     * BEFORE gate stops in that exact gap; AFTER stops with the SDL device
     * already created but before audio.c can publish the startup format. */
    wait_output_gate(false);
    audio_current_format_info_t info;
    assert(!audio_get_current_format_info(&info));
    assert(!audio_is_idle());
    audio_stop();
    assert(!audio_is_idle());

    release_output_gate(false);
    wait_output_gate(true);
    assert(!audio_get_current_format_info(&info));
    assert(!audio_is_idle()); /* output close has not returned yet */
    release_output_gate(true);

    wait_no_format(5000);
    wait_idle(5000);
    assert(audio_consume_error_ex(NULL) == AUDIO_ERROR_NONE);
}

static void test_established_stop_waits_for_output_release(const char *wav) {
    audio_play_file_at(wav, 11.0, false, 0.0, false, 0.0);
    audio_current_format_info_t info;
    assert(wait_format(wav, &info, 5000));
    double stopped_position = audio_get_resume_position_seconds();
    assert(stopped_position > 10.0);
    assert(!audio_is_idle());
    audio_stop();
    assert(!audio_is_idle());
    wait_idle(5000);
    assert(fabs(audio_get_resume_position_seconds() - stopped_position) < 0.1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *wav = argv[1];
    char latest_wav[PATH_MAX];
    snprintf(latest_wav, sizeof(latest_wav), "%s.latest.wav", wav);
    make_wav(wav);
    make_wav(latest_wav);
    audio_init();
    test_mode_and_pause_before_output(wav);
    stop_and_wait();
    test_stop_cancels_startup(wav);
    test_new_startup_resume_checkpoint(wav, latest_wav);
    test_pause_intent_during_restart(wav, latest_wav);
    test_stop_old_playback_with_queued_startup(wav, latest_wav);
    test_latest_request_replaygain_and_next_guard(wav, latest_wav);
    test_failed_open_clears_startup();
    test_stop_after_decoder_open_or_during_output_open(wav, AUDIO_OPEN_GATE_BEFORE);
    test_stop_after_decoder_open_or_during_output_open(wav, AUDIO_OPEN_GATE_AFTER);
    test_established_stop_waits_for_output_release(wav);
    puts("audio startup runtime spec passed");
    return 0;
}
