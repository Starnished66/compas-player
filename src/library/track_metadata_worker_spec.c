#define _POSIX_C_SOURCE 200809L
#include "track_metadata_worker.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENT_CAPACITY 128

static pthread_mutex_t spec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t spec_cond = PTHREAD_COND_INITIALIZER;
static bool slow_old_entered, release_slow_old;
static bool barrier_entered, release_barrier;
static bool audio_cancel_entered, release_audio_cancel;
static bool audio_replace_entered, release_audio_replace;
static size_t audio_wait_calls;
static uint64_t audio_generations[EVENT_CAPACITY];
static size_t fairness_audio_calls, fairness_release_calls;
static bool fairness_mode;
static size_t fairness_order, fairness_audio_order[16], first_fair_count_order;
static char read_events[EVENT_CAPACITY][PATH_MAX];
static char favorite_events[EVENT_CAPACITY][PATH_MAX];
static size_t read_event_count;
static char count_events[EVENT_CAPACITY][PATH_MAX];
static size_t count_event_count;
static size_t favorite_calls, favorite_event_count, probe_calls, wake_calls;
static pthread_t caller_thread;
static bool metadata_on_worker, favorite_on_worker, probe_on_worker, wake_on_worker;

void metadata_read_without_artwork(const char *path, track_metadata_t *out) {
    pthread_mutex_lock(&spec_mutex);
    assert(read_event_count < EVENT_CAPACITY);
    snprintf(read_events[read_event_count++], PATH_MAX, "%s", path);
    if (strcmp(path, "slow-old") == 0) {
        slow_old_entered = true;
        pthread_cond_broadcast(&spec_cond);
        while (!release_slow_old) pthread_cond_wait(&spec_cond, &spec_mutex);
    } else if (strcmp(path, "barrier") == 0) {
        barrier_entered = true;
        pthread_cond_broadcast(&spec_cond);
        while (!release_barrier) pthread_cond_wait(&spec_cond, &spec_mutex);
    }
    metadata_on_worker = !pthread_equal(pthread_self(), caller_thread);
    pthread_mutex_unlock(&spec_mutex);

    snprintf(out->title, sizeof(out->title), "read:%s", path);
    out->has_title = true;
    out->picture_data = malloc(4);
    out->picture_size = 4;
    out->lyrics = malloc(8);
    assert(out->picture_data && out->lyrics);
    memcpy(out->picture_data, "art", 4);
    memcpy(out->lyrics, "lyrics", 7);
}

audio_track_metadata_wait_result_t audio_wait_track_metadata(uint64_t generation, track_metadata_t *out) {
    pthread_mutex_lock(&spec_mutex);
    size_t call = ++audio_wait_calls;
    assert(call <= EVENT_CAPACITY);
    audio_generations[call - 1] = generation;
    if (generation == 9001) {
        audio_cancel_entered = true;
        pthread_cond_broadcast(&spec_cond);
        while (!release_audio_cancel) pthread_cond_wait(&spec_cond, &spec_mutex);
        memset(out, 0, sizeof(*out));
        pthread_mutex_unlock(&spec_mutex);
        return AUDIO_TRACK_METADATA_STOPPED;
    }
    if (generation == 9003) {
        audio_replace_entered = true;
        pthread_cond_broadcast(&spec_cond);
        while (!release_audio_replace) pthread_cond_wait(&spec_cond, &spec_mutex);
        memset(out, 0, sizeof(*out));
        pthread_mutex_unlock(&spec_mutex);
        return AUDIO_TRACK_METADATA_REPLACED;
    }
    if (fairness_mode) {
        assert(fairness_audio_calls < 16);
        size_t index = fairness_audio_calls++;
        fairness_audio_order[index] = ++fairness_order;
        pthread_cond_broadcast(&spec_cond);
        while (fairness_release_calls < fairness_audio_calls)
            pthread_cond_wait(&spec_cond, &spec_mutex);
    }
    pthread_mutex_unlock(&spec_mutex);

    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "audio-generation-%llu",
             (unsigned long long) generation);
    out->has_title = true;
    out->has_replaygain = true;
    out->replaygain_gain_db = -4.5;
    return AUDIO_TRACK_METADATA_READY;
}

