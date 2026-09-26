#include "usb_dac_bridge.h"

#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_BUILD
  #include <poll.h>
  #include <sys/ioctl.h>
  #include "audio_output.h"
#endif

/* Persistent diagnostic log written to .logs/usb_dac_bridge.log on the SD card
 * and mirrored to stderr, gated by the shared developer options toggle
 * (Settings -> About -> Developer Options -> "Enable database logging").
 * Flushed and fsync()'d after every line -- not just fflush(), which only
 * reaches the kernel page cache -- so evidence actually reaches the SD card
 * before an unrecoverable hang or hard reboot, and rotated to .log.1 past
 * BRIDGE_LOG_MAX_BYTES. */
#include "db_log.h"
#include <stdarg.h>
#include <sys/stat.h>

#ifdef HOST_BUILD
#define BRIDGE_LOG_DIR "./music/.logs"
#else
#define BRIDGE_LOG_DIR "/data/mnt/sd_0/.logs"
#endif
#define BRIDGE_LOG_PATH BRIDGE_LOG_DIR "/usb_dac_bridge.log"
#define BRIDGE_LOG_ROTATED_PATH BRIDGE_LOG_DIR "/usb_dac_bridge.log.1"
#define BRIDGE_LOG_MAX_BYTES (2L * 1024 * 1024)

static pthread_mutex_t bridge_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE * bridge_log_file = NULL;

/* Must be called with bridge_log_mutex held and bridge_log_file non-NULL. */
static void bridge_log_rotate_if_needed_locked(void) {
    db_log_rotate_file_if_needed_locked(&bridge_log_file, BRIDGE_LOG_PATH, BRIDGE_LOG_ROTATED_PATH, BRIDGE_LOG_MAX_BYTES);
}

void usb_dac_bridge_set_debug_log_enabled(bool enabled) {
    if (!enabled) {
        pthread_mutex_lock(&bridge_log_mutex);
        if (bridge_log_file) {
            fflush(bridge_log_file);
            fclose(bridge_log_file);
            bridge_log_file = NULL;
        }
        pthread_mutex_unlock(&bridge_log_mutex);
    }
}

static uint64_t monotonic_ns(void);

static void bridge_log(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (!db_log_enabled()) return;

    pthread_mutex_lock(&bridge_log_mutex);
    if (!db_log_enabled()) {
        if (bridge_log_file) {
            fflush(bridge_log_file);
            fclose(bridge_log_file);
            bridge_log_file = NULL;
        }
        pthread_mutex_unlock(&bridge_log_mutex);
        return;
    }

    if (!bridge_log_file) {
        if (mkdir(BRIDGE_LOG_DIR, 0755) != 0 && errno != EEXIST) {
            pthread_mutex_unlock(&bridge_log_mutex);
            return;
        }
        bridge_log_file = fopen(BRIDGE_LOG_PATH, "a");
        if (!bridge_log_file) {
            pthread_mutex_unlock(&bridge_log_mutex);
            return;
        }
    }

    uint64_t now_ms = monotonic_ns() / 1000000ULL;
    fprintf(bridge_log_file, "[usb_dac_bridge] t=%llu ", (unsigned long long) now_ms);

    va_start(ap, fmt);
    vfprintf(bridge_log_file, fmt, ap);
    va_end(ap);
    fflush(bridge_log_file);
    /* fflush() only moves data from libc's buffer to the kernel page cache --
     * not necessarily to physical SD storage. This log exists specifically
     * to survive a hard power-cycle (the only recovery from a stuck USB mode
     * switch with no ADB access), so the most recent lines are exactly the
     * ones that must not be lost to write-back timing; fsync() forces them
     * out to the device before returning.
     * Throttled to at most once/second rather than every call: this runs on
     * the reader thread, which must keep up with incoming USB audio, and
     * fsync() is a blocking device write -- confirmed on real hardware
     * (2026-09-10) as a direct cause of periodic audible crackling when
     * called on every one of this device's routine ~9-15ms EOF/reopen
     * cycles (hundreds of times/second). Worst-case durability cost: up to
     * ~1s of the most recent lines lost on a genuine hard power-cut,
     * accepted given this log's own purpose is post-mortem debugging of a
     * stuck USB mode, not sub-second forensic precision. */
    static uint64_t last_fsync_ms = 0;
    if (now_ms - last_fsync_ms >= 1000) {
        fsync(fileno(bridge_log_file));
        last_fsync_ms = now_ms;
    }
    bridge_log_rotate_if_needed_locked();
    pthread_mutex_unlock(&bridge_log_mutex);
}
#define BRIDGE_LOG(...) bridge_log(__VA_ARGS__)

#define UAC_SA_DEVICE_PATH "/dev/uac_sa"
/* Host stream format, as reported by the driver itself: ioctl(fd,
 * UAC_SA_IOC_GET_INFO, int[3] {format, rate, bits}) -- the same call the stock
 * HiBy player makes after opening the device. Verified on an R1 (2026-09-26)
 * against a Linux host playing 16-, 24- and 32-bit streams at 44.1, 48 and
 * 96 kHz: rate is the host's exact rate, bits is always 32 and format 1, and
 * the data is interleaved stereo S32_LE with the audio MSB-aligned whatever
 * the host's own sample size (a 16-bit sample 0x0365 arrives as 0x03650000).
 * Reading that as 16-bit stereo -- this bridge's former assumption -- is what
 * produced a "noise" first channel, L/R alternating on one channel, and a
 * byte-rate "measurement" of twice the real rate. */
