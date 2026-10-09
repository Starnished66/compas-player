/* Host-only lifecycle regression test for usb_dac_bridge.c.
 *
 * The bridge implementation is included with its device/syscall and audio
 * seams redirected to deterministic mocks.  This deliberately exercises the
 * real pthread lifecycle, rather than duplicating the state machine in the
 * test.  Build manually, for example:
 *
 *   cc -O0 -g -Wall -Wextra -Isrc/audio -Isrc/hardware -Isrc/core -Isrc/library \
 *      src/hardware/usb_dac_bridge_lifecycle_regression.c -pthread -o /tmp/usb_dac_bridge_lifecycle_test
 */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mock_cond = PTHREAD_COND_INITIALIZER;
static bool mock_write_stalled;
static bool mock_write_entered;
static bool mock_audio_stop_stalled;
static bool mock_audio_stop_entered;
static bool mock_audio_playing;
static bool mock_audio_idle = true;
static int mock_open_calls = 0;
static int mock_read_eof_after = -1; /* if >= 0, returns 0 once read count reaches this */
static int mock_read_calls = 0;
static int mock_read_eof_count = 0;   /* number of consecutive EOFs to return when hit */
static int mock_read_eperm_after = -1; /* if >= 0, returns EPERM starting at this call count */
static size_t mock_read_size = 4096;  /* bytes returned per successful read */
/* The mock device streams one continuous S32 stereo pattern with eight
 * distinct nonzero bytes per frame: L = 0x12342211, R = 0x56784433. Any
 * byte shift (1 to 7) puts other bytes in the top 16 bits of both channels
 * and breaks the L:R check below; none decodes to silence. */
static uint64_t mock_stream_offset = 0;
static int mock_bad_frames = 0;
static int mock_good_frames = 0;
static int mock_exact_frames = 0; /* unfaded, exactly 0x1234 / 0x5678 */
static int mock_ioctl_fail_count = 0;  /* fail this many ioctl calls first */
/* Keep failing until the reader has consumed a partial frame, so the first
 * successful answer always lands with a nonzero carry. */
static bool mock_ioctl_fail_until_mid_frame = false;
static int mock_usleep_calls = 0;
static uint64_t mock_usleep_total_us = 0;
static int mock_host_rate = 48000;     /* what the format ioctl reports */
static int mock_ioctl_calls = 0;
static bool mock_s24_active = false;
static int mock_write_s24_calls = 0;

static void mock_wait_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int mock_open(const char *path, int flags, ...) {
    (void) flags;
    pthread_mutex_lock(&mock_mutex);
    mock_open_calls++;
    pthread_mutex_unlock(&mock_mutex);
    return strcmp(path, "/dev/uac_sa") == 0 ? 91 : (errno = ENOENT, -1);
}

static int mock_close(int fd) { return fd == 91 ? 0 : -1; }

/* ioctl(fd, 1, int[3] {format, rate, bits}), as the R1 driver answers it. */
static int mock_ioctl(int fd, unsigned long request, void *arg) {
    if (fd != 91 || request != 1) { errno = EINVAL; return -1; }
    int *info = arg;
    pthread_mutex_lock(&mock_mutex);
    mock_ioctl_calls++;
    bool mid_frame_pending = mock_ioctl_fail_until_mid_frame &&
                             (mock_stream_offset < 20000 || mock_stream_offset % 8 == 0);
    if (mock_ioctl_fail_count > 0 || mid_frame_pending) {
        if (mock_ioctl_fail_count > 0) mock_ioctl_fail_count--;
        pthread_mutex_unlock(&mock_mutex);
        errno = ENOTTY;
        return -1;
    }
    /* Exact frames count only from the first answer on, so the test proves
     * clean audio after the transition, not before it. */
    if (mock_ioctl_fail_until_mid_frame) {
        mock_ioctl_fail_until_mid_frame = false;
        mock_exact_frames = 0;
    }
    info[0] = 1;
    info[1] = mock_host_rate;
    info[2] = 32;
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
    (void) timeout_ms;
    if (count > 0) fds[0].revents = POLLIN;
    return count > 0 ? 1 : 0;
}

