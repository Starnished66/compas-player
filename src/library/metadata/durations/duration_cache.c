#include "duration_cache.h"
#include "duration_cache_internal.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../core/process/worker_thread.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* One worker thread does everything slow: loading the file, folding recent
 * puts into the sorted table, and writing it back. Callers only ever take the
 * lookup mutex for a bounded lookup or an array append.
 *
 *   pending  : puts not yet seen by the worker (searched first)
 *   inflight : the batch the worker is merging right now (searched second)
 *   table    : the immutable sorted table (searched last; swapped whole)
 *
 * A "storage epoch" counts card changes (removal, insertion, library reopen).
 * Every load, merge and write job remembers the epoch it started in and is
 * discarded if it no longer matches, and so is every put: callers take the
 * epoch before they read the cache and hand it back with the result, so a
 * record measured on a removed card never reaches the replacement. The
 * directory is opened when the file is loaded and writes go through that
 * handle, so a write can only land on the filesystem it was loaded from. */
#define DURATION_MERGE_AT 96u
#define DURATION_WORKER_STACK (128u * 1024u)

pthread_mutex_t dc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER; /* one writer at a time */
pthread_mutex_t dc_merge_lock = PTHREAD_MUTEX_INITIALIZER; /* merge and prune swap the table */
pthread_cond_t dc_flushed_cond;
static pthread_cond_t loaded_cond, work_cond;
static pthread_once_t cond_once = PTHREAD_ONCE_INIT;
load_state_t dc_state = STATE_UNLOADED;
dc_table_t * dc_cur;
dc_entry_t dc_pending[DURATION_PENDING_MAX];
dc_entry_t dc_inflight[DURATION_PENDING_MAX];
size_t dc_pending_count, dc_inflight_count;
static bool dirty, worker_running, load_requested, merge_requested, write_failed, flush_now;
static bool read_only; /* this epoch's file could not be read: never overwrite it */
static uint32_t dirty_generation;
uint32_t dc_epoch;
static int64_t last_put_ms, first_dirty_ms;
static int64_t no_card_until_ms;
int64_t dc_merge_backoff_ms;
static dc_store_t store = { .dirfd = -1 };
static bool (*mount_check)(void);
static uint32_t debounce_ms = 5000, max_age_ms = 60000;
static char path_override[PATH_MAX];

static void init_conds(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&loaded_cond, &attr);
    pthread_cond_init(&work_cond, &attr);
    pthread_cond_init(&dc_flushed_cond, &attr);
    pthread_condattr_destroy(&attr);
}

int64_t dc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wait_until(pthread_cond_t * cond, int64_t deadline_ms) {
    struct timespec ts = { (time_t) (deadline_ms / 1000), (long) (deadline_ms % 1000) * 1000000L };
    pthread_cond_timedwait(cond, &dc_lock, &ts);
}

/* Caller holds `lock` for these. */
void dc_table_ref(dc_table_t * t) { if (t) t->refs++; }
void dc_table_unref(dc_table_t * t) { if (t && --t->refs == 0) free(t); }

static const char * file_path(void) {
    return path_override[0] ? path_override : storage_sd_path(COMPAS_STORAGE_DIR "/durations.bin");
}

uint64_t duration_cache_key(const char * path, int64_t mtime, int64_t size) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char * p = (const unsigned char *) (path ? path : ""); *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    uint32_t parts[2] = { (uint32_t) (int32_t) mtime, (uint32_t) (int32_t) size };
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < 4; b++) h = (h ^ ((parts[i] >> (8 * b)) & 0xffu)) * 1099511628211ull;
    h ^= h >> 32;
    return h ? h : 1;
}

const dc_entry_t * dc_find_run(const dc_entry_t * run, size_t n, uint64_t key) {
    for (size_t i = 0; i < n; i++) if (run[i].key == key) return &run[i];
    return NULL;
}