#define UAC_SA_IOC_GET_INFO 1
#define BRIDGE_CHANNELS 2
#define USB_DEFAULT_SAMPLE_RATE 48000
#define USB_DEFAULT_BITS 32
#define BRIDGE_PERIOD_FRAMES 1024

#define OPEN_RETRY_TIMEOUT_MS 5000
#define POLL_INTERVAL_MS 200
/* The driver returns 0 from read() when nothing is queued yet, not only at a
 * session boundary; the stock player simply sleeps 1 ms and reads again. The
 * device stays open through those gaps. Only a longer silence gets a reopen
 * (a real host stop/start can need one), and RECOVERY_TIMEOUT_NS of silence
 * ends the reader; the GUI restarts the bridge while DAC mode stays on. */
#define EMPTY_READ_SLEEP_US 1000
#define STREAM_IDLE_NS (250ULL * 1000000ULL)
#define REOPEN_AFTER_SILENCE_NS (1000ULL * 1000000ULL)
#define RECOVERY_TIMEOUT_NS (5ULL * 1000000000ULL)
/* Host format changes (a new rate) are picked up by re-reading the ioctl. */
#define FORMAT_REQUERY_NS (100ULL * 1000000ULL)

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool bridge_running = false;
static bool bridge_streaming = false;
static unsigned int active_output_rate = USB_DEFAULT_SAMPLE_RATE;
static unsigned int active_output_bits = 16;
/* Host stream format from the driver's ioctl, published by the reader under
 * bridge_mutex and followed by the writer. g_host_format_gen changes with
 * every published change; see publish_host_format(). */
static unsigned int g_host_rate = USB_DEFAULT_SAMPLE_RATE;
static unsigned int g_host_bits = USB_DEFAULT_BITS;
static bool g_host_format_known = false;
static unsigned int g_host_format_gen = 0;
static atomic_bool stop_requested = false;

#ifndef HOST_BUILD
/* Bounded circular byte buffer connecting the reader and writer threads.
 *
 * Buffer Capacity Rationale:
 * At the maximum supported rate (384 kHz, stereo 16-bit = 1,536,000 bytes/sec),
 * 512 KiB stores ~350 ms of audio; at standard 48 kHz (192,000 bytes/sec),
 * it stores ~2.8 seconds of audio. This easily absorbs realistic ALSA write
 * latencies (e.g. buffer draining, thread scheduling, Bluetooth aplay pipe
 * latency) across the full 32kHz-384kHz range without overflowing during
 * normal operation. On this device with >64MB RAM, 512 KiB is a lightweight,
 * trivial allocation.
 *
 * Overflow Policy:
 * When full, DROP THE OLDEST data in the ring buffer to accommodate new incoming
 * USB packets. The reader thread NEVER blocks on buffer full, ensuring the read()
 * cadence and sample rate measurement directly mirror the host's actual USB
 * data transmission rate without playback backpressure interference.
 */
#define RING_BUFFER_CAPACITY (512 * 1024)

typedef struct {
    uint8_t * data;
    size_t capacity;
    size_t head; /* write position */
    size_t tail; /* read position */
    size_t count;
    bool producer_finished;
    bool initialized;
    pthread_mutex_t mutex;
    pthread_cond_t cond_data_available;
    uint64_t dropped_bytes_total;
    uint64_t last_drop_log_ns;
} ring_buffer_t;

/* Serializes complete start/stop operations. Workers never take this mutex;
 * bridge_mutex must not be held while joining them. */
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static ring_buffer_t g_rb;
static bool g_reader_running = false;
static bool g_writer_running = false;
static pthread_t g_reader_thread;
static pthread_t g_writer_thread;
/* Owned by lifecycle_mutex; cleared only after a successful join. */
static bool g_reader_thread_created = false;
static bool g_writer_thread_created = false;

static bool ring_buffer_init(ring_buffer_t * rb, size_t capacity) {
    rb->data = malloc(capacity);
    if (!rb->data) {
        rb->capacity = 0;
        rb->head = 0;
        rb->tail = 0;
        rb->count = 0;
        rb->producer_finished = false;
        rb->initialized = false;
        rb->dropped_bytes_total = 0;
        rb->last_drop_log_ns = 0;
        return false;
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->producer_finished = false;
    rb->initialized = true;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->cond_data_available, NULL);
    rb->dropped_bytes_total = 0;
    rb->last_drop_log_ns = 0;
    return true;
}

static void ring_buffer_signal_finished(ring_buffer_t * rb) {
    if (!rb->initialized) return;
    pthread_mutex_lock(&rb->mutex);
    rb->producer_finished = true;
    pthread_cond_broadcast(&rb->cond_data_available);
    pthread_mutex_unlock(&rb->mutex);
}

static void ring_buffer_destroy(ring_buffer_t * rb) {
    if (!rb->initialized) return;
    pthread_mutex_lock(&rb->mutex);
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->capacity = 0;
    rb->count = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->producer_finished = false;
    rb->initialized = false;
    pthread_mutex_unlock(&rb->mutex);
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond_data_available);
}

/* Pushes incoming bytes into the ring buffer. If buffer would overflow,
 * drops the oldest unread data to make room, never blocking the reader. */
