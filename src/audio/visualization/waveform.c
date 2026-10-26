#define _POSIX_C_SOURCE 200809L

#include "waveform.h"

#include "../engine/audio.h"
#include "../../core/persistence/storage_paths.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* main.c defines the real hook. A waveform_check link of this file alone
 * leaves the symbol unresolved-weak and skips the call. */
extern void install_thread_crash_altstack(void) __attribute__((weak));

#define WAVEFORM_PATH_MAX 2048
#define WAVEFORM_CACHE_DIR SD_COMPAS_ROOT "/waveforms"
#define WAVEFORM_CACHE_MAGIC_V1 "WFM1"
#define WAVEFORM_CACHE_MAGIC_V2 "WFM2"
#define WAVEFORM_CACHE_V1_HEADER_SIZE 26U
#define WAVEFORM_CACHE_V2_HEADER_SIZE 34U
#define WAVEFORM_CACHE_RECORD_MAX (WAVEFORM_CACHE_V2_HEADER_SIZE + WAVEFORM_PATH_MAX + WAVEFORM_BINS + 4U)
#define WAVEFORM_CACHE_MAX_GENERATION_NS UINT64_C(86400000000000)
#define WAVEFORM_FAILURE_CACHE_LIMIT 16U

typedef struct {
    off_t size;
    int64_t mtime_sec;
    uint32_t mtime_nsec;
} file_facts_t;

typedef struct {
    bool valid;
    char path[WAVEFORM_PATH_MAX];
    file_facts_t facts;
    uint64_t last_used;
} failed_waveform_t;

static pthread_mutex_t waveform_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waveform_cond = PTHREAD_COND_INITIALIZER;
static pthread_t waveform_thread;
static bool waveform_thread_started;
static bool waveform_stopping;
static bool waveform_pending;
static bool waveform_ready;
static uint64_t waveform_generation;
static char waveform_requested_path[WAVEFORM_PATH_MAX];
static bool waveform_prefetch_pending;
static uint64_t waveform_prefetch_revision;
static char waveform_prefetch_path[WAVEFORM_PATH_MAX];
static char waveform_result_path[WAVEFORM_PATH_MAX];
static uint8_t waveform_result_bins[WAVEFORM_BINS];
static failed_waveform_t waveform_failures[WAVEFORM_FAILURE_CACHE_LIMIT];
static uint64_t waveform_failure_clock;

static bool path_is_local(const char * path) {
    return path && path[0] && !strchr(path, ':') &&
           strnlen(path, WAVEFORM_PATH_MAX) < WAVEFORM_PATH_MAX;
}