static const dc_entry_t * find_entry(uint64_t key) {
    const dc_entry_t * e = dc_find_run(dc_pending, dc_pending_count, key);
    if (!e) e = dc_find_run(dc_inflight, dc_inflight_count, key);
    if (!e && dc_cur) e = bsearch(&(dc_entry_t) { .key = key }, dc_cur->e, dc_cur->count, sizeof(dc_entry_t), dc_cmp_entry);
    return e;
}

/* ---- Worker -------------------------------------------------------------- */
static bool epoch_is_current(void * ctx) {
    uint32_t owner = *(uint32_t *) ctx;
    pthread_mutex_lock(&dc_lock);
    bool current = owner == dc_epoch;
    pthread_mutex_unlock(&dc_lock);
    return current;
}

static void do_load(uint32_t owner, const char * path) {
    bool mounted = !mount_check || mount_check();
    dc_store_t loaded_store = { .dirfd = -1 };
    dc_table_t * loaded = NULL;
    dc_load_status_t status = DC_LOAD_EMPTY;
    if (mounted) status = dc_file_load(path, &loaded_store, &loaded);
    pthread_mutex_lock(&dc_lock);
    if (owner != dc_epoch) { /* the card changed meanwhile: its reset already restarted the state */
        pthread_mutex_unlock(&dc_lock);
        free(loaded);
        dc_store_close(&loaded_store);
        return;
    }
    if (!mounted) { /* an empty table here could later be written over a real file: stay unloaded */
        dc_state = STATE_UNLOADED;
        no_card_until_ms = dc_now_ms() + 1000;
        pthread_cond_broadcast(&loaded_cond);
        pthread_mutex_unlock(&dc_lock);
        return;
    }
    dc_table_unref(dc_cur);
    dc_cur = loaded;
    dc_store_close(&store);
    store = loaded_store;
    read_only = status == DC_LOAD_FAILED;
    if (read_only) dirty = write_failed = false;
    dc_state = STATE_LOADED;
    if (dc_pending_count > 0) merge_requested = true;
    pthread_cond_broadcast(&loaded_cond);
    pthread_cond_signal(&work_cond);
    pthread_mutex_unlock(&dc_lock);
}

/* Writes the table plus everything not yet merged, from an owned snapshot, so
 * no lock is held while packing, checksumming or waiting on the card. */
static bool write_snapshot(void) {
    pthread_mutex_lock(&write_lock);
    pthread_mutex_lock(&dc_lock);
    if (dc_state != STATE_LOADED) { pthread_mutex_unlock(&dc_lock); pthread_mutex_unlock(&write_lock); return false; }
    uint32_t owner = dc_epoch, generation = dirty_generation;
    dc_table_t * base = dc_cur;
    dc_table_ref(base);
    dc_entry_t * extra = malloc((dc_inflight_count + dc_pending_count + 1) * sizeof(dc_entry_t));
    size_t extra_count = 0;
    if (extra) {
        memcpy(extra, dc_inflight, dc_inflight_count * sizeof(dc_entry_t));
        extra_count = dc_inflight_count;
        for (size_t i = 0; i < dc_pending_count; i++) { /* pending is newer than inflight */
            size_t k = 0;
            while (k < extra_count && extra[k].key != dc_pending[i].key) k++;
            extra[k] = dc_pending[i];
            if (k == extra_count) extra_count++;
        }
    }
    int dirfd = store.dirfd >= 0 ? dup(store.dirfd) : -1; /* the epoch's own handle may be closed meanwhile */
    char name[sizeof(store.name)];
    snprintf(name, sizeof(name), "%s", store.name);
    pthread_mutex_unlock(&dc_lock);

    bool ok = false;
    if (extra && dirfd >= 0) {
        qsort(extra, extra_count, sizeof(dc_entry_t), dc_cmp_entry);
        ok = dc_file_write(dirfd, name, base, extra, extra_count, epoch_is_current, &owner);
    }
    if (dirfd >= 0) close(dirfd);
    free(extra);

    pthread_mutex_lock(&dc_lock);
    if (owner == dc_epoch) {
        if (ok && generation == dirty_generation) { dirty = false; first_dirty_ms = 0; }
        else if (ok) first_dirty_ms = dc_now_ms(); /* puts arrived during the write: age restarts */
        else write_failed = true; /* a full or read-only card waits for the next put */
    }
    dc_table_unref(base);
    pthread_cond_broadcast(&dc_flushed_cond);
    pthread_mutex_unlock(&dc_lock);
    pthread_mutex_unlock(&write_lock);
    return ok;
}