static void ring_buffer_write_drop_oldest(ring_buffer_t * rb, const uint8_t * src, size_t len) {
    if (len == 0 || rb->capacity == 0) return;

    uint64_t dropped_bytes_to_log = 0;
    pthread_mutex_lock(&rb->mutex);
    if (!rb->data) {
        pthread_mutex_unlock(&rb->mutex);
        return;
    }

    if (len > rb->capacity) {
        /* Incoming chunk larger than entire capacity (pathological): keep the newest suffix */
        size_t original_len = len;
        src += (len - rb->capacity);
        len = rb->capacity;

        size_t dropped_oversized = original_len - len;
        size_t previously_buffered = rb->count;
        rb->dropped_bytes_total += (uint64_t)(dropped_oversized + previously_buffered);
        rb->head = 0;
        rb->tail = 0;
        rb->count = 0;

        uint64_t now_ns = monotonic_ns();
        if (now_ns - rb->last_drop_log_ns >= 5000000000ULL) { /* rate-limit to once per ~5s */
            dropped_bytes_to_log = rb->dropped_bytes_total;
            rb->last_drop_log_ns = now_ns;
        }
    }

    size_t space = rb->capacity - rb->count;
    if (len > space) {
        size_t bytes_to_drop = len - space;
        rb->tail = (rb->tail + bytes_to_drop) % rb->capacity;
        rb->count -= bytes_to_drop;
        rb->dropped_bytes_total += (uint64_t) bytes_to_drop;

        uint64_t now_ns = monotonic_ns();
        if (now_ns - rb->last_drop_log_ns >= 5000000000ULL) { /* rate-limit to once per ~5s */
            dropped_bytes_to_log = rb->dropped_bytes_total;
            rb->last_drop_log_ns = now_ns;
        }
    }

    /* Copy src into rb->data at head (handling wrap-around) */
    size_t first_chunk = rb->capacity - rb->head;
    if (len <= first_chunk) {
        memcpy(rb->data + rb->head, src, len);
    } else {
        memcpy(rb->data + rb->head, src, first_chunk);
        memcpy(rb->data, src + first_chunk, len - first_chunk);
    }
    rb->head = (rb->head + len) % rb->capacity;
    rb->count += len;

    pthread_cond_signal(&rb->cond_data_available);
    pthread_mutex_unlock(&rb->mutex);
    if (dropped_bytes_to_log != 0) {
        BRIDGE_LOG("usb_dac_bridge: ring buffer overflow, dropped %llu bytes total (writer falling behind)\n",
                (unsigned long long) dropped_bytes_to_log);
    }
}

/* Reads up to max_len bytes from the ring buffer into dst.
 * Blocks on cond_data_available until data is available, stop_requested is set,
 * or the producer has finished.
 * Returns number of bytes read. */
static size_t ring_buffer_read(ring_buffer_t * rb, uint8_t * dst, size_t max_len) {
    if (max_len == 0) return 0;

    pthread_mutex_lock(&rb->mutex);
    while (rb->count == 0 && !stop_requested && !rb->producer_finished) {
        pthread_cond_wait(&rb->cond_data_available, &rb->mutex);
    }

    if (rb->count == 0 || !rb->data) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    size_t to_read = (rb->count < max_len) ? rb->count : max_len;
    size_t first_chunk = rb->capacity - rb->tail;
    if (to_read <= first_chunk) {
        memcpy(dst, rb->data + rb->tail, to_read);
    } else {
        memcpy(dst, rb->data + rb->tail, first_chunk);
        memcpy(dst + first_chunk, rb->data, to_read - first_chunk);
    }
    rb->tail = (rb->tail + to_read) % rb->capacity;
    rb->count -= to_read;

    pthread_mutex_unlock(&rb->mutex);
    return to_read;
}

/* Discards up to max_len bytes from the front (oldest) of the ring buffer
 * without copying them anywhere. Non-blocking: never waits for more data to
 * arrive, only discards what is CURRENTLY buffered. Returns the number of
 * bytes actually discarded (may be less than max_len if the buffer doesn't
 * currently hold that much). */
static size_t ring_buffer_discard(ring_buffer_t * rb, size_t max_len) {
    if (max_len == 0) return 0;
    pthread_mutex_lock(&rb->mutex);
    size_t to_discard = (rb->count < max_len) ? rb->count : max_len;
    rb->tail = (rb->tail + to_discard) % rb->capacity;
    rb->count -= to_discard;
    pthread_mutex_unlock(&rb->mutex);
    return to_discard;
}

static size_t ring_buffer_get_occupancy(ring_buffer_t * rb) {
    if (!rb->initialized) return 0;
    pthread_mutex_lock(&rb->mutex);
    size_t count = rb->count;
    pthread_mutex_unlock(&rb->mutex);
    return count;
}

static void update_bridge_running_state(void) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_running = !stop_requested && (g_reader_running || g_writer_running);
    if (!bridge_running) bridge_streaming = false;
    pthread_mutex_unlock(&bridge_mutex);
}

static void set_streaming(bool streaming) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_streaming = streaming && !stop_requested;
    pthread_mutex_unlock(&bridge_mutex);
}

static void set_active_output(unsigned int rate, unsigned int bits) {
    pthread_mutex_lock(&bridge_mutex);
    active_output_rate = rate;
    active_output_bits = bits;
    pthread_mutex_unlock(&bridge_mutex);
}

/* 1. READER THREAD:
 * Opens /dev/uac_sa, reads the host's stream format from the driver, and
 * pushes raw incoming bytes into the ring buffer. Never calls audio_output.h. */

/* Reader thread only, so no new-format bytes are queued before this returns.
 * On a change everything queued is in the old format: the ring is emptied
 * and the generation bumped under one bridge_mutex hold. A chunk the writer
 * took before the discard is then either played with the old generation
 * (still correct) or seen with the new one and dropped; after the discard
 * only new-format bytes can arrive. Lock order: bridge_mutex, then the ring
 * mutex (nothing takes them the other way round). Returns true when the
 * format changed. *frame_size_changed (optional) is set when the sample
 * container changed too: only then is the reader's partial-frame carry from
 * another layout; a rate-only change keeps the same framing, so the carry
 * still belongs to the continuing stream and must be kept. */
