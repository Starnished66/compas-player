#include "plugin_store_internal.h"
#include "store_common.h"
#include "../../download/download.h"
#include "../../../core/files/file_integrity.h"
#include "../../../library/artwork/cache/image_thumb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STORE_PREVIEW_COUNT PLUGIN_STORE_PREVIEW_CAPACITY
#ifndef STORE_PREVIEW_ROOT
#define STORE_PREVIEW_ROOT "/tmp/compas-store-previews"
#endif

/* Layout and theme preview cache: bounded slots, a background fetch worker,
 * and the hash-verified files served to the UI and the remote page. */

static uint64_t preview_epoch = 1, preview_generation, preview_use_counter;
static bool preview_worker_running;
static uint64_t preview_spawn_epoch, preview_spawn_retry_ms;
static unsigned preview_spawn_attempts;
typedef struct {
    char sha[65];
    uint64_t attempted_epoch, retry_after_ms, last_used;
    unsigned attempts;
    bool ready;
} preview_cache_entry_t;
static preview_cache_entry_t preview_cache[STORE_PREVIEW_COUNT];

/* Cache slot holding the preview with this hash, or -1; caller holds store_mutex.
 * Each hash occupies at most one slot (prepare_previews guarantees it). */
static int preview_slot_find_locked(const char * sha) {
    for (int c = 0; c < STORE_PREVIEW_COUNT; c++) {
        if (strcmp(preview_cache[c].sha, sha) == 0) {
            return c;
        }
    }
    return -1;
}

/* Preview I/O never runs on LVGL's thread or changes installer progress. */
typedef struct {
    int slot;
    plugin_store_preview_t preview;
    char evicted_sha[65];
    preview_cache_entry_t previous;
} preview_item_t;
typedef struct {
    uint64_t epoch;
    char tag[64];
    size_t count;
    preview_item_t items[STORE_PREVIEW_COUNT];
} preview_job_t;
static int preview_dir_fd = -1, preview_lock_fd = -1;

static uint64_t preview_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint64_t) now.tv_nsec / 1000000;
}

/* Failed previews are retried only when a later viewport request arrives.
 * Keep network/validation failures slower than decoder contention, and cap
 * both paths at the existing three attempts per catalog epoch. */
static uint64_t preview_retry_delay_ms(unsigned attempts, bool decoder_busy) {
    return (decoder_busy ? 1000u : 2000u) * (uint64_t) attempts;
}

static bool preview_retry_blocked(const preview_cache_entry_t * cached, uint64_t epoch, uint64_t now) {
    if (cached->attempted_epoch != epoch || !cached->attempts) return false;
    return cached->attempts >= 3 || !cached->retry_after_ms || now < cached->retry_after_ms;
}

/* Called under store_mutex after a worker item finishes. Keep the retry state
 * transition here so ordinary transfer failures and decoder contention both
 * remain eligible for a later viewport-triggered attempt. */
static void preview_complete_entry(preview_cache_entry_t * cached, const char * sha,
                                   uint64_t current_epoch, uint64_t job_epoch,
                                   bool ready, bool decoder_busy, uint64_t now) {
    if (current_epoch != job_epoch || strcmp(cached->sha, sha) != 0) return;
    if (ready) {
        cached->ready = true;
        cached->retry_after_ms = 0;
        preview_generation++;
    } else if (cached->attempts < 3) {
        cached->retry_after_ms = now + preview_retry_delay_ms(cached->attempts, decoder_busy);
    }
}

/* <sha>.<ext> inside the preview cache, as a bare file name or a full path. */
static bool preview_path(char * out, size_t size, const char * sha, const char * ext, bool full) {
    int n = full ? snprintf(out, size, "%s/%s.%s", STORE_PREVIEW_ROOT, sha, ext)
                 : snprintf(out, size, "%s.%s", sha, ext);
    return n > 0 && (size_t) n < size;
}