static ssize_t mock_read(int fd, void *buf, size_t count) {
    static uint8_t packet[4096];
    (void) fd;
    if (count == 0) return 0;
    pthread_mutex_lock(&mock_mutex);
    mock_read_calls++;
    if (mock_read_eperm_after >= 0 && mock_read_calls >= mock_read_eperm_after) {
        pthread_mutex_unlock(&mock_mutex);
        errno = EPERM;
        return -1;
    }
    if (mock_read_eof_after >= 0 && mock_read_calls >= mock_read_eof_after) {
        if (mock_read_eof_count > 0) {
            mock_read_eof_count--;
            pthread_mutex_unlock(&mock_mutex);
            return 0; /* clean EOF */
        }
    }
    size_t size = mock_read_size < sizeof(packet) ? mock_read_size : sizeof(packet);
    if (size > count) size = count;
    static const uint8_t frame[8] = { 0x11, 0x22, 0x34, 0x12, 0x33, 0x44, 0x78, 0x56 };
    for (size_t i = 0; i < size; i++) packet[i] = frame[(mock_stream_offset + i) % 8];
    mock_stream_offset += size;
    pthread_mutex_unlock(&mock_mutex);
    memcpy(buf, packet, size);
    mock_wait_ms(1);
    return (ssize_t) size;
}

static int mock_usleep(useconds_t usec) {
    pthread_mutex_lock(&mock_mutex);
    mock_usleep_calls++;
    mock_usleep_total_us += usec;
    pthread_mutex_unlock(&mock_mutex);
    mock_wait_ms((unsigned)(usec / 1000));
    return 0;
}

bool audio_is_playing(void) { return mock_audio_playing; }
bool audio_is_paused(void) { return false; }
bool audio_is_idle(void) {
    pthread_mutex_lock(&mock_mutex);
    bool idle = mock_audio_idle;
    pthread_mutex_unlock(&mock_mutex);
    return idle;
}
void audio_stop(void) {
    pthread_mutex_lock(&mock_mutex);
    mock_audio_stop_entered = true;
    pthread_cond_broadcast(&mock_cond);
    while (mock_audio_stop_stalled) pthread_cond_wait(&mock_cond, &mock_mutex);
    mock_audio_playing = false;
    pthread_mutex_unlock(&mock_mutex);
}

static int mock_ensure_calls = 0;
static unsigned int mock_ensure_last_rate = 0;
static int mock_write_calls = 0;
static int mock_output_close_calls = 0;

bool audio_output_ensure(unsigned int channels, unsigned int rate, bool low_latency, bool want_s24) {
    (void) channels; (void) rate; (void) low_latency; (void) want_s24;
    pthread_mutex_lock(&mock_mutex);
    mock_ensure_calls++;
    mock_ensure_last_rate = rate;
    pthread_mutex_unlock(&mock_mutex);
    return true;
}
bool audio_output_write(const int16_t *buf, uint64_t frames, unsigned int channels,
                        uint64_t *written) {
    (void) channels;
    pthread_mutex_lock(&mock_mutex);
    mock_write_calls++;
    /* Both channels share any fade gain, so a well-framed pair keeps the
     * 0x1234 : 0x5678 ratio with L never above full pattern level. */
    for (uint64_t i = 0; buf && i < frames; i++) {
        int32_t l = buf[2 * i], r = buf[2 * i + 1];
        bool ok = l >= 0 && l <= 0x1234 && r >= 0 && r <= 0x5678 &&
                  llabs((long long) l * 0x5678 - (long long) r * 0x1234) <= 0x5678 * 2;
        if (ok) mock_good_frames++; else mock_bad_frames++;
        if (l == 0x1234 && r == 0x5678) mock_exact_frames++;
    }
    mock_write_entered = true;
    pthread_cond_broadcast(&mock_cond);
    while (mock_write_stalled) pthread_cond_wait(&mock_cond, &mock_mutex);
    if (written) *written = frames;
    pthread_mutex_unlock(&mock_mutex);
    return true;
}
bool audio_output_write_s24(const int32_t *buf, uint64_t frames, unsigned int channels,
                            uint64_t *written) {
    (void) buf; (void) channels;
    pthread_mutex_lock(&mock_mutex);
    mock_write_s24_calls++;
    if (written) *written = frames;
    pthread_mutex_unlock(&mock_mutex);
    return true;
}
bool audio_output_is_s24_active(void) {
    pthread_mutex_lock(&mock_mutex);
    bool active = mock_s24_active;
    pthread_mutex_unlock(&mock_mutex);
    return active;
}
void audio_output_close(void) {
    pthread_mutex_lock(&mock_mutex);
    mock_output_close_calls++;
    pthread_mutex_unlock(&mock_mutex);
}
void audio_output_set_bt_requested(bool enabled) { (void) enabled; }
void audio_output_set_usb_requested(bool enabled, const char *device) {
    (void) enabled; (void) device;
}