static bool publish_host_format(unsigned int rate, unsigned int bits, bool * frame_size_changed) {
    pthread_mutex_lock(&bridge_mutex);
    /* Compared against what is in effect (the defaults until the driver
     * answers), so a first answer that matches them is not a change and
     * cannot disturb framing already under way. */
    bool first = !g_host_format_known;
    g_host_format_known = true;
    bool changed = g_host_rate != rate || g_host_bits != bits;
    if (frame_size_changed) *frame_size_changed = g_host_bits != bits;
    if (changed) {
        ring_buffer_discard(&g_rb, SIZE_MAX);
        g_host_rate = rate;
        g_host_bits = bits;
        g_host_format_gen++;
    }
    pthread_mutex_unlock(&bridge_mutex);
    if (changed || first) BRIDGE_LOG("usb_dac_bridge: host stream %u Hz, %u-bit container\n", rate, bits);
    return changed;
}

/* Reads the driver's view of the host stream. Rates outside the gadget's
 * advertised 32-384 kHz range and unknown sample sizes are rejected, keeping
 * whatever was last published. 24 is accepted as a 32-bit container, the
 * only layout this driver has been seen to deliver. Returns true when the
 * frame size changed (see publish_host_format()). */
static bool query_host_format(int fd) {
    int info[3] = { 0, 0, 0 };
    if (ioctl(fd, UAC_SA_IOC_GET_INFO, info) != 0) {
        static int last_errno = 0;
        if (errno != last_errno) BRIDGE_LOG("usb_dac_bridge: format ioctl failed: %s\n", strerror(errno));
        last_errno = errno;
        return false;
    }
    unsigned int rate = info[1] > 0 ? (unsigned int) info[1] : 0;
    unsigned int bits = info[2] == 16 ? 16 : (info[2] == 24 || info[2] == 32) ? 32 : 0;
    if (rate < 32000 || rate > 384000 || bits == 0) {
        static int last_bad[3] = { -1, -1, -1 };
        if (memcmp(last_bad, info, sizeof(info)) != 0)
            BRIDGE_LOG("usb_dac_bridge: ignoring format ioctl result %d/%d/%d\n", info[0], info[1], info[2]);
        memcpy(last_bad, info, sizeof(info));
        return false;
    }
    bool frame_size_changed = false;
    publish_host_format(rate, bits, &frame_size_changed);
    return frame_size_changed;
}

static int open_uac_device(void) {
    int fd = open(UAC_SA_DEVICE_PATH, O_RDWR);
    if (fd >= 0) query_host_format(fd);
    return fd;
}

/* A reopen right after close() has been seen to fail with EBUSY on an R1
 * (the driver releasing the previous handle); keep trying for up to ~1 s
 * before giving up. */
static int reopen_uac_device(void) {
    int fd = -1;
    for (int attempt = 0; attempt < 50 && !stop_requested; attempt++) {
        fd = open_uac_device();
        if (fd >= 0 || (errno != EBUSY && errno != ENOENT)) break;
        usleep(20000);
    }
    return fd;
}

static void reader_finish(int fd, uint8_t * buf) {
    free(buf);
    if (fd >= 0) close(fd);
    /* Finish all ring access before publishing completion to stop(). */
    ring_buffer_signal_finished(&g_rb);
    pthread_mutex_lock(&bridge_mutex);
    g_reader_running = false;
    pthread_mutex_unlock(&bridge_mutex);
    update_bridge_running_state();
}

