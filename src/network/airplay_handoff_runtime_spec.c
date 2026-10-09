/* Runtime regression for local playback arriving while AirPlay is waiting
 * for the audio worker to release its output device. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_usleep(useconds_t usec);

static atomic_bool mock_idle = false;
static atomic_bool allow_new_writer = false;
static atomic_int opened_fifos = 0;
static atomic_int first_writer_reads = 0;
static atomic_int second_writer_reads = 0;
static atomic_int output_ensures = 0;
static atomic_int output_writes = 0;
static atomic_int output_closes = 0;

bool audio_is_idle(void) { return atomic_load(&mock_idle); }
void audio_stop(void) { }
void airplay_metadata_invalidate(void) { }
bool airplay_metadata_start(void) { return true; }
void airplay_metadata_stop(void) { }
bool subprocess_spawn_daemon(char * const argv[]) { (void) argv; return true; }
bool subprocess_run(char * const argv[], char * out, size_t size) {
    (void) argv; (void) out; (void) size; return true;
}

bool audio_output_ensure(unsigned int channels, unsigned int rate, bool low_latency, bool s24) {
    (void) channels; (void) rate; (void) low_latency; (void) s24;
    atomic_fetch_add(&output_ensures, 1);
    return true;
}
bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * written) {
    (void) buf; (void) channels;
    atomic_fetch_add(&output_writes, 1);
    if (written) *written = frames;
    return true;
}
void audio_output_close(void) { atomic_fetch_add(&output_closes, 1); }

/* The bridge and control code use these wrapped calls so the spec never
 * creates or reads the system AirPlay FIFO and never waits in the kernel. */
int __wrap_mkfifo(const char * path, mode_t mode) { (void) path; (void) mode; return 0; }
int __wrap_unlink(const char * path) { (void) path; return 0; }
int __wrap_open(const char * path, int flags, ...) {
    (void) path; (void) flags;
    return 70 + atomic_fetch_add(&opened_fifos, 1);
}
int __wrap_poll(struct pollfd * fds, nfds_t count, int timeout_ms) {
    (void) timeout_ms;
    assert(count == 1);
    int fd = fds[0].fd;
    if (fd == 70 && atomic_fetch_add(&first_writer_reads, 1) == 0) {
        fds[0].revents = POLLIN;
        return 1;
    }
    if (fd == 71 && atomic_load(&allow_new_writer) &&
        atomic_fetch_add(&second_writer_reads, 1) == 0) {
        fds[0].revents = POLLIN;
        return 1;
    }
    fds[0].revents = 0;
    __real_usleep(1000);
    return 0;
}
ssize_t __wrap_read(int fd, void * buf, size_t size) {
    assert(size >= 8);
    memset(buf, 0x2a, 8);
    if (fd == 70 || fd == 71) return 8;
    errno = EIO;
    return -1;
}
int __wrap_close(int fd) { (void) fd; return 0; }
int __wrap_usleep(useconds_t usec) { return __real_usleep(usec); }

/* Include the actual implementations so this exercises the production
 * run_session state machine and control handoff decision. */
#include "airplay_bridge.c"
#include "airplay_control.c"

static bool wait_for_pending(void) {
    for (int i = 0; i < 300; ++i) {
        if (airplay_bridge_has_active_stream() && !airplay_bridge_is_streaming()) return true;
        usleep(1000);
    }
    return false;
}

static bool wait_for_count(atomic_int * value, int expected) {
    for (int i = 0; i < 300; ++i) {
        if (atomic_load(value) >= expected) return true;
        usleep(1000);
    }
    return false;
}

int main(void) {
    assert(airplay_control_start("compas-runtime-spec"));
    assert(wait_for_pending());

    /* Local Play sees the pending handoff and disconnects/restarts shairport
     * while the first bridge session is still waiting for audio_is_idle(). */
    assert(!airplay_bridge_is_streaming());
    assert(airplay_control_disconnect_active_stream());
    assert(airplay_control_is_active());
    assert(airplay_bridge_has_active_stream() == false);
    assert(atomic_load(&output_ensures) == 0);
    assert(atomic_load(&output_writes) == 0);

    /* Releasing the old wait must not let its buffered PCM take over output. */
    atomic_store(&mock_idle, true);
    usleep(100000);
    assert(atomic_load(&output_ensures) == 0);
    assert(atomic_load(&output_writes) == 0);

    /* A genuinely new writer after restart still streams normally. */
    atomic_store(&allow_new_writer, true);
    assert(wait_for_count(&output_ensures, 1));
    assert(wait_for_count(&output_writes, 1));
    assert(airplay_bridge_is_streaming());

    airplay_control_stop();
    for (int i = 0; i < 300 && !airplay_bridge_is_stopped(); ++i) usleep(1000);
    assert(airplay_bridge_is_stopped());
    assert(atomic_load(&output_closes) >= 1);
    puts("airplay handoff runtime spec passed");
    return 0;
}