/* The cache directory, checked to be ours and private; -1 on any problem. */
static int preview_open_dir(void) {
    if (mkdir(STORE_PREVIEW_ROOT, 0700) != 0 && errno != EEXIST) return -1;
    int dir = open(STORE_PREVIEW_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    struct stat st;
    if (fstat(dir, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        close(dir);
        return -1;
    }
    return dir;
}

/* The exclusive owner lock inside dir; -1 when another process holds it. */
static int preview_lock_dir(int dir) {
    struct stat st;
    int lock = openat(dir, ".owner-lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock < 0 || fstat(lock, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) close(lock);
        return -1;
    }
    return lock;
}

/* Removes one cache directory entry unless it belongs to a slot. */
static void preview_prune_entry(int dir, const char * name) {
    size_t len = strlen(name);
    bool part = strncmp(name, ".part-", 6) == 0;
    bool thumb_tmp = len == 72 && strcmp(name + 64, ".bin.tmp") == 0;
    bool keep = false;
    struct stat st;
    if ((len == 68 && (strcmp(name + 64, ".jpg") == 0 || strcmp(name + 64, ".bin") == 0))) {
        char sha[65]; memcpy(sha, name, 64); sha[64] = 0;
        if (!store_sha_valid(sha)) return;
        pthread_mutex_lock(&store_mutex);
        keep = preview_slot_find_locked(sha) >= 0;
        pthread_mutex_unlock(&store_mutex);
    } else if (!part && !thumb_tmp) return;
    if (!keep && fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode) && st.st_uid == geteuid()) unlinkat(dir, name, 0);
}

/* Keep only files assigned to our bounded cache, including previews that
 * can be reused after a player restart. Ignore unrelated filenames. */
static void preview_prune_cache(int dir) {
    int scan_fd = dup(dir);
    DIR * scan = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!scan && scan_fd >= 0) close(scan_fd);
    if (!scan) return;
    struct dirent * ent;
    while ((ent = readdir(scan)) != NULL) preview_prune_entry(dir, ent->d_name);
    closedir(scan);
}

static bool preview_cache_open(void) {
    if (preview_dir_fd >= 0) return true;
    int dir = preview_open_dir();
    if (dir < 0) return false;
    int lock = preview_lock_dir(dir);
    if (lock < 0) {
        close(dir);
        return false;
    }
    preview_prune_cache(dir);
    preview_lock_fd = lock; /* Held for this process; no stale PID/lock directory. */
    preview_dir_fd = dir;
    return true;
}

/* Reads a regular file of exactly `size` bytes (at least min_size, at most
 * the preview limit) into a new buffer; NULL on any mismatch. */
static uint8_t * preview_read_exact(const char * path, uint32_t size, uint32_t min_size) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size == size && st.st_size >= min_size && st.st_size <= STORE_PREVIEW_LIMIT;
    uint8_t * data = valid ? malloc(size) : NULL;
    size_t used = 0;
    while (data && used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t) n;
    }
    close(fd);
    if (!data || used != size) { free(data); return NULL; }
    return data;
}

/* Baseline JPEG with the declared dimensions, one frame, and a scan. */
static bool preview_jpeg_buffer_valid(const uint8_t * data, size_t used,
                                      const plugin_store_preview_t * meta) {
    bool valid = data[0] == 0xff && data[1] == 0xd8 &&
                 data[used - 2] == 0xff && data[used - 1] == 0xd9;
    bool frame = false, scan = false;
    for (size_t pos = 2; valid && pos < used; ) {
        if (data[pos] != 0xff) { valid = false; break; }
        while (pos < used && data[pos] == 0xff) pos++;
        if (pos >= used) { valid = false; break; }
        unsigned marker = data[pos++];
        if (marker == 0xd9) break;
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 1) continue;
        if (used - pos < 2) { valid = false; break; }
        size_t len = ((size_t) data[pos] << 8) | data[pos + 1];
        if (len < 2 || len > used - pos) { valid = false; break; }
        if (marker == 0xc0) {
            if (frame || len < 8) { valid = false; break; }
            unsigned h = ((unsigned) data[pos + 3] << 8) | data[pos + 4];
            unsigned w = ((unsigned) data[pos + 5] << 8) | data[pos + 6];
            unsigned components = data[pos + 7];
            valid = data[pos + 2] == 8 && (components == 1 || components == 3) &&
                    len == 8 + 3 * components && w == meta->width && h == meta->height;
            frame = valid;
        } else if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            valid = false;
        } else if (marker == 0xda) { scan = frame && len >= 6; break; }
        pos += len;
    }
    return valid && frame && scan;
}