static void * bridge_reader_thread_func(void * arg) {
    (void) arg;

    int uac_fd = -1;
    for (int waited_ms = 0; waited_ms < OPEN_RETRY_TIMEOUT_MS && !stop_requested; waited_ms += POLL_INTERVAL_MS) {
        uac_fd = open_uac_device();
        if (uac_fd >= 0) break;
        usleep(POLL_INTERVAL_MS * 1000);
    }
    if (uac_fd < 0) {
        if (!stop_requested) BRIDGE_LOG("usb_dac_bridge: %s never appeared, giving up\n", UAC_SA_DEVICE_PATH);
        reader_finish(-1, NULL);
        return NULL;
    }

    /* One period of the largest container, plus room for a partial frame
     * carried over: the ring only ever receives whole frames, which the
     * writer's conversion and trims rely on. */
    const size_t max_frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int32_t);
    size_t buf_bytes = (size_t) BRIDGE_PERIOD_FRAMES * max_frame_bytes;
    uint8_t * buf = malloc(buf_bytes + max_frame_bytes);
    size_t carry = 0;
    if (!buf) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate read buffer\n");
        reader_finish(uac_fd, NULL);
        return NULL;
    }

    uint64_t total_bytes_read = 0;
    uint64_t now_ns = monotonic_ns();
    uint64_t last_data_ns = now_ns;
    uint64_t last_query_ns = now_ns;
    uint64_t last_reopen_ns = now_ns;
    int eperm_retries = 0;
    #define EPERM_RETRY_LIMIT 1500 /* ~5min at POLL_INTERVAL_MS between retries */
    int exit_errno = 0;
    const char * exit_reason = NULL;

    while (!stop_requested) {
        now_ns = monotonic_ns();
        if (now_ns - last_query_ns >= FORMAT_REQUERY_NS) {
            if (query_host_format(uac_fd)) carry = 0;
            last_query_ns = now_ns;
        }

        struct pollfd pfd = { .fd = uac_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (pr < 0 && errno == EINTR) continue;

        ssize_t n = 0;
        int read_errno = 0;
        if (pr > 0 && (pfd.revents & POLLIN)) {
            n = read(uac_fd, buf + carry, buf_bytes);
            read_errno = errno;
        }

        if (n > 0) {
            if (eperm_retries > 0) {
                BRIDGE_LOG("usb_dac_bridge: read() succeeded after %d EPERM retries\n", eperm_retries);
                eperm_retries = 0;
            }
            last_data_ns = monotonic_ns();
            total_bytes_read += (uint64_t) n;
            set_streaming(true);
            pthread_mutex_lock(&bridge_mutex);
            size_t frame_bytes = (size_t) BRIDGE_CHANNELS * (g_host_bits / 8);
            pthread_mutex_unlock(&bridge_mutex);
            size_t total = carry + (size_t) n;
            size_t whole = total - total % frame_bytes;
            /* Drops oldest if full; never blocks the reader. */
            ring_buffer_write_drop_oldest(&g_rb, buf, whole);
            carry = total - whole;
            memmove(buf, buf + whole, carry);
            continue;
        }

        if (n < 0 && read_errno == EPERM) {
            /* The host has not armed the streaming interface yet. */
            set_streaming(false);
            if (++eperm_retries == 1)
                BRIDGE_LOG("usb_dac_bridge: read() EPERM, waiting for host to arm streaming interface...\n");
            else if (eperm_retries % 25 == 0) /* ~5s periodic update */
                BRIDGE_LOG("usb_dac_bridge: still waiting on host stream (EPERM retries=%d)\n", eperm_retries);
            /* Waiting for a host that has not started yet may take minutes;
             * a stream that already ran and then stopped gets the usual
             * silence deadline, after which the GUI restarts the bridge. */
            bool stream_ran = total_bytes_read > 0;
            if (eperm_retries >= EPERM_RETRY_LIMIT ||
                (stream_ran && monotonic_ns() - last_data_ns >= RECOVERY_TIMEOUT_NS)) {
                exit_reason = stream_ran ? "host stream stopped (EPERM) for ~5s" : "EPERM retry limit reached";
                exit_errno = EPERM;
                break;
            }
            usleep(POLL_INTERVAL_MS * 1000);
            continue;
        }
        if (n < 0 && read_errno != EINTR && read_errno != EAGAIN) {
            exit_reason = "read() failed";
            exit_errno = read_errno;
            break; /* gadget likely torn down underneath us */
        }
        if (pr < 0 || (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL)))) {
            exit_reason = "poll() reported an error";
            exit_errno = pr < 0 ? errno : EIO;
            break;
        }

        /* Nothing queued (read() returned 0, EAGAIN, or poll() timed out). */
        now_ns = monotonic_ns();
        uint64_t silent_ns = now_ns - last_data_ns;
        if (silent_ns >= STREAM_IDLE_NS) set_streaming(false);
        if (silent_ns >= RECOVERY_TIMEOUT_NS) {
            exit_reason = "no data for ~5s";
            break;
        }
        if (silent_ns >= REOPEN_AFTER_SILENCE_NS && now_ns - last_reopen_ns >= REOPEN_AFTER_SILENCE_NS) {
            close(uac_fd);
            uac_fd = reopen_uac_device();
            last_reopen_ns = monotonic_ns();
            /* After a full second of empty reads the driver queue is empty,
             * and USB audio packets carry whole frames, so a carried partial
             * frame has no remainder left to arrive: it is dropped and the
             * next byte read starts a frame. This driver has only been
             * seen returning whole frames, so a carry here would itself be
             * news worth logging. */
            if (carry != 0) BRIDGE_LOG("usb_dac_bridge: dropping %zu carried bytes at reopen\n", carry);
            carry = 0;
            if (uac_fd < 0) {
                exit_reason = "reopen failed";
                exit_errno = errno;
                break;
            }
            continue;
        }
        if (pr > 0) usleep(EMPTY_READ_SLEEP_US); /* poll() said readable but nothing came */
    }

    if (!stop_requested) {
        BRIDGE_LOG("usb_dac_bridge: reader exiting (%s, errno=%d %s) after %llu bytes total\n",
                   exit_reason ? exit_reason : "unknown", exit_errno, strerror(exit_errno),
                   (unsigned long long) total_bytes_read);
    } else {
        BRIDGE_LOG("usb_dac_bridge: reader stopped, %llu bytes read total\n", (unsigned long long) total_bytes_read);
    }
    reader_finish(uac_fd, buf);
    return NULL;
}

/* Backlog targets. With the device no longer reopened on every empty read,
 * the reader delivers steadily, so a small cushion is enough. Hosts keep
 * their own clock (measured on an R1, 2026-09-26: Linux and macOS ran
 * ~0.08-0.09% fast), so the periodic check trims the surplus: about 9 ms,
 * faded in, every ~12 s at these targets. */
#define CATCHUP_TARGET_RESERVE_MS 40.0   /* at startup and on a rate change */
#define CATCHUP_PERIODIC_INTERVAL_MS 500.0
#define CATCHUP_PERIODIC_TARGET_RESERVE_MS 60.0
#define CATCHUP_FADE_MS 10.0             /* fade-in after a trim, so the skip does not click */

/* Discards ring backlog above target_reserve_ms (valid audio, just queued
 * longer than needed) and returns how many frames of fade-in the caller must
 * apply to what it plays next, or 0 when nothing was discarded. Frame size
 * follows the host container; discards stay frame-aligned. */