static bool get_file_facts(const char * path, file_facts_t * facts) {
    struct stat st;
    if (!path || !facts || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
        return false;
    facts->size = st.st_size;
    facts->mtime_sec = (int64_t) st.st_mtim.tv_sec;
    facts->mtime_nsec = (uint32_t) st.st_mtim.tv_nsec;
    return true;
}

static bool facts_equal(const file_facts_t * a, const file_facts_t * b) {
    return a->size == b->size && a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec;
}

/* Remember failed identities only in RAM. Reopening the same unchanged file
 * after switching tracks must not repeat a long background scan, while a
 * changed size/mtime gets a fresh attempt. */
static bool failure_cache_contains(const char * path, const file_facts_t * facts) {
    bool found = false;
    pthread_mutex_lock(&waveform_mutex);
    for (size_t i = 0; i < WAVEFORM_FAILURE_CACHE_LIMIT; i++) {
        failed_waveform_t * entry = &waveform_failures[i];
        if (!entry->valid || strcmp(entry->path, path) != 0) continue;
        if (!facts_equal(&entry->facts, facts)) {
            entry->valid = false;
            entry->path[0] = '\0';
            continue;
        }
        entry->last_used = ++waveform_failure_clock;
        found = true;
    }
    pthread_mutex_unlock(&waveform_mutex);
    return found;
}

static void failure_cache_store_if_current(uint64_t generation, const char * path,
                                           const file_facts_t * facts) {
    pthread_mutex_lock(&waveform_mutex);
    if (waveform_stopping || waveform_generation != generation ||
        strcmp(waveform_requested_path, path) != 0) {
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    size_t slot = WAVEFORM_FAILURE_CACHE_LIMIT;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < WAVEFORM_FAILURE_CACHE_LIMIT; i++) {
        failed_waveform_t * entry = &waveform_failures[i];
        if (entry->valid && strcmp(entry->path, path) == 0) {
            slot = i;
            break;
        }
        if (!entry->valid) {
            slot = i;
            break;
        }
        if (entry->last_used < oldest) {
            oldest = entry->last_used;
            slot = i;
        }
    }
    if (slot < WAVEFORM_FAILURE_CACHE_LIMIT) {
        failed_waveform_t * entry = &waveform_failures[slot];
        snprintf(entry->path, sizeof(entry->path), "%s", path);
        entry->facts = *facts;
        entry->last_used = ++waveform_failure_clock;
        entry->valid = true;
    }
    pthread_mutex_unlock(&waveform_mutex);
}

static bool generation_cancelled(void * opaque) {
    const uint64_t * generation_ptr = (const uint64_t *) opaque;
    if (!generation_ptr) return true;
    uint64_t generation = *generation_ptr;
    pthread_mutex_lock(&waveform_mutex);
    bool cancelled = waveform_stopping || waveform_generation != generation;
    pthread_mutex_unlock(&waveform_mutex);
    return cancelled;
}

typedef struct {
    uint64_t generation;
    uint64_t revision;
    const char * path;
} waveform_prefetch_context_t;

static bool prefetch_cancelled(void * opaque) {
    const waveform_prefetch_context_t * context = opaque;
    if (!context) return true;
    pthread_mutex_lock(&waveform_mutex);
    bool cancelled = waveform_stopping ||
                     waveform_generation != context->generation ||
                     waveform_prefetch_revision != context->revision ||
                     waveform_requested_path[0] == '\0' ||
                     waveform_prefetch_path[0] == '\0' ||
                     strcmp(waveform_prefetch_path, context->path) != 0;
    pthread_mutex_unlock(&waveform_mutex);
    return cancelled;
}

static bool prefetch_is_current_locked(uint64_t generation, uint64_t revision,
                                       const char * path) {
    return !waveform_stopping && waveform_generation == generation &&
           waveform_prefetch_revision == revision && waveform_requested_path[0] != '\0' &&
           waveform_prefetch_path[0] != '\0' && strcmp(waveform_prefetch_path, path) == 0;
}

/* A foreground transition invalidates speculative work without changing its
 * generation token or touching the visible waveform result. Caller holds the
 * mutex. */
static void clear_prefetch_locked(void) {
    if (waveform_prefetch_path[0] || waveform_prefetch_pending) {
        waveform_prefetch_revision++;
        if (waveform_prefetch_revision == 0) waveform_prefetch_revision++;
    }
    waveform_prefetch_pending = false;
    waveform_prefetch_path[0] = '\0';
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t * data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t record_checksum(const uint8_t * data, size_t len) {
    return fnv1a_update(2166136261U, data, len);
}

static void put_u16(uint8_t * out, uint16_t value) {
    out[0] = (uint8_t) value;
    out[1] = (uint8_t) (value >> 8);
}

static void put_u32(uint8_t * out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) out[i] = (uint8_t) (value >> (8U * i));
}

static void put_u64(uint8_t * out, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t) (value >> (8U * i));
}

static uint16_t get_u16(const uint8_t * in) {
    return (uint16_t) in[0] | ((uint16_t) in[1] << 8);
}

static uint32_t get_u32(const uint8_t * in) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t) in[i] << (8U * i);
    return value;
}

static uint64_t get_u64(const uint8_t * in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t) in[i] << (8U * i);
    return value;
}

static uint64_t monotonic_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0)
        return 0;
    uint64_t seconds = (uint64_t) now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) now.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) now.tv_nsec;
}

static uint64_t elapsed_ns(uint64_t start_ns) {
    uint64_t end_ns = monotonic_now_ns();
    return start_ns != 0 && end_ns > start_ns ? end_ns - start_ns : 0;
}