bool favorite_writer_is_set(const char *path) {
    pthread_mutex_lock(&spec_mutex);
    favorite_calls++;
    assert(favorite_event_count < EVENT_CAPACITY);
    snprintf(favorite_events[favorite_event_count++], PATH_MAX, "%s", path);
    favorite_on_worker = !pthread_equal(pthread_self(), caller_thread);
    pthread_mutex_unlock(&spec_mutex);
    return true;
}

bool audio_probe_file_format(const char *path, audio_current_format_info_t *out) {
    (void) path;
    pthread_mutex_lock(&spec_mutex);
    probe_calls++;
    probe_on_worker = !pthread_equal(pthread_self(), caller_thread);
    pthread_mutex_unlock(&spec_mutex);
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->source_sample_rate = 48000;
    return true;
}

void metadata_db_song_play_count_increment(const char *path) {
    pthread_mutex_lock(&spec_mutex);
    assert(count_event_count < EVENT_CAPACITY);
    snprintf(count_events[count_event_count++], PATH_MAX, "%s", path);
    if (fairness_mode && strncmp(path, "fair-play-", 10) == 0 && !first_fair_count_order)
        first_fair_count_order = ++fairness_order;
    pthread_cond_broadcast(&spec_cond);
    pthread_mutex_unlock(&spec_mutex);
}

void ui_wake_notify(void) {
    pthread_mutex_lock(&spec_mutex);
    wake_calls++;
    wake_on_worker = !pthread_equal(pthread_self(), caller_thread);
    pthread_cond_broadcast(&spec_cond);
    pthread_mutex_unlock(&spec_mutex);
}

static void deadline_after(struct timespec *deadline, int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void wait_flag(bool *flag, const char *name) {
    struct timespec deadline;
    deadline_after(&deadline, 5000);
    pthread_mutex_lock(&spec_mutex);
    while (!*flag) {
        int rc = pthread_cond_timedwait(&spec_cond, &spec_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "timeout waiting for %s\n", name);
            abort();
        }
    }
    pthread_mutex_unlock(&spec_mutex);
}

static void wait_count(size_t *value, size_t target, const char *name) {
    struct timespec deadline;
    deadline_after(&deadline, 5000);
    pthread_mutex_lock(&spec_mutex);
    while (*value < target) {
        int rc = pthread_cond_timedwait(&spec_cond, &spec_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "timeout waiting for %s (%zu/%zu)\n", name, *value, target);
            abort();
        }
    }
    pthread_mutex_unlock(&spec_mutex);
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static track_metadata_request_t request(uint64_t token, const char *path, bool read_file) {
    track_metadata_request_t req = { .token = token, .read_file = read_file };
    snprintf(req.path, sizeof(req.path), "%s", path);
    snprintf(req.metadata.title, sizeof(req.metadata.title), "seed:%s", path);
    req.metadata.has_title = true;
    return req;
}

static void release_flag(bool *flag) {
    pthread_mutex_lock(&spec_mutex);
    *flag = true;
    pthread_cond_broadcast(&spec_cond);
    pthread_mutex_unlock(&spec_mutex);
}