static uint64_t writer_catchup_trim(unsigned int rate, size_t frame_bytes, unsigned int format_gen,
                                    double target_reserve_ms, bool verbose) {
    size_t target_reserve_bytes = (size_t) ((double) rate * target_reserve_ms / 1000.0) * frame_bytes;
    /* Measured and cut under bridge_mutex (then the ring mutex, the same
     * order as publish_host_format()), and only while the ring still holds
     * the format the caller sized frame_bytes for: a trim computed for one
     * layout must never cut into another's frames. */
    pthread_mutex_lock(&bridge_mutex);
    if (g_host_format_gen != format_gen) {
        pthread_mutex_unlock(&bridge_mutex);
        return 0;
    }
    size_t occupancy_before = ring_buffer_get_occupancy(&g_rb);
    size_t discarded = 0;
    if (occupancy_before > target_reserve_bytes) {
        size_t excess = occupancy_before - target_reserve_bytes;
        excess -= excess % frame_bytes;
        discarded = ring_buffer_discard(&g_rb, excess);
    }
    pthread_mutex_unlock(&bridge_mutex);
    if (discarded == 0) return 0;

    if (verbose) {
        BRIDGE_LOG("usb_dac_bridge: catch-up trim: discarded %zu bytes (occupancy %zu -> %zu bytes, target ~%zu bytes, rate %u Hz)\n",
                   discarded, occupancy_before, ring_buffer_get_occupancy(&g_rb), target_reserve_bytes, rate);
    } else {
        static uint64_t silent_discarded_bytes_total = 0;
        static uint64_t last_silent_log_ns = 0;
        silent_discarded_bytes_total += (uint64_t) discarded;
        uint64_t now_ns = monotonic_ns();
        if (now_ns - last_silent_log_ns >= 10000000000ULL) { /* rate-limit to once per ~10s */
            BRIDGE_LOG("usb_dac_bridge: catch-up trim: discarded %llu bytes since last log (occupancy now %zu bytes, target ~%zu bytes, rate %u Hz)\n",
                       (unsigned long long) silent_discarded_bytes_total, ring_buffer_get_occupancy(&g_rb),
                       target_reserve_bytes, rate);
            silent_discarded_bytes_total = 0;
            last_silent_log_ns = now_ns;
        }
    }
    uint64_t fade_frames = (uint64_t) ((double) rate * CATCHUP_FADE_MS / 1000.0);
    return fade_frames > 0 ? fade_frames : 1;
}

/* Converts interleaved host frames to the output format. 32-bit containers
 * carry the audio MSB-aligned; S24 output is right-justified in 32 bits. The
 * first *fade_remaining frames (of fade_total) ramp up linearly. */
static void convert_frames(const uint8_t * in, size_t frames, unsigned int bits, bool s24,
                           int16_t * out16, int32_t * out24,
                           uint64_t * fade_remaining, uint64_t fade_total) {
    for (size_t i = 0; i < frames; i++) {
        double gain = 1.0;
        if (*fade_remaining > 0 && fade_total > 0) {
            gain = (double) (fade_total - *fade_remaining) / (double) fade_total;
            (*fade_remaining)--;
        }
        for (unsigned int c = 0; c < BRIDGE_CHANNELS; c++) {
            size_t idx = i * BRIDGE_CHANNELS + c;
            int32_t s32;
            if (bits == 16) {
                int16_t s16;
                memcpy(&s16, in + idx * sizeof(int16_t), sizeof(s16));
                s32 = (int32_t) ((uint32_t) (uint16_t) s16 << 16);
            } else {
                memcpy(&s32, in + idx * sizeof(int32_t), sizeof(s32));
            }
            if (gain < 1.0) s32 = (int32_t) ((double) s32 * gain);
            if (s24) out24[idx] = s32 >> 8;
            else out16[idx] = (int16_t) (s32 >> 16);
        }
    }
}

static void writer_finish(uint8_t * in, int16_t * out16, int32_t * out24, bool close_output) {
    free(in);
    free(out16);
    free(out24);
    if (close_output) audio_output_close();
    pthread_mutex_lock(&bridge_mutex);
    g_writer_running = false;
    pthread_mutex_unlock(&bridge_mutex);
    update_bridge_running_state();
}

/* 2. WRITER THREAD:
 * Only thread calling audio_output.h. Takes host frames from the ring buffer,
 * follows the host format the reader publishes, converts, and plays them. */