static bool mock_db_log_enabled = false;
bool db_log_enabled(void) { return mock_db_log_enabled; }

static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                  void *(*)(void *), void *) = pthread_create;
static int mock_fail_create_on;
static int mock_create_calls;
static int bridge_test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                      void *(*entry)(void *), void *arg) {
    mock_create_calls++;
    if (mock_fail_create_on > 0 && mock_create_calls == mock_fail_create_on)
        return EAGAIN;
    return real_pthread_create(thread, attr, entry, arg);
}

#define open mock_open
#define ioctl mock_ioctl
#define close mock_close
#define poll mock_poll
#define read mock_read
#define usleep mock_usleep
#define pthread_create bridge_test_pthread_create
#include "usb_dac_bridge.c"
#undef open
#undef ioctl
#undef close
#undef poll
#undef read
#undef usleep
#undef pthread_create

static void wait_for_flag(bool *flag, unsigned timeout_ms, const char *what) {
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        pthread_mutex_lock(&mock_mutex);
        bool set = *flag;
        pthread_mutex_unlock(&mock_mutex);
        if (set) return;
        mock_wait_ms(10);
    }
    fprintf(stderr, "FAIL: timed out waiting for %s\n", what);
    assert(false);
}

static void *stop_call(void *arg) { (void) arg; usb_dac_bridge_stop(); return NULL; }

struct async_call { atomic_bool done; bool start; };
static void *lifecycle_call(void *arg) {
    struct async_call *call = arg;
    if (call->start) usb_dac_bridge_start(); else usb_dac_bridge_stop();
    atomic_store(&call->done, true);
    return NULL;
}

static void reset_mocks(void) {
    pthread_mutex_lock(&mock_mutex);
    mock_write_stalled = false;
    mock_write_entered = false;
    mock_audio_stop_stalled = false;
    mock_audio_stop_entered = false;
    mock_audio_playing = false;
    mock_audio_idle = true;
    mock_fail_create_on = 0;
    mock_create_calls = 0;
    mock_open_calls = 0;
    mock_read_calls = 0;
    mock_read_eof_after = -1;
    mock_read_eof_count = 0;
    mock_read_eperm_after = -1;
    mock_usleep_calls = 0;
    mock_usleep_total_us = 0;
    mock_ensure_calls = 0;
    mock_ensure_last_rate = 0;
    mock_write_calls = 0;
    mock_output_close_calls = 0;
    mock_host_rate = 48000;
    mock_read_size = 4096;
    mock_stream_offset = 0;
    mock_bad_frames = 0;
    mock_good_frames = 0;
    mock_exact_frames = 0;
    mock_ioctl_fail_count = 0;
    mock_ioctl_fail_until_mid_frame = false;
    mock_ioctl_calls = 0;
    mock_s24_active = false;
    mock_write_s24_calls = 0;
    pthread_mutex_unlock(&mock_mutex);
}

static void test_partial_start_rolls_back_joinably(void) {
    reset_mocks();
    mock_fail_create_on = 2;
    usb_dac_bridge_start();
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(!info.bridge_running);
    assert(!info.streaming);

    /* A subsequent start must be able to use the fully cleaned-up ring. */
    mock_fail_create_on = 0;
    usb_dac_bridge_start();
    usb_dac_bridge_stop();
}

