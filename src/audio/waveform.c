#define _POSIX_C_SOURCE 200809L

#include "waveform.h"

#include "audio.h"
#include "../core/storage_paths.h"

#include <dirent.h>
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

#define WAVEFORM_PATH_MAX 2048
#define WAVEFORM_CACHE_DIR SD_COMPAS_ROOT "/waveforms"
#define WAVEFORM_CACHE_LIMIT 64
#define WAVEFORM_CACHE_MAGIC "WFM1"
#define WAVEFORM_CACHE_HEADER_SIZE 26U
#define WAVEFORM_CACHE_RECORD_MAX (WAVEFORM_CACHE_HEADER_SIZE + WAVEFORM_PATH_MAX + WAVEFORM_BINS + 4U)

typedef struct {
    off_t size;
    int64_t mtime_sec;
    uint32_t mtime_nsec;
} file_facts_t;

static pthread_mutex_t waveform_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waveform_cond = PTHREAD_COND_INITIALIZER;
static pthread_t waveform_thread;
static bool waveform_thread_started;
static bool waveform_stopping;
static bool waveform_pending;
static bool waveform_ready;
static uint64_t waveform_generation;
static char waveform_requested_path[WAVEFORM_PATH_MAX];
static char waveform_result_path[WAVEFORM_PATH_MAX];
static uint8_t waveform_result_bins[WAVEFORM_BINS];

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

static bool generation_cancelled(void * opaque) {
    const uint64_t * generation_ptr = (const uint64_t *) opaque;
    if (!generation_ptr) return true;
    uint64_t generation = *generation_ptr;
    pthread_mutex_lock(&waveform_mutex);
    bool cancelled = waveform_stopping || waveform_generation != generation;
    pthread_mutex_unlock(&waveform_mutex);
    return cancelled;
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

static bool cache_load(const char * path, const file_facts_t * facts, uint8_t * bins) {
    if (!ensure_cache_dir()) return false;
    char name[sizeof(WAVEFORM_CACHE_DIR) + 40U];
    uint64_t key = cache_key(path, facts);
    if (!cache_path(key, name, sizeof(name))) return false;

    FILE * file = fopen(name, "rb");
    if (!file) return false;
    uint8_t record[WAVEFORM_CACHE_RECORD_MAX];
    size_t got = fread(record, 1, sizeof(record), file);
    bool extra = fgetc(file) != EOF;
    bool io_ok = !ferror(file);
    fclose(file);
    if (!io_ok || extra || got < WAVEFORM_CACHE_HEADER_SIZE + WAVEFORM_BINS + 4U ||
        memcmp(record, WAVEFORM_CACHE_MAGIC, 4) != 0) return false;

    uint16_t path_len = get_u16(record + 4);
    uint64_t size = get_u64(record + 6);
    int64_t mtime_sec = (int64_t) get_u64(record + 14);
    uint32_t mtime_nsec = get_u32(record + 22);
    size_t expected = WAVEFORM_CACHE_HEADER_SIZE + path_len + WAVEFORM_BINS + 4U;
    if (path_len == 0 || path_len >= WAVEFORM_PATH_MAX || expected != got ||
        size != (uint64_t) facts->size || mtime_sec != facts->mtime_sec ||
        mtime_nsec != facts->mtime_nsec || strlen(path) != path_len ||
        memcmp(record + WAVEFORM_CACHE_HEADER_SIZE, path, path_len) != 0)
        return false;

    uint32_t stored_checksum = get_u32(record + got - 4U);
    if (stored_checksum != record_checksum(record, got - 4U)) return false;
    memcpy(bins, record + WAVEFORM_CACHE_HEADER_SIZE + path_len, WAVEFORM_BINS);
    return true;
}

static void cache_evict_oldest_until_room(const char * keep_path) {
    for (;;) {
        DIR * dir = opendir(WAVEFORM_CACHE_DIR);
        if (!dir) return;
        size_t count = 0;
        char oldest[sizeof(WAVEFORM_CACHE_DIR) + 40U] = {0};
        time_t oldest_time = 0;
        struct dirent * entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "wf_", 3) != 0) continue;
            size_t len = strlen(entry->d_name);
            if (len < 7 || strcmp(entry->d_name + len - 4, ".bin") != 0) continue;
            char full[sizeof(WAVEFORM_CACHE_DIR) + 40U];
            int n = snprintf(full, sizeof(full), WAVEFORM_CACHE_DIR "/%s", entry->d_name);
            if (n <= 0 || (size_t) n >= sizeof(full)) continue;
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            count++;
            if (keep_path && strcmp(full, keep_path) == 0) continue;
            if (!oldest[0] || st.st_mtime < oldest_time) {
                oldest_time = st.st_mtime;
                snprintf(oldest, sizeof(oldest), "%s", full);
            }
        }
        closedir(dir);
        if (count <= WAVEFORM_CACHE_LIMIT || !oldest[0]) return;
        if (unlink(oldest) != 0) return;
    }
}

