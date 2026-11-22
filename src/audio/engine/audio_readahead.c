/* Local read-ahead: logical reader, one fetcher thread, one ring window.
 *
 * The window holds file bytes [w_start, w_end); byte at file offset o lives
 * at buf[o % cap]. The fetcher preads into the free part of the ring without
 * the lock and publishes by moving w_end. The audio thread copies out under
 * the lock (short memcpy, never I/O). Everything else about the claimed
 * reader is the FILE the reader was opened with, used as the fallback. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "audio_readahead.h"

#include "../../core/system/logging/db_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef RA_TEST_HOOK
#define RA_PREAD ra_test_pread
#define RA_MALLOC ra_test_malloc
#else
#define RA_PREAD pread
#define RA_MALLOC malloc
#endif

enum {
    RA_FIRST_BLOCK = 16 * 1024,   /* first read after a reset costs one small read, as a seek does today */
    RA_BLOCK = 32 * 1024,         /* smallest fetch block, and the one for an unknown rate */
    RA_MAX_BLOCK = 1024 * 1024,
    RA_STACK = 64 * 1024,
    RA_IDLE_FREE_MS = 5000,       /* unclaimed this long: give the window back */
    RA_FETCHER_NICE = -15,        /* the audio thread's level; set absolutely */
    RA_IOPRIO_CLASS_SHIFT = 13,
    RA_IOPRIO_CLASS_BE = 2,
    RA_IOPRIO_WHO_PROCESS = 1,
};

struct ra_reader {
    FILE * file;
    int64_t size;
    int64_t pos;        /* logical position; under g.lock while windowed */
    int64_t file_pos;   /* where the FILE is, -1 when unknown */
    bool claim_done;    /* ra_claim() was decided for this reader, once */
    bool windowed;      /* it may own the window */
    bool err;           /* the last read stopped on a read error, not on the end of the file */
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
    bool sleeping;          /* fetcher waits on cond */
    ra_reader_t * owner;
    uint64_t epoch;         /* bumped on every claim and release */
    int pending_fd;         /* dup'd fd handed to the fetcher, -1 none */
    int fd;                 /* the fetcher's current fd, -1 none */
    bool in_pread;          /* the fetcher is inside pread on g.fd */
    int64_t size;
    size_t want_cap;
    int64_t want_block;     /* fetch block for the claimed rate */
    int64_t block;          /* the window's fetch block */
    bool ready;             /* window set up for the current owner */
    uint8_t * buf;
    size_t cap;
    int64_t w_start, w_end;
    int64_t fail_end;       /* a read failed or hit EOF at this offset, -1 none */
    bool fresh;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .pending_fd = -1, .fd = -1, .fail_end = -1 };

#ifdef RA_TEST_HOOK
uint64_t ra_test_window_bytes; /* bytes served from the window */
#define RA_COUNT(n) (ra_test_window_bytes += (n))
#else
#define RA_COUNT(n) ((void) 0)
#endif

typedef enum { JOB_NONE, JOB_FETCH, JOB_RESET } ra_job_t;

static int64_t history(void) { return (int64_t) (g.cap / 4); }

static int64_t floor_locked(int64_t pos) {
    int64_t lo = pos - history();
    return lo < g.w_start ? g.w_start : lo;
}

static int64_t limit_locked(void) {
    return g.fail_end >= 0 && g.fail_end < g.size ? g.fail_end : g.size;
}

/* What the fetcher should do for the owner's position. Lock held. */
static ra_job_t job_locked(void) {
    int64_t pos = g.owner->pos;
    if (pos < g.w_start || pos > g.w_end) return JOB_RESET;
    int64_t space = floor_locked(pos) + (int64_t) g.cap - g.w_end;
    int64_t rem = limit_locked() - g.w_end;
    if (rem <= 0) return JOB_NONE;
    return space >= g.block || space >= rem ? JOB_FETCH : JOB_NONE;
}

static void kick_locked(void) {
    if (g.sleeping && g.owner && g.ready && job_locked() != JOB_NONE) pthread_cond_signal(&g.cond);
}