typedef struct {
    uint64_t generation_ns;
    uint64_t lookup_ns;
    bool valid_record;
} cache_lookup_t;

/* Cache identity includes the full local path and its current file facts.
 * Hash collisions are harmless: records also verify all three values. */
static uint64_t cache_key(const char * path, const file_facts_t * facts) {
    uint32_t hash = 2166136261U;
    hash = fnv1a_update(hash, (const uint8_t *) path, strlen(path));
    uint8_t fields[20];
    put_u64(fields, (uint64_t) facts->size);
    put_u64(fields + 8, (uint64_t) facts->mtime_sec);
    put_u32(fields + 16, facts->mtime_nsec);
    hash = fnv1a_update(hash, fields, sizeof(fields));
    uint32_t second = fnv1a_update(0x9e3779b9U, (const uint8_t *) path, strlen(path));
    second = fnv1a_update(second, fields, sizeof(fields));
    return ((uint64_t) hash << 32) | second;
}

static bool ensure_cache_dir(void) {
    if (mkdir(SD_COMPAS_ROOT, 0755) != 0 && errno != EEXIST) return false;
    if (mkdir(WAVEFORM_CACHE_DIR, 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(WAVEFORM_CACHE_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool cache_path(uint64_t key, char * out, size_t out_size) {
    int n = snprintf(out, out_size, WAVEFORM_CACHE_DIR "/wf_%016llx.bin",
                     (unsigned long long) key);
    return n > 0 && (size_t) n < out_size;
}

/* Remove only the entry for this exact identity, and only while the request
 * that observed its lookup is still current. Filesystem work stays outside
 * the mutex used by the UI-facing request state. */
static void cache_remove_if_current(const char * path, const file_facts_t * facts,
                                    uint64_t generation, uint64_t revision,
                                    bool prefetch_job) {
    file_facts_t current;
    if (!get_file_facts(path, &current) || !facts_equal(facts, &current)) return;
    pthread_mutex_lock(&waveform_mutex);
    bool request_current = prefetch_job
        ? prefetch_is_current_locked(generation, revision, path)
        : !waveform_stopping && waveform_generation == generation &&
          strcmp(waveform_requested_path, path) == 0;
    pthread_mutex_unlock(&waveform_mutex);
    if (!request_current) return;

    char name[sizeof(WAVEFORM_CACHE_DIR) + 40U];
    if (cache_path(cache_key(path, facts), name, sizeof(name))) (void) unlink(name);
}

static bool cache_store_worthwhile(const cache_lookup_t * lookup, uint64_t generation_ns) {
    /* Unknown measurements retain the previous cache behavior. */
    return !lookup || lookup->lookup_ns == 0 || generation_ns == 0 ||
           lookup->lookup_ns <= generation_ns / 2U;
}

static bool cache_load(const char * path, const file_facts_t * facts, uint8_t * bins,
                       cache_lookup_t * lookup) {
    uint64_t started_ns = monotonic_now_ns();
    if (lookup) memset(lookup, 0, sizeof(*lookup));
    bool loaded = false;
    if (!ensure_cache_dir()) goto done;
    char name[sizeof(WAVEFORM_CACHE_DIR) + 40U];
    uint64_t key = cache_key(path, facts);
    if (!cache_path(key, name, sizeof(name))) goto done;

    FILE * file = fopen(name, "rb");
    if (!file) goto done;
    uint8_t record[WAVEFORM_CACHE_RECORD_MAX];
    size_t got = fread(record, 1, sizeof(record), file);
    bool extra = fgetc(file) != EOF;
    bool io_ok = !ferror(file);
    fclose(file);
    if (!io_ok || extra || got < WAVEFORM_CACHE_V1_HEADER_SIZE + WAVEFORM_BINS + 4U)
        goto done;

    size_t header_size;
    uint64_t generation_ns = 0;
    if (memcmp(record, WAVEFORM_CACHE_MAGIC_V1, 4) == 0)
        header_size = WAVEFORM_CACHE_V1_HEADER_SIZE;
    else if (memcmp(record, WAVEFORM_CACHE_MAGIC_V2, 4) == 0) {
        header_size = WAVEFORM_CACHE_V2_HEADER_SIZE;
        if (got < header_size + WAVEFORM_BINS + 4U) goto done;
        generation_ns = get_u64(record + 26);
        if (generation_ns > WAVEFORM_CACHE_MAX_GENERATION_NS) goto done;
    }
    else goto done;

    uint16_t path_len = get_u16(record + 4);
    uint64_t size = get_u64(record + 6);
    int64_t mtime_sec = (int64_t) get_u64(record + 14);
    uint32_t mtime_nsec = get_u32(record + 22);
    size_t expected = header_size + path_len + WAVEFORM_BINS + 4U;
    if (path_len == 0 || path_len >= WAVEFORM_PATH_MAX || expected != got ||
        size != (uint64_t) facts->size || mtime_sec != facts->mtime_sec ||
        mtime_nsec != facts->mtime_nsec || strlen(path) != path_len ||
        memcmp(record + header_size, path, path_len) != 0)
        goto done;

    uint32_t stored_checksum = get_u32(record + got - 4U);
    if (stored_checksum != record_checksum(record, got - 4U)) goto done;
    memcpy(bins, record + header_size + path_len, WAVEFORM_BINS);
    loaded = true;
    if (lookup) {
        lookup->generation_ns = generation_ns;
        lookup->valid_record = true;
    }
done:
    if (lookup) lookup->lookup_ns = elapsed_ns(started_ns);
    return loaded;
}

static void cache_store(const char * path, const file_facts_t * facts, const uint8_t * bins,
                        uint64_t generation_ns, uint64_t prefetch_generation,
                        uint64_t prefetch_revision) {
    if (!ensure_cache_dir()) return;
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= WAVEFORM_PATH_MAX || path_len > UINT16_MAX) return;
    size_t record_len = WAVEFORM_CACHE_V2_HEADER_SIZE + path_len + WAVEFORM_BINS + 4U;
    uint8_t record[WAVEFORM_CACHE_RECORD_MAX];
    memcpy(record, WAVEFORM_CACHE_MAGIC_V2, 4);
    put_u16(record + 4, (uint16_t) path_len);
    put_u64(record + 6, (uint64_t) facts->size);
    put_u64(record + 14, (uint64_t) facts->mtime_sec);
    put_u32(record + 22, facts->mtime_nsec);
    put_u64(record + 26, generation_ns);
    memcpy(record + WAVEFORM_CACHE_V2_HEADER_SIZE, path, path_len);
    memcpy(record + WAVEFORM_CACHE_V2_HEADER_SIZE + path_len, bins, WAVEFORM_BINS);
    put_u32(record + record_len - 4U, record_checksum(record, record_len - 4U));

    char destination[sizeof(WAVEFORM_CACHE_DIR) + 40U];
    if (!cache_path(cache_key(path, facts), destination, sizeof(destination))) return;
    char temporary[sizeof(destination) + 32U];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", destination, (long) getpid());
    if (n <= 0 || (size_t) n >= sizeof(temporary)) return;
    FILE * file = fopen(temporary, "wb");
    if (!file) return;
    bool ok = fwrite(record, 1, record_len, file) == record_len && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        bool renamed = false;
        if (prefetch_generation != 0) {
            file_facts_t current;
            bool identity_current = get_file_facts(path, &current) && facts_equal(facts, &current);
            pthread_mutex_lock(&waveform_mutex);
            bool requested = prefetch_is_current_locked(prefetch_generation,
                                                          prefetch_revision, path);
            pthread_mutex_unlock(&waveform_mutex);
            /* Do not hold the GUI-facing mutex across SD I/O. A transition
             * after this check may leave a complete, valid cache record, but
             * it can never publish speculative bins as the current track. */
            if (identity_current && requested && rename(temporary, destination) == 0)
                renamed = true;
        }
        else if (rename(temporary, destination) == 0) renamed = true;
        if (!renamed) unlink(temporary);
    }
    else unlink(temporary);
}

static void publish_result(uint64_t generation, const char * path, const uint8_t * bins) {
    pthread_mutex_lock(&waveform_mutex);
    if (!waveform_stopping && waveform_generation == generation &&
        strcmp(waveform_requested_path, path) == 0) {
        memcpy(waveform_result_bins, bins, WAVEFORM_BINS);
        snprintf(waveform_result_path, sizeof(waveform_result_path), "%s", path);
        waveform_ready = true;
    }
    pthread_mutex_unlock(&waveform_mutex);
}

/* Match the visible seekbar resolution while retaining the 256-byte on-disk
 * format. Each sampled bin spans four published bins. */
#define WAVEFORM_SAMPLE_BINS 64U
_Static_assert(WAVEFORM_BINS % WAVEFORM_SAMPLE_BINS == 0, "waveform sample expansion");

typedef struct {
    uint64_t generation;
    const char * path;
    file_facts_t facts;
} waveform_progress_context_t;

static void clear_result_if_current(uint64_t generation) {
    pthread_mutex_lock(&waveform_mutex);
    if (waveform_generation == generation) {
        waveform_ready = false;
        waveform_result_path[0] = '\0';
    }
    pthread_mutex_unlock(&waveform_mutex);
}

static void expand_samples(const uint8_t * samples, uint8_t * bins) {
    for (size_t i = 0; i < WAVEFORM_BINS; ++i)
        bins[i] = samples[i * WAVEFORM_SAMPLE_BINS / WAVEFORM_BINS];
}

static void publish_progress(const uint8_t * samples, size_t count,
                             size_t completed, void * user) {
    waveform_progress_context_t * context = user;
    if (count != WAVEFORM_SAMPLE_BINS || completed == 0 ||
        generation_cancelled(&context->generation)) return;
    file_facts_t current;
    if (!get_file_facts(context->path, &current) ||
        !facts_equal(&current, &context->facts)) {
        clear_result_if_current(context->generation);
        return;
    }
    uint8_t bins[WAVEFORM_BINS];
    expand_samples(samples, bins);
    publish_result(context->generation, context->path, bins);
}

static void set_worker_background_priority(void) {
    (void) setpriority(PRIO_PROCESS, 0, 10);
    /* Remain below playback/UI, but eligible to use spare CPU during normal
     * scheduling. SCHED_IDLE starves previews during unrelated background work. */
}

static void process_prefetch(const char * path, uint64_t generation, uint64_t revision) {
    waveform_prefetch_context_t context = {
        .generation = generation, .revision = revision, .path = path
    };
    if (prefetch_cancelled(&context)) return;
    file_facts_t facts;
    if (!get_file_facts(path, &facts)) return;
    if (failure_cache_contains(path, &facts)) return;

    uint8_t bins[WAVEFORM_BINS];
    cache_lookup_t lookup;
    if (cache_load(path, &facts, bins, &lookup)) {
        if (lookup.generation_ns != 0 && lookup.lookup_ns != 0 &&
            lookup.lookup_ns > lookup.generation_ns / 2U)
            cache_remove_if_current(path, &facts, generation, revision, true);
        return;
    }
    if (prefetch_cancelled(&context)) return;

    uint8_t samples[WAVEFORM_SAMPLE_BINS];
    uint64_t generation_started_ns = monotonic_now_ns();
    if (!audio_extract_waveform_progress(path, samples, WAVEFORM_SAMPLE_BINS,
                                         prefetch_cancelled, &context, NULL, NULL))
        return;
    uint64_t generation_ns = elapsed_ns(generation_started_ns);
    if (prefetch_cancelled(&context)) return;
    file_facts_t after;
    if (!get_file_facts(path, &after) || !facts_equal(&facts, &after) ||
        prefetch_cancelled(&context)) return;
    expand_samples(samples, bins);
    /* Prefetch never publishes a result. The cache rename is fenced by both
     * request tokens so a superseded scan cannot leave a stale cache entry. */
    if (cache_store_worthwhile(&lookup, generation_ns))
        cache_store(path, &facts, bins, generation_ns, generation, revision);
}

static void * waveform_worker(void * unused) {
    (void) unused;
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "waveform");
#endif
    if (install_thread_crash_altstack) install_thread_crash_altstack(); /* see its own comment (main.c) */
    set_worker_background_priority();
    for (;;) {
        char path[WAVEFORM_PATH_MAX];
        uint64_t generation;
        uint64_t prefetch_revision = 0;
        bool prefetch_job = false;
        pthread_mutex_lock(&waveform_mutex);
        while (!waveform_stopping && !waveform_pending && !waveform_prefetch_pending)
            pthread_cond_wait(&waveform_cond, &waveform_mutex);
        if (waveform_stopping) {
            pthread_mutex_unlock(&waveform_mutex);
            return NULL;
        }
        if (waveform_pending) {
            snprintf(path, sizeof(path), "%s", waveform_requested_path);
            generation = waveform_generation;
            waveform_pending = false;
        }
        else {
            snprintf(path, sizeof(path), "%s", waveform_prefetch_path);
            generation = waveform_generation;
            prefetch_revision = waveform_prefetch_revision;
            waveform_prefetch_pending = false;
            prefetch_job = true;
        }
        pthread_mutex_unlock(&waveform_mutex);

        if (prefetch_job) {
            process_prefetch(path, generation, prefetch_revision);
            continue;
        }

        if (generation_cancelled(&generation)) continue;
        file_facts_t facts;
        if (!get_file_facts(path, &facts)) continue;
        bool previously_failed = failure_cache_contains(path, &facts);

        uint8_t bins[WAVEFORM_BINS];
        cache_lookup_t lookup;
        if (cache_load(path, &facts, bins, &lookup)) {
            file_facts_t confirmed;
            if (!generation_cancelled(&generation) && get_file_facts(path, &confirmed) &&
                facts_equal(&facts, &confirmed) && !generation_cancelled(&generation)) {
                /* Serve the validated hit before any cache-removal I/O. */
                publish_result(generation, path, bins);
                if (lookup.generation_ns != 0 && lookup.lookup_ns != 0 &&
                    lookup.lookup_ns > lookup.generation_ns / 2U)
                    cache_remove_if_current(path, &facts, generation, 0, false);
            }
            continue;
        }
        if (previously_failed) continue;
        if (generation_cancelled(&generation)) continue;
        uint8_t samples[WAVEFORM_SAMPLE_BINS];
        waveform_progress_context_t progress = {
            .generation = generation, .path = path, .facts = facts
        };
        uint64_t generation_started_ns = monotonic_now_ns();
        if (!audio_extract_waveform_progress(path, samples, WAVEFORM_SAMPLE_BINS,
                                            generation_cancelled, &generation,
                                            publish_progress, &progress)) {
            clear_result_if_current(generation);
            file_facts_t after_failure;
            if (get_file_facts(path, &after_failure) && facts_equal(&facts, &after_failure))
                failure_cache_store_if_current(generation, path, &facts);
            continue;
        }
        uint64_t generation_ns = elapsed_ns(generation_started_ns);
        if (generation_cancelled(&generation)) continue;
        file_facts_t after;
        if (!get_file_facts(path, &after) || !facts_equal(&facts, &after)) {
            clear_result_if_current(generation);
            continue;
        }
        expand_samples(samples, bins);
        /* Display the complete result before the SD cache write. Only a
         * successful, identity-checked extraction is cached. */
        publish_result(generation, path, bins);
        if (!generation_cancelled(&generation) && cache_store_worthwhile(&lookup, generation_ns))
            cache_store(path, &facts, bins, generation_ns, 0, 0);
        file_facts_t confirmed;
        if (!get_file_facts(path, &confirmed) || !facts_equal(&facts, &confirmed))
            clear_result_if_current(generation);
    }
}

/* faad2 HE-AAC v2 decode needs ~70 KiB on top of the decoder frames. */
#define WAVEFORM_WORKER_STACK_SIZE (256 * 1024)

static bool start_worker_locked(void) {
    if (waveform_thread_started) return true;
    if (waveform_stopping) return false;
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized &&
        pthread_attr_setstacksize(&attr, WAVEFORM_WORKER_STACK_SIZE) == 0)
        attr_ptr = &attr;
    bool created = pthread_create(&waveform_thread, attr_ptr, waveform_worker, NULL) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (!created) return false;
    waveform_thread_started = true;
    return true;
}