static void cache_store(const char * path, const file_facts_t * facts, const uint8_t * bins) {
    if (!ensure_cache_dir()) return;
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= WAVEFORM_PATH_MAX || path_len > UINT16_MAX) return;
    size_t record_len = WAVEFORM_CACHE_HEADER_SIZE + path_len + WAVEFORM_BINS + 4U;
    uint8_t record[WAVEFORM_CACHE_RECORD_MAX];
    memcpy(record, WAVEFORM_CACHE_MAGIC, 4);
    put_u16(record + 4, (uint16_t) path_len);
    put_u64(record + 6, (uint64_t) facts->size);
    put_u64(record + 14, (uint64_t) facts->mtime_sec);
    put_u32(record + 22, facts->mtime_nsec);
    memcpy(record + WAVEFORM_CACHE_HEADER_SIZE, path, path_len);
    memcpy(record + WAVEFORM_CACHE_HEADER_SIZE + path_len, bins, WAVEFORM_BINS);
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
        if (rename(temporary, destination) != 0) unlink(temporary);
        else cache_evict_oldest_until_room(destination);
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

static void set_worker_background_priority(void) {
    (void) setpriority(PRIO_PROCESS, 0, 10);
#ifdef SCHED_IDLE
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    (void) pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif
}

static void * waveform_worker(void * unused) {
    (void) unused;
    set_worker_background_priority();
    for (;;) {
        char path[WAVEFORM_PATH_MAX];
        uint64_t generation;
        pthread_mutex_lock(&waveform_mutex);
        while (!waveform_stopping && !waveform_pending)
            pthread_cond_wait(&waveform_cond, &waveform_mutex);
        if (waveform_stopping) {
            pthread_mutex_unlock(&waveform_mutex);
            return NULL;
        }
        snprintf(path, sizeof(path), "%s", waveform_requested_path);
        generation = waveform_generation;
        waveform_pending = false;
        pthread_mutex_unlock(&waveform_mutex);

        if (generation_cancelled(&generation)) continue;
        file_facts_t facts;
        if (!get_file_facts(path, &facts)) continue;

        uint8_t bins[WAVEFORM_BINS];
        if (cache_load(path, &facts, bins)) {
            file_facts_t confirmed;
            if (!generation_cancelled(&generation) && get_file_facts(path, &confirmed) &&
                facts_equal(&facts, &confirmed) && !generation_cancelled(&generation))
                publish_result(generation, path, bins);
            continue;
        }
        if (generation_cancelled(&generation)) continue;
        if (!audio_extract_waveform(path, bins, WAVEFORM_BINS, generation_cancelled,
                                   &generation))
            continue;
        if (generation_cancelled(&generation)) continue;
        file_facts_t after;
        if (!get_file_facts(path, &after) || !facts_equal(&facts, &after)) continue;
        cache_store(path, &facts, bins);
        file_facts_t confirmed;
        if (!generation_cancelled(&generation) && get_file_facts(path, &confirmed) &&
            facts_equal(&facts, &confirmed) && !generation_cancelled(&generation))
            publish_result(generation, path, bins);
    }
}

static bool start_worker_locked(void) {
    if (waveform_thread_started) return true;
    if (waveform_stopping) return false;
    if (pthread_create(&waveform_thread, NULL, waveform_worker, NULL) != 0) return false;
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
        waveform_pending = false;
        waveform_ready = false;
        waveform_requested_path[0] = '\0';
        waveform_result_path[0] = '\0';
        pthread_mutex_unlock(&waveform_mutex);
        return;
    }
    waveform_stopping = true;
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