static size_t class_for(uint64_t bytes_per_second) {
    uint64_t want = bytes_per_second * 2;
    if (bytes_per_second == 0) return 1024 * 1024;
    /* 4 MiB only for DSD128 and up or very high-rate PCM: 24/192 stereo stays at 2 MiB. */
    return want <= 512 * 1024 ? 512 * 1024 : want <= 1024 * 1024 ? 1024 * 1024
         : want <= 2560 * 1024 ? 2048 * 1024 : 4096 * 1024;
}

/* One pread is about a quarter second of content, in 32 KiB steps: the I/O
 * scheduler shares the card per request, so a bigger request gets a bigger
 * share against other readers' large readahead. */
static int64_t block_for(uint64_t bytes_per_second) {
    if (bytes_per_second == 0) return RA_BLOCK;
    uint64_t block = (bytes_per_second / 4 + RA_BLOCK - 1) / RA_BLOCK * RA_BLOCK;
    return block < RA_BLOCK ? RA_BLOCK : block > RA_MAX_BLOCK ? RA_MAX_BLOCK : (int64_t) block;
}

/* --- fetcher --- */

/* Replaces the buffer with one of the wanted size; unlocks while it
 * allocates. Lock held on entry and on return. A failed allocation gives the
 * claim up (if no newer claim arrived meanwhile), so reads stay direct. */
static void resize_buffer_locked(void) {
    size_t cap = g.want_cap;
    uint8_t * old;
    uint64_t epoch = g.epoch;
    pthread_mutex_unlock(&g.lock);
    uint8_t * nb = RA_MALLOC(cap);
    if (nb) (void) mlock(nb, cap);
    pthread_mutex_lock(&g.lock);
    if (!nb) {
        /* A newer claim gets its own attempt on the next pass. */
        if (epoch == g.epoch) {
            g.owner = NULL;
            g.epoch++;
        }
        return;
    }
    old = g.buf;
    g.buf = nb;
    g.cap = cap;
    pthread_mutex_unlock(&g.lock);
    free(old);
    pthread_mutex_lock(&g.lock);
}

static void prepare_window_locked(void) {
    while (g.owner && g.cap != g.want_cap) resize_buffer_locked();
    if (!g.owner) return;
    g.block = g.want_block;
    g.w_start = g.w_end = g.owner->pos;
    g.fail_end = -1;
    g.fresh = true;
    g.ready = true;
}

/* No owner: wait, and give the buffer back after a while. */
static void park_locked(void) {
    g.sleeping = true;
    if (!g.buf) {
        pthread_cond_wait(&g.cond, &g.lock);
    } else {
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += RA_IDLE_FREE_MS / 1000;
        if (pthread_cond_timedwait(&g.cond, &g.lock, &until) == ETIMEDOUT && !g.owner) {
            uint8_t * old = g.buf;
            g.buf = NULL;
            g.cap = 0;
            pthread_mutex_unlock(&g.lock);
            free(old);
            pthread_mutex_lock(&g.lock);
        }
    }
    g.sleeping = false;
}

/* The fd to close now, if any: the old one once a new one arrived, or the
 * current one once nobody owns it. Never the one a pread is using. Lock held;
 * the caller closes it after unlocking. */
static int retire_fd_locked(void) {
    int old = -1;
    if (g.in_pread) return -1;
    if (g.pending_fd >= 0) {
        if (g.owner) {
            old = g.fd;
            g.fd = g.pending_fd;
        } else {
            old = g.pending_fd; /* its claim was given up: nobody to serve */
        }
        g.pending_fd = -1;
    } else if (!g.owner) {
        old = g.fd;
        g.fd = -1;
    }
    return old;
}