static void test_stale_result_and_lane_priority(void) {
    track_metadata_request_t old = request(1, "slow-old", true);
    assert(track_metadata_submit(TRACK_METADATA_CURRENT, &old));
    wait_flag(&slow_old_entered, "slow CURRENT reader");

    track_metadata_request_t current = request(2, "current-latest", true);
    current.probe = true;
    track_metadata_request_t next = request(3, "next-lane", true);
    next.metadata.track_number = 7;
    next.metadata.has_track_number = true;
    assert(track_metadata_submit(TRACK_METADATA_CURRENT, &current));
    assert(track_metadata_submit(TRACK_METADATA_NEXT, &next));
    release_flag(&release_slow_old);
    wait_count(&read_event_count, 3, "CURRENT/NEXT reads");

    assert(strcmp(read_events[0], "slow-old") == 0);
    assert(strcmp(read_events[1], "next-lane") == 0);
    assert(strcmp(read_events[2], "current-latest") == 0);
    wait_count(&wake_calls, 3, "all metadata completions");

    track_metadata_result_t result;
    assert(track_metadata_take(TRACK_METADATA_NEXT, &result));
    assert(result.request.token == 3);
    assert(result.request.metadata.track_number == 7);
    assert(!result.favorite && !result.has_probe);
    assert(result.request.metadata.picture_data == NULL && result.request.metadata.lyrics == NULL);
    assert(track_metadata_take(TRACK_METADATA_CURRENT, &result));
    assert(result.request.token == 2);
    assert(strcmp(result.request.metadata.title, "read:current-latest") == 0);
    assert(result.request.metadata.picture_data == NULL && result.request.metadata.picture_size == 0);
    assert(result.request.metadata.lyrics == NULL);
    assert(result.favorite && result.has_probe && result.probe.valid && result.probe.source_sample_rate == 48000);
    assert(!track_metadata_take(TRACK_METADATA_CURRENT, &result));

    pthread_mutex_lock(&spec_mutex);
    assert(metadata_on_worker && favorite_on_worker && probe_on_worker && wake_on_worker);
    assert(favorite_calls == 2 && probe_calls == 1 && wake_calls == 3);
    pthread_mutex_unlock(&spec_mutex);
}

static void test_favorite_lane_audio_metadata_cancel(void) {
    pthread_mutex_lock(&spec_mutex);
    size_t initial_audio_calls = audio_wait_calls;
    size_t initial_wakes = wake_calls;
    size_t initial_reads = read_event_count;
    size_t initial_favorites = favorite_event_count;
    pthread_mutex_unlock(&spec_mutex);

    track_metadata_request_t canceled = request(30, "favorite-audio-cancel", false);
    canceled.read_audio = true;
    canceled.audio_generation = 9001;
    assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &canceled));
    wait_flag(&audio_cancel_entered, "canceled audio metadata wait");

    track_metadata_request_t latest = request(31, "favorite-audio-latest", false);
    latest.read_audio = true;
    latest.audio_generation = 9002;
    assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &latest));
    release_flag(&release_audio_cancel);
    wait_count(&audio_wait_calls, initial_audio_calls + 2, "audio metadata generations");
    wait_count(&read_event_count, initial_reads + 1, "canceled startup file fallback");
    wait_count(&favorite_event_count, initial_favorites + 1, "canceled startup favorite refresh");
    wait_count(&wake_calls, initial_wakes + 2, "audio metadata wake notifications");
    pthread_mutex_lock(&spec_mutex);
    assert(strcmp(read_events[initial_reads], "favorite-audio-cancel") == 0);
    assert(strcmp(favorite_events[initial_favorites], "favorite-audio-cancel") == 0);
    pthread_mutex_unlock(&spec_mutex);

    track_metadata_result_t result;
    assert(track_metadata_take(TRACK_METADATA_FAVORITE, &result));
    assert(result.request.token == 31);
    assert(result.request.audio_generation == 9002);
    assert(strcmp(result.request.metadata.title, "audio-generation-9002") == 0);
    assert(result.request.metadata.has_replaygain);
    assert(fabs(result.request.metadata.replaygain_gain_db - (-4.5)) < 0.001);
    assert(result.favorite && !result.has_probe);
    assert(!track_metadata_take(TRACK_METADATA_FAVORITE, &result));
    pthread_mutex_lock(&spec_mutex);
    assert(audio_generations[initial_audio_calls] == 9001);
    assert(audio_generations[initial_audio_calls + 1] == 9002);
    pthread_mutex_unlock(&spec_mutex);

    pthread_mutex_lock(&spec_mutex);
    size_t replace_initial_reads = read_event_count;
    size_t replace_initial_favorites = favorite_event_count;
    size_t replace_initial_audio_calls = audio_wait_calls;
    size_t replace_initial_wakes = wake_calls;
    pthread_mutex_unlock(&spec_mutex);
    track_metadata_request_t replaced = request(40, "favorite-audio-replaced", false);
    replaced.read_audio = true;
    replaced.audio_generation = 9003;
    assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &replaced));
    wait_flag(&audio_replace_entered, "replaced audio metadata wait");
    track_metadata_request_t replacement = request(41, "favorite-audio-replacement", false);
    replacement.read_audio = true;
    replacement.audio_generation = 9004;
    assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &replacement));
    release_flag(&release_audio_replace);
    wait_count(&audio_wait_calls, replace_initial_audio_calls + 2, "replacement audio metadata generations");
    wait_count(&favorite_event_count, replace_initial_favorites + 1, "replacement favorite refresh");
    wait_count(&wake_calls, replace_initial_wakes + 1, "replacement metadata wake notifications");
    pthread_mutex_lock(&spec_mutex);
    assert(read_event_count == replace_initial_reads);
    assert(strcmp(favorite_events[replace_initial_favorites], "favorite-audio-replacement") == 0);
    pthread_mutex_unlock(&spec_mutex);
    assert(track_metadata_take(TRACK_METADATA_FAVORITE, &result));
    assert(result.request.token == 41);
    assert(strcmp(result.request.metadata.title, "audio-generation-9004") == 0);
    assert(!track_metadata_take(TRACK_METADATA_FAVORITE, &result));
}