static void * cache_worker(void * arg) {
    (void) arg;
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "durcache");
#endif
    /* Loading is what a page waits for (the artist aggregate blocks on it); the merge
     * and the write-back below are opportunistic and drop to idle; a later load raises it again. */
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    pthread_mutex_lock(&dc_lock);
    for (;;) {
        if (load_requested) {
            load_requested = false;
            uint32_t owner = dc_epoch;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s", file_path());
            pthread_mutex_unlock(&dc_lock);
            worker_raise_priority(WORKER_NICE_BACKGROUND); /* a page waits; an earlier merge or write dropped us */
            do_load(owner, path);
            pthread_mutex_lock(&dc_lock);
            continue;
        }
        if (dc_state == STATE_LOADED && (merge_requested || dc_pending_count >= DURATION_MERGE_AT) &&
            dc_now_ms() >= dc_merge_backoff_ms) {
            merge_requested = false;
            pthread_mutex_unlock(&dc_lock);
            worker_lower_priority(WORKER_NICE_IDLE);
            (void) dc_do_merge(); /* on failure it set the back-off: no retry until it passes and a put arrives */
            pthread_mutex_lock(&dc_lock);
            continue;
        }
        if (dirty && dc_state == STATE_LOADED && !write_failed) {
            int64_t now = dc_now_ms();
            int64_t due = last_put_ms + debounce_ms, limit = first_dirty_ms + max_age_ms;
            if (limit < due) due = limit;
            if (flush_now || now >= due) {
                flush_now = false;
                pthread_mutex_unlock(&dc_lock);
                worker_lower_priority(WORKER_NICE_IDLE);
                (void) write_snapshot();
                pthread_mutex_lock(&dc_lock);
                continue;
            }
            wait_until(&work_cond, due);
            continue;
        }
        if (dirty && dc_state == STATE_LOADING) { /* a load is queued behind us or running: wait for it */
            pthread_cond_wait(&work_cond, &dc_lock);
            continue;
        }
        flush_now = false;
        pthread_cond_broadcast(&dc_flushed_cond);
        worker_running = false; /* idle: the next put or load starts a new one */
        pthread_mutex_unlock(&dc_lock);
        return NULL;
    }
}

/* Caller holds `lock`. */
static bool ensure_worker_locked(void) {
    if (worker_running) { pthread_cond_signal(&work_cond); return true; }
    pthread_t thread;
    if (worker_thread_create(&thread, DURATION_WORKER_STACK, cache_worker, NULL) != 0) return false;
    pthread_detach(thread);
    worker_running = true;
    return true;
}

static void start_load_locked(void) {
    if (dc_state != STATE_UNLOADED || dc_now_ms() < no_card_until_ms) return;
    dc_state = STATE_LOADING;
    load_requested = true;
    if (!ensure_worker_locked()) { dc_state = STATE_UNLOADED; load_requested = false; } /* retried on the next call */
}

void dc_note_dirty_locked(void) {
    if (!read_only) { /* an unreadable file is left alone; the records still serve this session */
        int64_t now = dc_now_ms();
        dirty = true;
        dirty_generation++;
        last_put_ms = now;
        if (!first_dirty_ms) first_dirty_ms = now;
        write_failed = false;
    }
    (void) ensure_worker_locked();
}