static bool preview_jpeg_valid(const char * path, const plugin_store_preview_t * meta) {
    uint8_t * data = preview_read_exact(path, meta->size, 4);
    bool valid = data && preview_jpeg_buffer_valid(data, meta->size, meta);
    free(data);
    return valid;
}

static bool preview_file_verified(const char * path, const plugin_store_preview_t * meta) {
    char sha[65]; uint64_t bytes;
    return preview_jpeg_valid(path, meta) && store_hash_file(path, &bytes, sha) &&
           bytes == meta->size && strcmp(sha, meta->sha256) == 0;
}

static bool preview_progress(uint64_t downloaded, uint64_t total, void * context) {
    (void) downloaded; (void) total;
    const preview_job_t * job = context;
    pthread_mutex_lock(&store_mutex);
    bool current = preview_epoch == job->epoch;
    pthread_mutex_unlock(&store_mutex);
    return current;
}

static bool preview_decode_cancel(void * context) {
    return !preview_progress(0, 0, context);
}

static bool preview_read_jpeg(const char * path, const plugin_store_preview_t * meta,
                              uint8_t ** data_out) {
    *data_out = NULL;
    if (!preview_file_verified(path, meta)) return false;
    *data_out = preview_read_exact(path, meta->size, 1);
    return *data_out != NULL;
}

static void preview_finish_item(preview_job_t * job, const preview_item_t * item,
                                bool ready, bool decoder_busy) {
    pthread_mutex_lock(&store_mutex);
    preview_complete_entry(&preview_cache[item->slot], item->preview.sha256, preview_epoch,
                           job->epoch, ready, decoder_busy, preview_now_ms());
    pthread_mutex_unlock(&store_mutex);
}

/* Drain every eviction even if navigation cancelled the transfer batch.
 * Slot ownership changed before launch; skipping later evictions would
 * lose their filenames and let abandoned cache files accumulate. */
static void preview_drain_evictions(const preview_job_t * job) {
    for (size_t i = 0; i < job->count; i++) {
        if (!job->items[i].evicted_sha[0]) continue;
        char name[80];
        preview_path(name, sizeof(name), job->items[i].evicted_sha, "jpg", false);
        unlinkat(preview_dir_fd, name, 0);
        preview_path(name, sizeof(name), job->items[i].evicted_sha, "bin", false);
        unlinkat(preview_dir_fd, name, 0);
    }
}

/* Makes sure the verified JPEG is at path, downloading it when it is not. */
static bool preview_fetch_jpeg(preview_job_t * job, const preview_item_t * item, const char * path) {
    if (preview_file_verified(path, &item->preview)) return true;
    char part[PATH_MAX], url[512];
    snprintf(part, sizeof(part), "%s/.part-%s", STORE_PREVIEW_ROOT, item->preview.sha256);
    unlink(part); /* Private directory, owned by this worker's process lock. */
    download_request_t request = { .url = url, .dest_path = part,
        .max_bytes = STORE_PREVIEW_LIMIT, .has_expected_size = true,
        .expected_size = item->preview.size, .expected_sha256 = item->preview.sha256,
        .verify_tls = true, .max_redirects = 5, .connect_timeout_ms = 5000,
        .read_timeout_ms = 8000 };
    download_result_t result;
    /* Real directory I/O errors remain failures even after commit. */
    bool ready = ps_make_asset_url(url, sizeof(url), job->tag, item->preview.asset) &&
        download_to_file(&request, NULL, preview_progress, job, &result) == DOWNLOAD_OK &&
        preview_progress(0, 0, job) && preview_jpeg_valid(part, &item->preview) &&
        rename(part, path) == 0 && filesystem_sync_parent(path);
    if (!ready) unlink(part);
    return ready;
}