static void test_scalar_admission_async_and_count_capacity(void) {
    track_metadata_request_t barrier = request(10, "barrier", true);
    assert(track_metadata_submit(TRACK_METADATA_CURRENT, &barrier));
    wait_flag(&barrier_entered, "barrier reader");

    /* Request seeds are scalar-only; rejected blob pointers remain owned by
     * the caller and can be freed there. */
    track_metadata_request_t invalid = request(11, "blob-input", false);
    invalid.metadata.picture_data = malloc(1);
    invalid.metadata.lyrics = malloc(1);
    assert(invalid.metadata.picture_data && invalid.metadata.lyrics);
    assert(!track_metadata_submit(TRACK_METADATA_NEXT, &invalid));
    free(invalid.metadata.picture_data);
    free(invalid.metadata.lyrics);

    track_metadata_request_t scalar = request(12, "scalar-seed", false);
    scalar.metadata.release_year = 1999;
    scalar.metadata.has_release_year = true;
    long long start = monotonic_ms();
    assert(track_metadata_submit(TRACK_METADATA_NEXT, &scalar));
    for (int i = 0; i < 64; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "play-%02d", i);
        assert(track_metadata_count_play(path));
    }
    assert(!track_metadata_count_play("overflow"));
    assert(monotonic_ms() - start < 100);

    release_flag(&release_barrier);
    wait_count(&read_event_count, 5, "barrier and canceled-startup file reads");
    wait_count(&count_event_count, 64, "all admitted play-count events");

    track_metadata_result_t result;
    assert(track_metadata_take(TRACK_METADATA_NEXT, &result));
    assert(result.request.token == 12);
    assert(result.request.metadata.release_year == 1999 && result.request.metadata.has_release_year);
    assert(strcmp(result.request.metadata.title, "seed:scalar-seed") == 0);
    assert(result.request.metadata.picture_data == NULL && result.request.metadata.lyrics == NULL);
    for (int i = 0; i < 64; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "play-%02d", i);
        assert(strcmp(count_events[i], expected) == 0);
    }
    bool barrier_read = false;
    pthread_mutex_lock(&spec_mutex);
    for (size_t i = 0; i < read_event_count; ++i)
        if (strcmp(read_events[i], "barrier") == 0) barrier_read = true;
    pthread_mutex_unlock(&spec_mutex);
    assert(barrier_read);
}