static void fetch_once_locked(ra_job_t job) {
    if (job == JOB_RESET) {
        g.w_start = g.w_end = g.owner->pos;
        g.fail_end = -1;
        g.fresh = true;
    }
    g.w_start = floor_locked(g.owner->pos);
    int64_t off = g.w_end;
    int64_t space = g.w_start + (int64_t) g.cap - off;
    int64_t rem = limit_locked() - off;
    size_t idx = (size_t) (off % (int64_t) g.cap);
    int64_t n = g.fresh ? RA_FIRST_BLOCK : g.block;
    if (n > space) n = space;
    if (n > rem) n = rem;
    if (n > (int64_t) (g.cap - idx)) n = (int64_t) (g.cap - idx);
    if (n <= 0) return; /* reset past the end of the file */
    uint8_t * dst = g.buf + idx;
    uint64_t epoch = g.epoch;
    int fd = g.fd;
    g.in_pread = true;
    pthread_mutex_unlock(&g.lock);

    ssize_t got;
    do got = RA_PREAD(fd, dst, (size_t) n, (off_t) off); while (got < 0 && errno == EINTR);
    int err = got < 0 ? errno : 0;

    pthread_mutex_lock(&g.lock);
    g.in_pread = false;
    if (epoch != g.epoch) return; /* released or replaced meanwhile: stale */
    if (got > 0) {
        g.w_end += got;
        g.fresh = false;
        return;
    }
    g.fail_end = g.w_end;
    pthread_mutex_unlock(&g.lock); /* the log writes a file: never under the lock */
    DB_LOG("audio", "readahead: %s at offset %lld", got < 0 ? strerror(err) : "short file", (long long) off);
    pthread_mutex_lock(&g.lock);
}

static void * fetcher_main(void * unused) {
    (void) unused;
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "readahead", 0, 0, 0);
#endif
    (void) setpriority(PRIO_PROCESS, 0, RA_FETCHER_NICE);
#ifdef SYS_ioprio_set
    /* Highest best-effort I/O level (0): CFQ favours it over normal readers and
     * writers. Not the real-time class, which would also put the audio thread's
     * own direct reads (crossfade into an uncached track, A/B) behind it; on the
     * R1 best-effort 0 measured better under contention anyway. Best effort. */
    (void) syscall(SYS_ioprio_set, RA_IOPRIO_WHO_PROCESS, 0, (RA_IOPRIO_CLASS_BE << RA_IOPRIO_CLASS_SHIFT) | 0);
#endif
    pthread_mutex_lock(&g.lock);
    for (;;) {
        int stale = retire_fd_locked();
        if (stale >= 0) {
            pthread_mutex_unlock(&g.lock);
            close(stale);
            pthread_mutex_lock(&g.lock);
            continue;
        }
        if (!g.owner) { park_locked(); continue; }
        if (!g.ready) { prepare_window_locked(); continue; }
        ra_job_t job = job_locked();
        if (job == JOB_NONE) {
            g.sleeping = true;
            pthread_cond_wait(&g.cond, &g.lock);
            g.sleeping = false;
            continue;
        }
        fetch_once_locked(job);
    }
    return NULL;
}

void ra_start(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g.cond, &attr);
    pthread_condattr_destroy(&attr);
    pthread_t thread;
    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setstacksize(&tattr, RA_STACK);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    g.running = pthread_create(&thread, &tattr, fetcher_main, NULL) == 0;
    pthread_attr_destroy(&tattr);
}

/* --- reader --- */

ra_reader_t * ra_open(const char * path) {
    FILE * file = fopen(path, "rb");
    if (!file) return NULL;
    struct stat st;
    ra_reader_t * r = calloc(1, sizeof(*r));
    if (!r) { fclose(file); return NULL; }
    r->file = file;
    r->size = fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode) ? (int64_t) st.st_size : 0;
    return r;
}

void ra_close(ra_reader_t * r) {
    if (!r) return;
    if (r->windowed) {
        int unused = -1, stale = -1;
        pthread_mutex_lock(&g.lock);
        if (g.owner == r) {
            g.owner = NULL;
            g.ready = false;
            g.epoch++;
            unused = g.pending_fd;
            g.pending_fd = -1;
            stale = retire_fd_locked(); /* -1 while a pread is stuck: the fetcher closes it */
            pthread_cond_signal(&g.cond);
        }
        pthread_mutex_unlock(&g.lock);
        if (unused >= 0) close(unused);
        if (stale >= 0) close(stale);
    }
    fclose(r->file);
    free(r);
}

int64_t ra_size(const ra_reader_t * r) { return r->size; }

int ra_is_claimed(const ra_reader_t * r) { return r->claim_done; }