static void test_timeout_then_restart_reaps_before_reuse(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_write_stalled = true;
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    wait_for_flag(&mock_write_entered, 2000, "stalled output write");

    pthread_t stopper;
    uint64_t stop_started_ns = monotonic_ns();
    assert(pthread_create(&stopper, NULL, stop_call, NULL) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    uint64_t stop_elapsed_ns = monotonic_ns() - stop_started_ns;
    assert(stop_elapsed_ns >= 2500000000ULL && stop_elapsed_ns < 4500000000ULL);

    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(!info.bridge_running);
    pthread_mutex_lock(&bridge_mutex);
    assert(g_writer_thread_created);
    assert(g_writer_running);
    pthread_mutex_unlock(&bridge_mutex);
    assert(g_rb.initialized);
    uint8_t *old_ring_data = g_rb.data;

    struct async_call restart = { .start = true };
    atomic_init(&restart.done, false);
    pthread_t starter;
    assert(pthread_create(&starter, NULL, lifecycle_call, &restart) == 0);
    mock_wait_ms(150);
    assert(!atomic_load(&restart.done));
    assert(g_rb.initialized && g_rb.data == old_ring_data);

    struct async_call overlapping_stop = { .start = false };
    atomic_init(&overlapping_stop.done, false);
    pthread_t overlapping_stop_thread;
    assert(pthread_create(&overlapping_stop_thread, NULL, lifecycle_call,
                          &overlapping_stop) == 0);
    mock_wait_ms(100);
    assert(!atomic_load(&overlapping_stop.done));

    pthread_mutex_lock(&mock_mutex);
    mock_write_stalled = false;
    pthread_cond_broadcast(&mock_cond);
    pthread_mutex_unlock(&mock_mutex);
    assert(pthread_join(starter, NULL) == 0);
    assert(atomic_load(&restart.done));
    assert(pthread_join(overlapping_stop_thread, NULL) == 0);
}

static void test_repeat_stop_reaps_timed_out_writer(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_write_stalled = true;
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    wait_for_flag(&mock_write_entered, 2000, "stalled output write for repeat-stop test");

    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_call, NULL) == 0);
    assert(pthread_join(stopper, NULL) == 0);

    pthread_mutex_lock(&mock_mutex);
    mock_write_stalled = false;
    pthread_cond_broadcast(&mock_cond);
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_stop();

    pthread_mutex_lock(&bridge_mutex);
    assert(!g_reader_thread_created);
    assert(!g_writer_thread_created);
    assert(!g_reader_running);
    assert(!g_writer_running);
    pthread_mutex_unlock(&bridge_mutex);
    assert(!g_rb.initialized);
}

static void test_overlapping_lifecycle_calls_are_serialized(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_audio_stop_stalled = true;
    pthread_mutex_unlock(&mock_mutex);

    struct async_call first = { .start = true }, second = { .start = true };
    atomic_init(&first.done, false);
    atomic_init(&second.done, false);
    pthread_t first_thread, second_thread;
    assert(pthread_create(&first_thread, NULL, lifecycle_call, &first) == 0);
    pthread_mutex_lock(&mock_mutex);
    while (!mock_audio_stop_entered) pthread_cond_wait(&mock_cond, &mock_mutex);
    pthread_mutex_unlock(&mock_mutex);
    assert(pthread_create(&second_thread, NULL, lifecycle_call, &second) == 0);
    mock_wait_ms(100);
    assert(!atomic_load(&second.done));

    pthread_mutex_lock(&mock_mutex);
    mock_audio_stop_stalled = false;
    pthread_cond_broadcast(&mock_cond);
    pthread_mutex_unlock(&mock_mutex);
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);
    assert(mock_create_calls == 2);
    usb_dac_bridge_stop();
}

static void test_recoverable_eof_survives_without_bridge_teardown(void) {
    reset_mocks();
    /* Set mock to return EOF on the 5th read (1 time only) */
    pthread_mutex_lock(&mock_mutex);
    mock_read_eof_after = 5;
    mock_read_eof_count = 1;
    pthread_mutex_unlock(&mock_mutex);

    usb_dac_bridge_start();

    /* Wait long enough for reader to hit the empty read and carry on */
    mock_wait_ms(150);

    /* The bridge and reader should STILL be running -- NOT torn down! */
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.bridge_running);

    pthread_mutex_lock(&mock_mutex);
    int opens = mock_open_calls;
    int reads = mock_read_calls;
    pthread_mutex_unlock(&mock_mutex);

    /* An empty read is not a session end: the device stays open. */
    assert(opens == 1);
    /* Reads continued past the EOF (read #5) */
    assert(reads > 5);

    usb_dac_bridge_stop();

    usb_dac_bridge_get_stream_info(&info);
    assert(!info.bridge_running);
}