static void * bridge_writer_thread_func(void * arg) {
    (void) arg;

    /* audio_stop() (called by usb_dac_bridge_start() before spawning threads)
     * only signals the audio thread -- audio_output_close() happens
     * asynchronously on that thread. Wait for it to actually finish before
     * touching the shared output device ourselves, or audio_output_ensure()
     * below will fail with EBUSY. */
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (!audio_is_playing() && !audio_is_paused()) break;
        usleep(20000);
    }

    /* Room for one period of the largest container plus a partial frame
     * carried over between reads. */
    const size_t max_frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int32_t);
    const size_t in_cap = (size_t) BRIDGE_PERIOD_FRAMES * max_frame_bytes + max_frame_bytes;
    uint8_t * in = malloc(in_cap);
    int16_t * out16 = malloc((size_t) BRIDGE_PERIOD_FRAMES * BRIDGE_CHANNELS * sizeof(int16_t));
    int32_t * out24 = malloc((size_t) BRIDGE_PERIOD_FRAMES * BRIDGE_CHANNELS * sizeof(int32_t));
    if (!in || !out16 || !out24) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate write buffers\n");
        stop_requested = true;
        ring_buffer_signal_finished(&g_rb);
        writer_finish(in, out16, out24, true);
        return NULL;
    }

    unsigned int rate = 0, bits = 0, format_gen = 0;
    bool started_output = false;
    size_t have = 0;
    uint64_t fade_remaining = 0, fade_total = 0;
    uint64_t last_periodic_trim_ns = 0;

    while (!stop_requested) {
        /* Whole frames already held are played before waiting for more, so
         * they neither stall while the host pauses nor get lost at the end. */
        size_t held_frame_bytes = (size_t) BRIDGE_CHANNELS * (bits / 8);
        if (!started_output || have < held_frame_bytes) {
            size_t n = ring_buffer_read(&g_rb, in + have, in_cap - have);
            if (n == 0) {
                /* Woke up due to stop_requested or empty buffer after reader finished */
                if (stop_requested) break;
                pthread_mutex_lock(&g_rb.mutex);
                bool finished = g_rb.producer_finished && (g_rb.count == 0);
                pthread_mutex_unlock(&g_rb.mutex);
                if (finished) break;
                continue;
            }
            have += n;
        }

        /* Sampled after the read: bytes read under a newer generation than
         * the output was opened for are dropped below, never played. */
        pthread_mutex_lock(&bridge_mutex);
        unsigned int host_rate = g_host_rate;
        unsigned int host_bits = g_host_bits;
        unsigned int host_gen = g_host_format_gen;
        pthread_mutex_unlock(&bridge_mutex);

        if (!started_output || host_gen != format_gen) {
            if (started_output) {
                BRIDGE_LOG("usb_dac_bridge: host format %u Hz/%u -> %u Hz/%u, reopening output\n",
                           rate, bits, host_rate, host_bits);
            }
            rate = host_rate;
            bits = host_bits;
            format_gen = host_gen;
            if (!audio_output_ensure(BRIDGE_CHANNELS, rate, true, true)) {
                BRIDGE_LOG("usb_dac_bridge: audio_output_ensure failed (rate=%u)\n", rate);
                if (!started_output) {
                    stop_requested = true;
                    ring_buffer_signal_finished(&g_rb);
                    writer_finish(in, out16, out24, false);
                    return NULL;
                }
            }
            started_output = true;
            size_t frame_bytes = (size_t) BRIDGE_CHANNELS * (bits / 8);
            /* Backlog built up before output was ready (or in the old
             * format) is stale: skip to the newest audio. The reader only
             * queues whole frames, so dropping `have` keeps alignment. */
            have = 0;
            fade_total = fade_remaining = writer_catchup_trim(rate, frame_bytes, format_gen, CATCHUP_TARGET_RESERVE_MS, true);
            if (fade_total == 0) fade_total = fade_remaining = (uint64_t) ((double) rate * CATCHUP_FADE_MS / 1000.0);
            last_periodic_trim_ns = monotonic_ns();
            continue;
        }

        /* Cheap recheck; reopens only when the output target (e.g. Bluetooth
         * routing) changed. */
        if (!audio_output_ensure(BRIDGE_CHANNELS, rate, true, true)) {
            BRIDGE_LOG("usb_dac_bridge: audio_output_ensure failed (rate=%u)\n", rate);
        }
        bool s24 = audio_output_is_s24_active();
        set_active_output(rate, s24 ? 24 : 16);

        size_t frame_bytes = (size_t) BRIDGE_CHANNELS * (bits / 8);
        size_t frames = have / frame_bytes;
        bool write_ok = true;
        if (frames > 0) {
            if (frames > BRIDGE_PERIOD_FRAMES) frames = BRIDGE_PERIOD_FRAMES;
            convert_frames(in, frames, bits, s24, out16, out24, &fade_remaining, fade_total);
            uint64_t written_frames = 0;
            write_ok = s24 ? audio_output_write_s24(out24, frames, BRIDGE_CHANNELS, &written_frames)
                           : audio_output_write(out16, frames, BRIDGE_CHANNELS, &written_frames);
            if (!write_ok || written_frames != (uint64_t) frames) {
                BRIDGE_LOG("usb_dac_bridge: audio_output_write failure/partial: ok=%d written=%" PRIu64 "/%zu frames\n",
                           write_ok ? 1 : 0, written_frames, frames);
            }
            size_t used = frames * frame_bytes;
            memmove(in, in + used, have - used);
            have -= used;
        }

        /* !stop_requested: keeps worst-case shutdown latency within
         * usb_dac_bridge_stop()'s join timeout. */
        if (write_ok && !stop_requested) {
            uint64_t now_ns = monotonic_ns();
            if (now_ns - last_periodic_trim_ns >= (uint64_t) (CATCHUP_PERIODIC_INTERVAL_MS * 1000000.0)) {
                uint64_t fade = writer_catchup_trim(rate, frame_bytes, format_gen, CATCHUP_PERIODIC_TARGET_RESERVE_MS, false);
                if (fade > 0) {
                    /* What is still in `in` precedes the cut; drop it so the
                     * fade starts right after the discontinuity. */
                    have = 0;
                    fade_total = fade_remaining = fade;
                }
                last_periodic_trim_ns = now_ns;
            }
        }
    }

    BRIDGE_LOG("usb_dac_bridge: writer thread exiting\n");
    writer_finish(in, out16, out24, true);
    return NULL;
}

/* Caller holds lifecycle_mutex, never bridge_mutex. A failed join retains
 * ownership so no caller may destroy/reuse resources still owned by a worker. */
static bool join_owned_thread(pthread_t thread, bool * created) {
    if (!*created) return true;
    int rc = pthread_join(thread, NULL);
    if (rc != 0) {
        BRIDGE_LOG("usb_dac_bridge: pthread_join failed: %s\n", strerror(rc));
        return false;
    }
    *created = false;
    return true;
}

#endif