void ra_claim(ra_reader_t * r, uint64_t bytes_per_second) {
    if (!r || r->claim_done || !g.running) return;
    r->claim_done = true;
    if (r->size <= 0) return;
    int fd = dup(fileno(r->file));
    if (fd < 0) return;
    pthread_mutex_lock(&g.lock);
    int unused = g.pending_fd;
    g.pending_fd = fd;
    g.owner = r;
    g.epoch++;
    g.ready = false;
    g.size = r->size;
    g.want_cap = class_for(bytes_per_second);
    g.want_block = block_for(bytes_per_second);
    r->windowed = true;
    pthread_cond_signal(&g.cond);
    pthread_mutex_unlock(&g.lock);
    if (unused >= 0) close(unused);
}

static void set_pos(ra_reader_t * r, int64_t pos) {
    if (!r->windowed) { r->pos = pos; return; }
    pthread_mutex_lock(&g.lock);
    r->pos = pos;
    kick_locked();
    pthread_mutex_unlock(&g.lock);
}

static size_t window_copy(ra_reader_t * r, uint8_t * dst, size_t bytes) {
    size_t got = 0;
    pthread_mutex_lock(&g.lock);
    if (g.owner == r && g.ready && r->pos >= g.w_start && r->pos < g.w_end) {
        int64_t avail = g.w_end - r->pos;
        got = bytes < (size_t) avail ? bytes : (size_t) avail;
        size_t idx = (size_t) (r->pos % (int64_t) g.cap);
        size_t first = got < g.cap - idx ? got : g.cap - idx;
        memcpy(dst, g.buf + idx, first);
        memcpy(dst + first, g.buf, got - first);
        r->pos += (int64_t) got;
        RA_COUNT(got);
    }
    kick_locked();
    pthread_mutex_unlock(&g.lock);
    return got;
}

static size_t direct_read(ra_reader_t * r, uint8_t * dst, size_t bytes) {
    if (r->file_pos != r->pos) {
        if (fseeko(r->file, (off_t) r->pos, SEEK_SET) != 0) return 0;
        r->file_pos = r->pos;
    }
    size_t got = fread(dst, 1, bytes, r->file);
    if (got < bytes && ferror(r->file)) { r->err = true; clearerr(r->file); }
    r->file_pos = got == bytes ? r->pos + (int64_t) got : -1;
    set_pos(r, r->pos + (int64_t) got);
    return got;
}

size_t ra_read(ra_reader_t * r, void * buffer, size_t bytes) {
    r->err = false;
    size_t got = r->windowed ? window_copy(r, buffer, bytes) : 0;
    if (got < bytes) got += direct_read(r, (uint8_t *) buffer + got, bytes - got);
    return got;
}

int ra_seek(ra_reader_t * r, int64_t offset, int whence) {
    int64_t base, target;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = r->pos; break;
        case SEEK_END: base = r->size; break;
        default: errno = EINVAL; return -1;
    }
    if (__builtin_add_overflow(base, offset, &target) || target < 0) { errno = EINVAL; return -1; }
    if (target == r->pos) return 0; /* an ftello: the FILE stays where it is */
    r->file_pos = -1;
    set_pos(r, target);
    return 0;
}

int64_t ra_tell(const ra_reader_t * r) { return r->pos; }

int ra_error(const ra_reader_t * r) { return r->err; }

/* --- stdio view --- */

/* Read: 0 only at the end of the file, -1 on a read error, so ferror and feof
 * on the FILE say what they would on a plain one. */
static ssize_t cookie_read(void * cookie, char * buffer, size_t bytes) {
    ra_reader_t * r = cookie;
    size_t got = ra_read(r, buffer, bytes);
    return got == 0 && bytes > 0 && ra_error(r) ? -1 : (ssize_t) got;
}

static int cookie_seek(void * cookie, off_t * offset, int whence) {
    ra_reader_t * r = cookie;
    if (ra_seek(r, *offset, whence) != 0) return -1;
    *offset = (off_t) ra_tell(r);
    return 0;
}

/* The reader outlives the FILE: the owner closes the reader after fclose. */
FILE * ra_as_file(ra_reader_t * r) {
    cookie_io_functions_t io = { .read = cookie_read, .seek = cookie_seek };
    return fopencookie(r, "rb", io);
}