/* Decodes the JPEG at path into the card-sized .bin. *retryable is set when
 * the decoder was only busy. */
static bool preview_make_bin(preview_job_t * job, const preview_item_t * item,
                             const char * path, bool * retryable) {
    char bin_path[PATH_MAX];
    preview_path(bin_path, sizeof(bin_path), item->preview.sha256, "bin", true);
    uint8_t * jpeg = NULL;
    const char * reason = NULL;
    bool ready = false;
    if (preview_read_jpeg(path, &item->preview, &jpeg)) {
        int card_w, cover_h;
        store_preview_bounds(&card_w, &cover_h);
        ready = image_thumb_write_bin(jpeg, item->preview.size, card_w, cover_h, bin_path,
                                      ARTWORK_PRIO_THUMBNAIL, preview_decode_cancel, job,
                                      &reason) && preview_progress(0, 0, job);
        *retryable = !ready && reason && strcmp(reason, "busy") == 0;
        free(jpeg);
    }
    if (!ready && preview_progress(0, 0, job)) unlink(bin_path);
    return ready;
}

static void * preview_worker(void * context) {
    preview_job_t * job = context;
    bool opened = preview_cache_open();
    if (opened) preview_drain_evictions(job);
    for (size_t i = 0; opened && i < job->count && preview_progress(0, 0, job); i++) {
        const preview_item_t * item = &job->items[i];
        char path[PATH_MAX];
        bool retryable = false;
        preview_path(path, sizeof(path), item->preview.sha256, "jpg", true);
        bool ready = preview_fetch_jpeg(job, item, path) && preview_progress(0, 0, job) &&
                     preview_make_bin(job, item, path, &retryable);
        preview_finish_item(job, item, ready, retryable);
    }
    /* Without the cache directory no item ran; fail each so its slot gets a
     * retry time instead of staying blocked until the epoch changes. */
    for (size_t i = 0; !opened && i < job->count; i++) {
        preview_finish_item(job, &job->items[i], false, false);
    }
    pthread_mutex_lock(&store_mutex);
    preview_worker_running = false;
    pthread_mutex_unlock(&store_mutex);
    free(job);
    return NULL;
}

/* False while a worker runs or the spawn retry window is closed. Caller holds store_mutex. */
static bool preview_spawn_allowed_locked(uint64_t now) {
    if (preview_worker_running || !last_index_count) return false;
    if (preview_spawn_epoch != preview_epoch) {
        preview_spawn_epoch = preview_epoch;
        preview_spawn_attempts = 0;
        preview_spawn_retry_ms = 0;
    }
    return now >= preview_spawn_retry_ms && preview_spawn_attempts < 3;
}

/* Finds the catalog entries asked for and pins their cached slots. Caller holds store_mutex. */
static void preview_collect_requested_locked(const char * const * ids, size_t count,
                                             const plugin_store_plugin_t ** requested, bool * protected) {
    for (size_t i = 0; i < count; i++) {
        if (!ids[i]) continue;
        const plugin_store_plugin_t * entry = ps_index_find_locked(ids[i]);
        if (!entry || !ps_preview_eligible(entry)) continue;
        requested[i] = entry;
        int cached_slot = preview_slot_find_locked(entry->preview.sha256);
        if (cached_slot >= 0) protected[cached_slot] = true;
    }
}

/* The slot to fetch p into, or -1 when it is ready, blocked or has no slot.
 * Caller holds store_mutex. */