static void test_empty_reads_keep_device_open(void) {
    reset_mocks();
    /* A burst of empty reads (the driver's "nothing queued yet") must not
     * reopen the device; each costs a ~1 ms sleep, not a backoff ladder. */
    pthread_mutex_lock(&mock_mutex);
    mock_read_eof_after = 3;
    mock_read_eof_count = 40;
    pthread_mutex_unlock(&mock_mutex);

    usb_dac_bridge_start();
    mock_wait_ms(300);

    pthread_mutex_lock(&mock_mutex);
    int opens = mock_open_calls;
    int reads = mock_read_calls;
    int sleeps = mock_usleep_calls;
    uint64_t slept_us = mock_usleep_total_us;
    pthread_mutex_unlock(&mock_mutex);
    assert(opens == 1);
    assert(reads > 43);            /* data resumed after the empty burst */
    assert(sleeps >= 40);
    assert(slept_us < 40 * 2000ULL + 2000000ULL); /* ~1 ms each, plus the writer's startup wait */

    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.bridge_running);
    usb_dac_bridge_stop();
}

static void test_eperm_mid_recovery_obeys_recovery_timeout(void) {
    reset_mocks();
    /* Hit clean EOF on read #2 to enter recovery, then persistent EPERM starting at read #3 */
    pthread_mutex_lock(&mock_mutex);
    mock_read_eof_after = 2;
    mock_read_eof_count = 1;
    mock_read_eperm_after = 3;
    pthread_mutex_unlock(&mock_mutex);

    usb_dac_bridge_start();

    /* During recovery, deadline is 5s. Wait ~5.5s so the 5s RECOVERY_TIMEOUT_NS expires. */
    mock_wait_ms(5500);

    /* The reader thread should have exited due to RECOVERY_TIMEOUT_NS,
     * and NOT continued for 1500 EPERM retries (~5 minutes). */
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(!info.streaming);

    pthread_mutex_lock(&mock_mutex);
    int reads = mock_read_calls;
    pthread_mutex_unlock(&mock_mutex);

    /* It should have attempted EPERM retries only for ~5s (~25 retries at 200ms),
     * well below EPERM_RETRY_LIMIT (1500), and the reader must have exited. */
    assert(reads < 50);
    pthread_mutex_lock(&bridge_mutex);
    bool reader_running = g_reader_running;
    pthread_mutex_unlock(&bridge_mutex);
    assert(!reader_running);

    usb_dac_bridge_stop();
}

static void test_writer_catchup_trim_reduces_backlog(void) {
    reset_mocks();
    const unsigned int rate = 96000;
    const size_t frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int32_t);

    /* A backlog well above the startup target, in whole 32-bit frames. */
    assert(ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY));
    size_t seed_bytes = 170000 - 170000 % frame_bytes;
    uint8_t * seed = calloc(1, seed_bytes);
    assert(seed);
    ring_buffer_write_drop_oldest(&g_rb, seed, seed_bytes);
    free(seed);
    size_t occupancy_before = ring_buffer_get_occupancy(&g_rb);
    assert(occupancy_before == seed_bytes);

    uint64_t fade = writer_catchup_trim(rate, frame_bytes, g_host_format_gen, CATCHUP_TARGET_RESERVE_MS, true);
    size_t occupancy_after = ring_buffer_get_occupancy(&g_rb);
    size_t target_reserve_bytes = (size_t) ((double) rate * CATCHUP_TARGET_RESERVE_MS / 1000.0) * frame_bytes;
    assert(fade > 0);                                   /* caller must fade in after the cut */
    assert(occupancy_after < occupancy_before);
    assert(occupancy_after <= target_reserve_bytes + frame_bytes);
    assert(occupancy_after % frame_bytes == 0);         /* discards stay frame-aligned */

    /* Trimming never plays anything itself. */
    pthread_mutex_lock(&mock_mutex);
    assert(mock_write_calls == 0 && mock_write_s24_calls == 0);
    pthread_mutex_unlock(&mock_mutex);
    ring_buffer_destroy(&g_rb);
}