void usb_dac_bridge_start(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&bridge_mutex);
    bool running = bridge_running;
    pthread_mutex_unlock(&bridge_mutex);
    if (running) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    /* This may block on a prior timed-out stop. Keep its buffer and stop
     * request intact until every owned thread has actually been joined. */
    bool reader_joined = join_owned_thread(g_reader_thread, &g_reader_thread_created);
    bool writer_joined = join_owned_thread(g_writer_thread, &g_writer_thread_created);
    if (!reader_joined || !writer_joined) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    ring_buffer_destroy(&g_rb);

    pthread_mutex_lock(&bridge_mutex);
    if (!ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY)) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate %d bytes for ring buffer\n", RING_BUFFER_CAPACITY);
        g_reader_running = false;
        g_writer_running = false;
        bridge_running = false;
        bridge_streaming = false;
        pthread_mutex_unlock(&bridge_mutex);
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    stop_requested = false;
    bridge_running = true;
    bridge_streaming = false;
    g_host_rate = USB_DEFAULT_SAMPLE_RATE;
    g_host_bits = USB_DEFAULT_BITS;
    g_host_format_known = false;
    active_output_rate = USB_DEFAULT_SAMPLE_RATE;
    active_output_bits = 16;

    /* Clear external USB DAC output request: operating as a USB audio device
     * gadget is mutually exclusive with driving an external USB host DAC. */
    audio_output_set_usb_requested(false, NULL);

    audio_stop();

    int rc_reader = pthread_create(&g_reader_thread, NULL, bridge_reader_thread_func, NULL);
    if (rc_reader == 0) {
        g_reader_running = true;
        g_reader_thread_created = true;
    } else {
        BRIDGE_LOG("usb_dac_bridge: failed to create reader thread: %s\n", strerror(rc_reader));
        g_reader_running = false;
    }

    int rc_writer = pthread_create(&g_writer_thread, NULL, bridge_writer_thread_func, NULL);
    if (rc_writer == 0) {
        g_writer_running = true;
        g_writer_thread_created = true;
    } else {
        BRIDGE_LOG("usb_dac_bridge: failed to create writer thread: %s\n", strerror(rc_writer));
        g_writer_running = false;
    }

    /* All-or-nothing: both threads must start successfully. If either failed,
     * immediately tear down whatever started and leave bridge cleanly stopped. */
    if (rc_reader != 0 || rc_writer != 0) {
        BRIDGE_LOG("usb_dac_bridge: partial startup failure (reader=%d writer=%d), rolling back\n",
                rc_reader, rc_writer);
        stop_requested = true;
        ring_buffer_signal_finished(&g_rb);

        bridge_running = false;
        bridge_streaming = false;
        pthread_mutex_unlock(&bridge_mutex);

        bool reader_joined = join_owned_thread(g_reader_thread, &g_reader_thread_created);
        bool writer_joined = join_owned_thread(g_writer_thread, &g_writer_thread_created);
        if (reader_joined && writer_joined) ring_buffer_destroy(&g_rb);
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    pthread_mutex_unlock(&bridge_mutex);
    pthread_mutex_unlock(&lifecycle_mutex);
#endif
}

void usb_dac_bridge_stop(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&lifecycle_mutex);
    stop_requested = true;
    pthread_mutex_lock(&bridge_mutex);
    bridge_running = false;
    bridge_streaming = false;
    pthread_mutex_unlock(&bridge_mutex);

    /* Start the worker wait budget before waking the writer. Serialization
     * itself may wait for an overlapping start to reap an older session. */
    const uint64_t stop_timeout_ns = 3000000000ULL; /* 3.0 seconds */
    uint64_t stop_start_ns = monotonic_ns();
    if (g_rb.initialized) {
        pthread_mutex_lock(&g_rb.mutex);
        pthread_cond_broadcast(&g_rb.cond_data_available);
        pthread_mutex_unlock(&g_rb.mutex);
    }
    bool r_done = !g_reader_thread_created;
    bool w_done = !g_writer_thread_created;

    while (!r_done || !w_done) {
        pthread_mutex_lock(&bridge_mutex);
        if (!r_done && !g_reader_running) r_done = true;
        if (!w_done && !g_writer_running) w_done = true;
        pthread_mutex_unlock(&bridge_mutex);

        if (r_done && w_done) break;
        if ((monotonic_ns() - stop_start_ns) >= stop_timeout_ns) break;
        usleep(20000); /* 20ms poll */
    }

    /* Completion flags are published after blocking worker cleanup. Only a
     * successful join releases ownership; timeouts leave it for the next call. */
    if (r_done) join_owned_thread(g_reader_thread, &g_reader_thread_created);
    if (w_done) join_owned_thread(g_writer_thread, &g_writer_thread_created);

    if (!g_reader_thread_created && !g_writer_thread_created) {
        ring_buffer_destroy(&g_rb);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
#endif
}

void usb_dac_bridge_set_bt_output(bool enabled) {
#ifndef HOST_BUILD
    /* Same shared flag audio_set_bt_output() itself sets (see
     * audio_output_set_bt_requested()) -- both call into the same
     * audio_output module, since only one of this bridge's thread or
     * audio.c's own playback thread is ever actually running at a time
     * (see audio_output.h's own comment on why that makes sharing this
     * safe without a lock). */
    audio_output_set_bt_requested(enabled);
#else
    (void) enabled;
#endif
}

void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bridge_mutex);
    out->bridge_running = bridge_running;
    out->streaming = bridge_streaming;
    out->output_sample_rate = active_output_rate;
    out->output_bit_depth = active_output_bits;
    out->input_sample_rate = g_host_rate;
    out->input_bit_depth = g_host_bits;
    pthread_mutex_unlock(&bridge_mutex);
}