static int preview_pick_slot_locked(const plugin_store_plugin_t * p, const bool * protected, uint64_t now) {
    int slot = preview_slot_find_locked(p->preview.sha256);
    if (slot >= 0) {
        preview_cache_entry_t * cached = &preview_cache[slot];
        cached->last_used = ++preview_use_counter;
        if (cached->ready) return -1;
        if (preview_retry_blocked(cached, preview_epoch, now)) return -1;
        return slot;
    }
    /* Visible/nearby cards are pinned for this request. Replace only
     * an offscreen slot, preferring an empty then the oldest slot. */
    for (int c = 0; c < STORE_PREVIEW_COUNT; c++) {
        if (protected[c]) continue;
        if (slot < 0 || preview_cache[c].last_used < preview_cache[slot].last_used) slot = c;
        if (!preview_cache[c].sha[0]) { slot = c; break; }
    }
    return slot;
}

/* Queues p for slot in the job and marks the slot as attempted. Caller holds store_mutex. */
static void preview_job_add_locked(preview_job_t * job, int slot, const plugin_store_plugin_t * p) {
    preview_item_t * item = &job->items[job->count++];
    item->slot = slot;
    item->preview = p->preview;
    preview_cache_entry_t * cached = &preview_cache[slot];
    item->previous = *cached;
    if (strcmp(cached->sha, p->preview.sha256) != 0) {
        memcpy(item->evicted_sha, cached->sha, sizeof(item->evicted_sha));
        if (cached->ready) preview_generation++;
        memset(cached, 0, sizeof(*cached));
        memcpy(cached->sha, p->preview.sha256, sizeof(cached->sha));
    }
    if (cached->attempted_epoch != job->epoch) cached->attempts = 0;
    cached->attempted_epoch = job->epoch;
    cached->attempts++;
    cached->retry_after_ms = 0;
    cached->last_used = ++preview_use_counter;
}

/* Starts the worker for the job (sets preview_worker_running under the lock the
 * caller holds, then releases it). */