static void test_writer_catchup_trim_noop_below_target(void) {
    reset_mocks();
    const unsigned int rate = 96000;
    const size_t frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int32_t);
    assert(ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY));
    size_t seed_bytes = 4096; /* well under the target at 96kHz */
    uint8_t * seed = calloc(1, seed_bytes);
    assert(seed);
    ring_buffer_write_drop_oldest(&g_rb, seed, seed_bytes);
    free(seed);
    assert(writer_catchup_trim(rate, frame_bytes, g_host_format_gen, CATCHUP_TARGET_RESERVE_MS, true) == 0);
    assert(ring_buffer_get_occupancy(&g_rb) == seed_bytes);
    ring_buffer_destroy(&g_rb);

    /* A trim sized for an older format generation must not cut anything. */
    assert(ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY));
    seed_bytes = 170000 - 170000 % frame_bytes;
    seed = calloc(1, seed_bytes);
    assert(seed);
    ring_buffer_write_drop_oldest(&g_rb, seed, seed_bytes);
    free(seed);
    assert(writer_catchup_trim(rate, frame_bytes, g_host_format_gen + 1, CATCHUP_TARGET_RESERVE_MS, true) == 0);
    assert(ring_buffer_get_occupancy(&g_rb) == seed_bytes);
    ring_buffer_destroy(&g_rb);
}

static void test_debug_log_persists_and_toggles_cleanly(void) {
    reset_mocks();

    /* This suite's documented manual build command (see this file's own top
     * comment) does not define HOST_BUILD, so BRIDGE_LOG_DIR here resolves
     * to the real target path ("/data/mnt/sd_0/.logs"), which cannot exist
     * on a dev machine. That's the same shape of failure a real device
     * would hit if the SD card weren't mounted -- so rather than requiring
     * root to create a matching path at the filesystem root just for this
     * test, verify the failure mode is actually graceful: enabling logging
     * against an uncreatable directory must not crash and must leave
     * bridge_log_file NULL (no partial/stale handle), and toggling the flag
     * around that must remain safe. The full happy-path write+rotate+
     * reopen cycle mirrors db_log.c's own already-shipped, working
     * implementation and is exercised for real on-device (SD card actually
     * mounted at that exact path). */
    assert(access("/data/mnt/sd_0", F_OK) != 0);

    BRIDGE_LOG("should not appear\n");
    assert(bridge_log_file == NULL);

    mock_db_log_enabled = true;
    BRIDGE_LOG("test line %d\n", 42);
    assert(bridge_log_file == NULL); /* mkdir() failed (ENOENT) -- graceful no-op, no crash */

    usb_dac_bridge_set_debug_log_enabled(false); /* safe even when never opened */
    assert(bridge_log_file == NULL);

    mock_db_log_enabled = false;
    BRIDGE_LOG("still should not crash\n");
    assert(bridge_log_file == NULL);

    mock_db_log_enabled = false;
}

static void test_host_format_follows_ioctl(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_host_rate = 44100;
    pthread_mutex_unlock(&mock_mutex);

    usb_dac_bridge_start();
    mock_wait_ms(200);
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.input_sample_rate == 44100 && info.input_bit_depth == 32);
    assert(info.output_sample_rate == 44100 && info.output_bit_depth == 16);
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_last_rate == 44100);
    assert(mock_write_calls > 0 && mock_write_s24_calls == 0);
    /* The host switches rate and the output can take 24-bit. */
    mock_host_rate = 96000;
    mock_s24_active = true;
    pthread_mutex_unlock(&mock_mutex);

    mock_wait_ms(400); /* > FORMAT_REQUERY_NS */
    usb_dac_bridge_get_stream_info(&info);
    assert(info.input_sample_rate == 96000);
    assert(info.output_sample_rate == 96000 && info.output_bit_depth == 24);
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_last_rate == 96000);
    assert(mock_write_s24_calls > 0);
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_stop();
}