/* ---- Public API ---------------------------------------------------------- */
static bool lookup_locked(uint64_t key, duration_info_t * out) {
    const dc_entry_t * e = find_entry(key);
    if (!e) return false;
    if ((e->flags & DURATION_CACHE_FLAG_FAILED) && e->bit_depth != DURATION_PROBE_REVISION) return false;
    if (out) *out = (duration_info_t) { e->duration_ms, e->sample_rate, e->bit_depth, e->codec, e->flags };
    return true;
}

bool duration_cache_get_key(uint64_t key, duration_info_t * out) {
    pthread_once(&cond_once, init_conds);
    pthread_mutex_lock(&dc_lock);
    if (dc_state != STATE_LOADED) { start_load_locked(); pthread_mutex_unlock(&dc_lock); return false; }
    bool found = lookup_locked(key, out);
    pthread_mutex_unlock(&dc_lock);
    return found;
}

bool duration_cache_get(const char * path, int64_t mtime, int64_t size, duration_info_t * out) {
    return duration_cache_get_key(duration_cache_key(path, mtime, size), out);
}

bool duration_cache_wait_loaded(void) {
    pthread_once(&cond_once, init_conds);
    pthread_mutex_lock(&dc_lock);
    while (dc_state != STATE_LOADED) {
        if (dc_state == STATE_UNLOADED) {
            if (dc_now_ms() < no_card_until_ms) { pthread_mutex_unlock(&dc_lock); return false; } /* no card mounted */
            start_load_locked();
        }
        if (dc_state == STATE_UNLOADED) { /* no thread available: load inline */
            dc_state = STATE_LOADING;
            uint32_t owner = dc_epoch;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s", file_path());
            pthread_mutex_unlock(&dc_lock);
            do_load(owner, path);
            pthread_mutex_lock(&dc_lock);
            continue;
        }
        pthread_cond_wait(&loaded_cond, &dc_lock);
    }
    pthread_mutex_unlock(&dc_lock);
    return true;
}

bool duration_cache_get_blocking(uint64_t key, duration_info_t * out) {
    return duration_cache_wait_loaded() && duration_cache_get_key(key, out);
}

void duration_cache_wait_merged(void) {
    if (!duration_cache_wait_loaded()) return;
    pthread_mutex_lock(&dc_lock);
    while (dc_pending_count > 0 || dc_inflight_count > 0) {
        merge_requested = true;
        if (!ensure_worker_locked()) { pthread_mutex_unlock(&dc_lock); (void) dc_do_merge(); pthread_mutex_lock(&dc_lock); continue; }
        wait_until(&dc_flushed_cond, dc_now_ms() + 50);
    }
    pthread_mutex_unlock(&dc_lock);
}

uint32_t duration_cache_epoch(void) {
    pthread_mutex_lock(&dc_lock);
    uint32_t e = dc_epoch;
    pthread_mutex_unlock(&dc_lock);
    return e;
}

void duration_cache_put(uint32_t at_epoch, const char * path, int64_t mtime, int64_t size, const duration_info_t * info) {
    bool failed = info && (info->flags & DURATION_CACHE_FLAG_FAILED);
    if (!path || !info || (info->duration_ms == 0 && !failed) || (failed && info->duration_ms != 0)) return;
    uint64_t key = duration_cache_key(path, mtime, size);
    dc_entry_t value = { key, info->duration_ms, info->sample_rate, info->bit_depth, info->codec, info->flags };
    pthread_once(&cond_once, init_conds);
    pthread_mutex_lock(&dc_lock);
    if (at_epoch != dc_epoch) { pthread_mutex_unlock(&dc_lock); return; } /* measured on a card that is gone */
    if (dc_state == STATE_UNLOADED && dc_now_ms() < no_card_until_ms) { pthread_mutex_unlock(&dc_lock); return; } /* no card to keep it for */
    start_load_locked();
    dc_entry_t * queued = (dc_entry_t *) dc_find_run(dc_pending, dc_pending_count, key);
    if (queued) {
        if (memcmp(queued, &value, sizeof(value)) != 0) { *queued = value; dc_note_dirty_locked(); }
    } else {
        const dc_entry_t * existing = find_entry(key);
        if (existing && memcmp(existing, &value, sizeof(value)) == 0) {
            /* unchanged */
        } else if (existing || (dc_cur ? dc_cur->count : 0) + dc_inflight_count + dc_pending_count < DURATION_CACHE_MAX_ENTRIES) {
            /* A bounded enqueue: the worker merges. A full queue (the worker is
             * still loading or merging) drops the put; the track is probed again. */
            if (dc_pending_count < DURATION_PENDING_MAX) {
                dc_pending[dc_pending_count++] = value;
                dc_note_dirty_locked();
                if (dc_state == STATE_LOADED && dc_pending_count >= DURATION_MERGE_AT) pthread_cond_signal(&work_cond);
            }
        }
    }
    pthread_mutex_unlock(&dc_lock);
}