void waveform_request(const char * path) {
    if (!path) {
        waveform_cancel();
        return;
    }
    pthread_mutex_lock(&waveform_mutex);
    if (!path_is_local(path)) {
        clear_prefetch_locked();
        if (waveform_requested_path[0] || waveform_pending || waveform_ready) {
            waveform_generation++;
            if (waveform_generation == 0) waveform_generation++;
            waveform_pending = false;
            waveform_ready = false;
            waveform_requested_path[0] = '\0';
            waveform_result_path[0] = '\0';
            pthread_cond_signal(&waveform_cond);
        }
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    if (waveform_stopping || strcmp(waveform_requested_path, path) == 0) {
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    clear_prefetch_locked();
    waveform_generation++;
    if (waveform_generation == 0) waveform_generation++;
    snprintf(waveform_requested_path, sizeof(waveform_requested_path), "%s", path);
    waveform_result_path[0] = '\0';
    waveform_ready = false;
    waveform_pending = true;
    if (!start_worker_locked()) {
        waveform_pending = false;
        waveform_requested_path[0] = '\0';
    }
    else pthread_cond_signal(&waveform_cond);
    pthread_mutex_unlock(&waveform_mutex);
}

void waveform_prefetch(const char * path) {
    pthread_mutex_lock(&waveform_mutex);
    if (!path) {
        clear_prefetch_locked();
        pthread_cond_signal(&waveform_cond);
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    if (waveform_stopping) {
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    if (!waveform_requested_path[0] || !path_is_local(path) ||
        strcmp(waveform_requested_path, path) == 0) {
        clear_prefetch_locked();
        pthread_cond_signal(&waveform_cond);
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    if (strcmp(waveform_prefetch_path, path) == 0) {
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    waveform_prefetch_revision++;
    if (waveform_prefetch_revision == 0) waveform_prefetch_revision++;
    snprintf(waveform_prefetch_path, sizeof(waveform_prefetch_path), "%s", path);
    waveform_prefetch_pending = true;
    if (!start_worker_locked()) waveform_prefetch_pending = false;
    else pthread_cond_signal(&waveform_cond);
    pthread_mutex_unlock(&waveform_mutex);
}

bool waveform_copy(const char * path, waveform_data_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!path || !path_is_local(path)) return false;
    pthread_mutex_lock(&waveform_mutex);
    bool ready = waveform_ready && strcmp(waveform_result_path, path) == 0;
    if (ready) {
        memcpy(out->bins, waveform_result_bins, WAVEFORM_BINS);
        out->ready = true;
    }
    pthread_mutex_unlock(&waveform_mutex);
    return ready;
}

void waveform_cancel(void) {
    pthread_mutex_lock(&waveform_mutex);
    clear_prefetch_locked();
    if (waveform_requested_path[0] || waveform_pending || waveform_ready) {
        waveform_generation++;
        if (waveform_generation == 0) waveform_generation++;
        waveform_pending = false;
        waveform_ready = false;
        waveform_requested_path[0] = '\0';
        waveform_result_path[0] = '\0';
        pthread_cond_signal(&waveform_cond);
    }
    pthread_mutex_unlock(&waveform_mutex);
}

void waveform_shutdown(void) {
    pthread_mutex_lock(&waveform_mutex);
    if (!waveform_thread_started) {
        waveform_stopping = true;
        clear_prefetch_locked();
        waveform_pending = false;
        waveform_ready = false;
        waveform_requested_path[0] = '\0';
        waveform_result_path[0] = '\0';
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    waveform_stopping = true;
    clear_prefetch_locked();
    waveform_generation++;
    waveform_pending = false;
    waveform_ready = false;
    waveform_requested_path[0] = '\0';
    waveform_result_path[0] = '\0';
    pthread_cond_broadcast(&waveform_cond);
    pthread_t thread = waveform_thread;
    pthread_mutex_unlock(&waveform_mutex);

    pthread_join(thread, NULL);
    pthread_mutex_lock(&waveform_mutex);
    waveform_thread_started = false;
    pthread_mutex_unlock(&waveform_mutex);
}