typedef struct { size_t jobs; } fairness_producer_args_t;

static void *fairness_producer(void *data) {
    fairness_producer_args_t *args = data;
    for (size_t call = 1; call <= args->jobs; ++call) {
        struct timespec deadline;
        deadline_after(&deadline, 5000);
        pthread_mutex_lock(&spec_mutex);
        while (fairness_audio_calls < call) {
            int rc = pthread_cond_timedwait(&spec_cond, &spec_mutex, &deadline);
            if (rc == ETIMEDOUT) abort();
        }
        pthread_mutex_unlock(&spec_mutex);

        if (call < args->jobs) {
            char path[64];
            snprintf(path, sizeof(path), "fair-metadata-%02zu", call);
            track_metadata_request_t req = request(2000 + call, path, false);
            req.read_audio = true;
            req.audio_generation = 10000 + call;
            assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &req));
        }
        pthread_mutex_lock(&spec_mutex);
        fairness_release_calls = call;
        pthread_cond_broadcast(&spec_cond);
        pthread_mutex_unlock(&spec_mutex);
    }
    return NULL;
}

static void test_count_fairness_while_metadata_continues(void) {
    enum { FAIR_JOBS = 8, FAIR_COUNTS = 3 };
    pthread_mutex_lock(&spec_mutex);
    size_t initial_wakes = wake_calls;
    size_t initial_counts = count_event_count;
    fairness_mode = true;
    fairness_audio_calls = 0;
    fairness_release_calls = 0;
    fairness_order = 0;
    first_fair_count_order = 0;
    memset(fairness_audio_order, 0, sizeof(fairness_audio_order));
    pthread_mutex_unlock(&spec_mutex);

    track_metadata_request_t first = request(2000, "fair-metadata-initial", false);
    first.read_audio = true;
    first.audio_generation = 10000;
    assert(track_metadata_submit(TRACK_METADATA_FAVORITE, &first));
    wait_count(&fairness_audio_calls, 1, "first gated metadata job");

    for (int i = 0; i < FAIR_COUNTS; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "fair-play-%02d", i);
        assert(track_metadata_count_play(path));
    }
    fairness_producer_args_t args = { .jobs = FAIR_JOBS };
    pthread_t producer;
    assert(pthread_create(&producer, NULL, fairness_producer, &args) == 0);
    assert(pthread_join(producer, NULL) == 0);

    wait_count(&fairness_audio_calls, FAIR_JOBS, "continuous metadata jobs");
    wait_count(&wake_calls, initial_wakes + FAIR_JOBS, "metadata job completions");
    wait_count(&count_event_count, initial_counts + FAIR_COUNTS, "fair play-count service");
    pthread_mutex_lock(&spec_mutex);
    assert(first_fair_count_order > fairness_audio_order[0]);
    assert(first_fair_count_order < fairness_audio_order[1]);
    assert(fairness_mode);
    fairness_mode = false;
    pthread_mutex_unlock(&spec_mutex);

    for (int i = 0; i < FAIR_COUNTS; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "fair-play-%02d", i);
        assert(strcmp(count_events[initial_counts + (size_t) i], expected) == 0);
    }
    track_metadata_result_t result;
    assert(track_metadata_take(TRACK_METADATA_FAVORITE, &result));
    assert(result.request.token == 2007);
    assert(strcmp(result.request.metadata.title, "audio-generation-10007") == 0);
}

int main(void) {
    caller_thread = pthread_self();
    test_stale_result_and_lane_priority();
    test_favorite_lane_audio_metadata_cancel();
    test_scalar_admission_async_and_count_capacity();
    test_count_fairness_while_metadata_continues();
    puts("track_metadata_worker async spec passed");
    return 0;
}