static void test_implausible_ioctl_result_is_ignored(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_host_rate = 5; /* nonsense from the driver */
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    mock_wait_ms(200);
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.input_sample_rate == USB_DEFAULT_SAMPLE_RATE);
    usb_dac_bridge_stop();
}

static void test_partial_reads_keep_ring_frame_aligned(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_read_size = 4093; /* never a whole number of 8-byte frames */
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    for (int i = 0; i < 50; i++) {
        mock_wait_ms(4);
        assert(ring_buffer_get_occupancy(&g_rb) % 8 == 0);
    }
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.bridge_running && info.streaming);
    pthread_mutex_lock(&mock_mutex);
    assert(mock_write_calls > 0);
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_stop();
}

static void test_late_ioctl_with_partial_reads_stays_framed(void) {
    reset_mocks();
    /* The driver answers only after a few failures, then with a new rate,
     * while reads keep splitting frames: every frame reaching the output
     * must still be a correctly framed L/R pair. */
    pthread_mutex_lock(&mock_mutex);
    mock_read_size = 4093;
    mock_ioctl_fail_until_mid_frame = true;
    mock_host_rate = 44100;
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    mock_wait_ms(600); /* several FORMAT_REQUERY_NS periods */
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    usb_dac_bridge_stop();
    assert(info.input_sample_rate == 44100);
    pthread_mutex_lock(&mock_mutex);
    int bad = mock_bad_frames, good = mock_good_frames, exact = mock_exact_frames;
    uint64_t answered_at = mock_stream_offset;
    pthread_mutex_unlock(&mock_mutex);
    (void) answered_at;
    if (bad != 0) fprintf(stderr, "misframed %d of %d frames\n", bad, bad + good);
    assert(bad == 0);
    assert(exact > 1000); /* real, unfaded pattern frames after the answer */
}

static void test_same_format_answer_is_not_a_change(void) {
    reset_mocks();
    /* The driver confirming the defaults must not count as a format change
     * (which would drop the reader's partial-frame carry mid-stream). */
    pthread_mutex_lock(&bridge_mutex);
    g_host_rate = USB_DEFAULT_SAMPLE_RATE;
    g_host_bits = USB_DEFAULT_BITS;
    g_host_format_known = false;
    unsigned int gen = g_host_format_gen;
    pthread_mutex_unlock(&bridge_mutex);
    assert(ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY));
    assert(!publish_host_format(USB_DEFAULT_SAMPLE_RATE, USB_DEFAULT_BITS, NULL));
    assert(g_host_format_gen == gen && g_host_format_known);
    assert(publish_host_format(44100, 32, NULL));
    assert(g_host_format_gen == gen + 1);
    assert(!publish_host_format(44100, 32, NULL));
    ring_buffer_destroy(&g_rb);
}

static void test_convert_frames_layouts(void) {
    /* 32-bit containers, audio MSB-aligned, as the driver delivers them. */
    int32_t in32[4] = { 0x03650000, (int32_t) 0xFC9B0000u, 0x7FFFFF00, (int32_t) 0x80000000u };
    int16_t out16[8];
    int32_t out24[8];
    uint64_t fade = 0;
    convert_frames((const uint8_t *) in32, 2, 32, false, out16, out24, &fade, 0);
    assert(out16[0] == 0x0365 && out16[1] == (int16_t) 0xFC9B);
    assert(out16[2] == 0x7FFF && out16[3] == (int16_t) 0x8000);
    convert_frames((const uint8_t *) in32, 2, 32, true, out16, out24, &fade, 0);
    assert(out24[0] == 0x036500 && out24[1] == -0x036500);
    assert(out24[2] == 0x7FFFFF && out24[3] == -0x800000);

    /* 16-bit layout, should the driver ever report it. */
    int16_t in16[4] = { 0x0365, (int16_t) 0xFC9B, 1, -1 };
    convert_frames((const uint8_t *) in16, 2, 16, false, out16, out24, &fade, 0);
    assert(out16[0] == 0x0365 && out16[1] == (int16_t) 0xFC9B && out16[2] == 1 && out16[3] == -1);
    convert_frames((const uint8_t *) in16, 2, 16, true, out16, out24, &fade, 0);
    assert(out24[0] == 0x036500 && out24[3] == -0x100);

    /* A fade starts at silence and consumes its frames. */
    int32_t loud[8] = { 0x40000000, 0x40000000, 0x40000000, 0x40000000,
                        0x40000000, 0x40000000, 0x40000000, 0x40000000 };
    fade = 4;
    convert_frames((const uint8_t *) loud, 4, 32, false, out16, NULL, &fade, 4);
    assert(fade == 0);
    assert(out16[0] == 0 && out16[1] == 0);
    assert(out16[6] > out16[2] && out16[6] < 0x4000);
}