static bool preview_launch(preview_job_t * job) {
    preview_worker_running = job != NULL;
    pthread_mutex_unlock(&store_mutex);
    if (!job) return false;
    if (!store_start_thread(preview_worker, job)) {
        pthread_mutex_lock(&store_mutex);
        preview_worker_running = false;
        preview_spawn_attempts++;
        preview_spawn_retry_ms = preview_now_ms() + 1000;
        for (size_t i = 0; i < job->count; i++) {
            /* No worker ran: keep old files and their ownership so a failed
             * thread start cannot orphan evicted files in the RAM cache. */
            preview_cache[job->items[i].slot] = job->items[i].previous;
        }
        pthread_mutex_unlock(&store_mutex);
        free(job); return false;
    }
    pthread_mutex_lock(&store_mutex);
    preview_spawn_attempts = 0;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

bool plugin_store_prepare_previews(const char * const * ids, size_t count) {
    if (!ids || !count) return false;
    if (count > STORE_PREVIEW_COUNT) count = STORE_PREVIEW_COUNT;
    pthread_mutex_lock(&store_mutex);
    uint64_t now = preview_now_ms();
    if (!preview_spawn_allowed_locked(now)) {
        pthread_mutex_unlock(&store_mutex); return false;
    }
    const plugin_store_plugin_t * requested[STORE_PREVIEW_COUNT] = {0};
    bool protected[STORE_PREVIEW_COUNT] = {0};
    preview_collect_requested_locked(ids, count, requested, protected);
    preview_job_t * job = NULL;
    for (size_t i = 0; i < count; i++) {
        if (!requested[i]) continue;
        int slot = preview_pick_slot_locked(requested[i], protected, now);
        if (slot < 0) continue;
        if (!job) {
            job = calloc(1, sizeof(*job));
            if (!job) break;
            job->epoch = preview_epoch;
            snprintf(job->tag, sizeof(job->tag), "%s", last_tag);
        }
        preview_job_add_locked(job, slot, requested[i]);
        protected[slot] = true;
    }
    return preview_launch(job);
}

/* The catalog preview of id when it is cached and ready; caller holds store_mutex. */
static bool ready_preview_locked(const char * id, plugin_store_preview_t * meta) {
    const plugin_store_plugin_t * entry = ps_index_find_locked(id);
    if (!entry || !entry->preview.asset[0]) return false;
    int slot = preview_slot_find_locked(entry->preview.sha256);
    if (slot < 0 || !preview_cache[slot].ready) return false;
    *meta = entry->preview;
    return true;
}

bool plugin_store_get_preview(const char * id, char * out, size_t size) {
    if (!id || !out || !size) return false;
    out[0] = 0;
    plugin_store_preview_t meta = {0};
    pthread_mutex_lock(&store_mutex);
    bool ready = ready_preview_locked(id, &meta);
    pthread_mutex_unlock(&store_mutex);
    if (!ready || !preview_path(out, size, meta.sha256, "bin", true)) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool plugin_store_get_preview_jpeg(const char * id, char * out, size_t size) {
    if (!id || !out || !size) return false;
    out[0] = 0;
    plugin_store_preview_t meta = {0};
    pthread_mutex_lock(&store_mutex);
    bool ready = ready_preview_locked(id, &meta);
    pthread_mutex_unlock(&store_mutex);
    if (!ready) return false;

    if (!preview_path(out, size, meta.sha256, "jpg", true) || !preview_file_verified(out, &meta)) {
        out[0] = 0;
        return false;
    }
    return true;
}

/* State of a preview that has a cache slot. Caller holds store_mutex. */
static plugin_store_preview_state_t preview_state_cached_locked(const preview_cache_entry_t * cached,
                                                                uint64_t now) {
    if (cached->ready) return PLUGIN_STORE_PREVIEW_READY;
    if (preview_worker_running ||
        (cached->attempted_epoch == preview_epoch && cached->attempts < 3 &&
         cached->retry_after_ms > now)) {
        return PLUGIN_STORE_PREVIEW_PENDING;
    }
    if (cached->attempted_epoch == preview_epoch && cached->attempts >= 3) {
        return PLUGIN_STORE_PREVIEW_FAILED;
    }
    return PLUGIN_STORE_PREVIEW_STARTABLE;
}

/* State of a preview with no slot yet, from the worker spawn limits. Caller holds store_mutex. */
static plugin_store_preview_state_t preview_state_uncached_locked(uint64_t now) {
    if (preview_worker_running ||
        (preview_spawn_epoch == preview_epoch && preview_spawn_attempts < 3 &&
         preview_spawn_retry_ms > now)) {
        return PLUGIN_STORE_PREVIEW_PENDING;
    }
    if (preview_spawn_epoch == preview_epoch && preview_spawn_attempts >= 3) {
        return PLUGIN_STORE_PREVIEW_FAILED;
    }
    return PLUGIN_STORE_PREVIEW_STARTABLE;
}

plugin_store_preview_state_t plugin_store_preview_state(const char * id) {
    if (!id || !id[0]) return PLUGIN_STORE_PREVIEW_UNKNOWN;
    plugin_store_preview_state_t state = PLUGIN_STORE_PREVIEW_UNKNOWN;
    pthread_mutex_lock(&store_mutex);
    const plugin_store_plugin_t * plugin = ps_index_find_locked(id);
    if (plugin && !ps_preview_eligible(plugin)) {
        state = PLUGIN_STORE_PREVIEW_NONE;
    } else if (plugin) {
        uint64_t now = preview_now_ms();
        int slot = preview_slot_find_locked(plugin->preview.sha256);
        state = slot >= 0 ? preview_state_cached_locked(&preview_cache[slot], now)
                          : preview_state_uncached_locked(now);
    }
    pthread_mutex_unlock(&store_mutex);
    return state;
}

/* The catalog changed or the store was reset, so pending preview work is
 * stale. The caller holds store_mutex. */
void plugin_store_preview_invalidate_locked(void) {
    preview_epoch++;
}

void plugin_store_cancel_previews(void) {
    pthread_mutex_lock(&store_mutex);
    plugin_store_preview_invalidate_locked();
    pthread_mutex_unlock(&store_mutex);
}

uint64_t plugin_store_preview_generation(void) {
    pthread_mutex_lock(&store_mutex);
    uint64_t generation = preview_generation;
    pthread_mutex_unlock(&store_mutex);
    return generation;
}