void duration_cache_put_failed(uint32_t at_epoch, const char * path, int64_t mtime, int64_t size) {
    duration_info_t info = { .bit_depth = DURATION_PROBE_REVISION, .flags = DURATION_CACHE_FLAG_FAILED };
    duration_cache_put(at_epoch, path, mtime, size, &info);
}

bool duration_info_is_failed(const duration_info_t * info) {
    return info && (info->flags & DURATION_CACHE_FLAG_FAILED);
}

size_t duration_cache_count(void) {
    pthread_mutex_lock(&dc_lock);
    size_t n = (dc_cur ? dc_cur->count : 0) + dc_inflight_count + dc_pending_count;
    pthread_mutex_unlock(&dc_lock);
    return n;
}

bool duration_cache_flush_bounded(uint32_t timeout_ms) {
    pthread_once(&cond_once, init_conds);
    int64_t deadline = dc_now_ms() + timeout_ms;
    pthread_mutex_lock(&dc_lock);
    bool ok = true;
    while (dirty) {
        if (write_failed || dc_now_ms() >= deadline) { ok = false; break; }
        flush_now = true;
        if (!ensure_worker_locked()) { ok = false; break; }
        wait_until(&dc_flushed_cond, deadline);
    }
    pthread_mutex_unlock(&dc_lock);
    return ok;
}

bool duration_cache_flush(void) { return duration_cache_flush_bounded(60000); }

void duration_cache_new_storage_epoch(void) {
    pthread_once(&cond_once, init_conds);
    pthread_mutex_lock(&dc_lock);
    dc_epoch++;
    dc_table_unref(dc_cur);
    dc_cur = NULL;
    dc_pending_count = dc_inflight_count = 0;
    dirty = write_failed = flush_now = merge_requested = load_requested = false;
    first_dirty_ms = 0;
    read_only = false;
    no_card_until_ms = dc_merge_backoff_ms = 0;
    dc_store_close(&store);
    dc_state = STATE_UNLOADED; /* the next lookup loads the new card's file */
    pthread_cond_broadcast(&loaded_cond);
    pthread_cond_broadcast(&dc_flushed_cond);
    pthread_mutex_unlock(&dc_lock);
}

void duration_cache_reset(void) { duration_cache_new_storage_epoch(); }

void duration_cache_set_path(const char * path) {
    pthread_mutex_lock(&dc_lock);
    snprintf(path_override, sizeof(path_override), "%s", path ? path : "");
    pthread_mutex_unlock(&dc_lock);
}

void duration_cache_set_mount_check(bool (*is_mounted)(void)) {
    pthread_mutex_lock(&dc_lock);
    mount_check = is_mounted;
    pthread_mutex_unlock(&dc_lock);
}

bool duration_cache_read_only(void) {
    pthread_mutex_lock(&dc_lock);
    bool r = read_only;
    pthread_mutex_unlock(&dc_lock);
    return r;
}

void duration_cache_set_timing(uint32_t debounce, uint32_t max_age) {
    pthread_mutex_lock(&dc_lock);
    debounce_ms = debounce;
    max_age_ms = max_age;
    pthread_cond_signal(&work_cond);
    pthread_mutex_unlock(&dc_lock);
}