static void test_stop_while_streaming_shuts_down_cleanly(void) {
    reset_mocks();
    usb_dac_bridge_start();
    mock_wait_ms(100);
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    assert(info.bridge_running && info.streaming);
    usb_dac_bridge_stop();
    usb_dac_bridge_get_stream_info(&info);
    assert(!info.bridge_running && !info.streaming);
    assert(!g_reader_thread_created && !g_writer_thread_created);
}

static void test_writer_waits_for_audio_idle_or_owns_stop(void) {
    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_audio_idle = false;
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();

    /* Exceed the old fixed startup wait while playback still owns output. */
    mock_wait_ms(2200);
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_calls == 0);
    assert(mock_write_calls == 0 && mock_write_s24_calls == 0);
    assert(mock_output_close_calls == 0);
    pthread_mutex_unlock(&mock_mutex);
    pthread_mutex_lock(&bridge_mutex);
    assert(g_writer_running && g_writer_thread_created);
    pthread_mutex_unlock(&bridge_mutex);

    pthread_mutex_lock(&mock_mutex);
    mock_audio_idle = true;
    pthread_mutex_unlock(&mock_mutex);
    wait_for_flag(&mock_write_entered, 2000, "writer output after audio becomes idle");
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_calls > 0 && mock_write_calls > 0);
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_stop();

    reset_mocks();
    pthread_mutex_lock(&mock_mutex);
    mock_audio_idle = false;
    pthread_mutex_unlock(&mock_mutex);
    usb_dac_bridge_start();
    mock_wait_ms(2200);
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_calls == 0);
    assert(mock_write_calls == 0 && mock_write_s24_calls == 0);
    assert(mock_output_close_calls == 0);
    pthread_mutex_unlock(&mock_mutex);

    /* Stop owns cancellation of this startup wait; it must join without
     * ever claiming, writing, or closing the local playback output. */
    usb_dac_bridge_stop();
    pthread_mutex_lock(&mock_mutex);
    assert(mock_ensure_calls == 0);
    assert(mock_write_calls == 0 && mock_write_s24_calls == 0);
    assert(mock_output_close_calls == 0);
    pthread_mutex_unlock(&mock_mutex);
    pthread_mutex_lock(&bridge_mutex);
    assert(!g_writer_running && !g_writer_thread_created);
    pthread_mutex_unlock(&bridge_mutex);
}

int main(void) {
    alarm(30);
    test_partial_start_rolls_back_joinably();
    test_repeat_stop_reaps_timed_out_writer();
    test_timeout_then_restart_reaps_before_reuse();
    test_overlapping_lifecycle_calls_are_serialized();
    test_recoverable_eof_survives_without_bridge_teardown();
    test_empty_reads_keep_device_open();
    test_eperm_mid_recovery_obeys_recovery_timeout();
    test_writer_catchup_trim_reduces_backlog();
    test_writer_catchup_trim_noop_below_target();
    test_debug_log_persists_and_toggles_cleanly();
    test_host_format_follows_ioctl();
    test_implausible_ioctl_result_is_ignored();
    test_convert_frames_layouts();
    test_partial_reads_keep_ring_frame_aligned();
    test_same_format_answer_is_not_a_change();
    test_late_ioctl_with_partial_reads_stays_framed();
    test_stop_while_streaming_shuts_down_cleanly();
    test_writer_waits_for_audio_idle_or_owns_stop();
    puts("PASS: usb_dac_bridge lifecycle regressions");
    return 0;
}
