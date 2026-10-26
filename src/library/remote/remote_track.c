#include "remote_track.h"

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STREAM_REQUEST_SLOTS 3
#define STREAM_WAIT_MS 15000
#define STREAM_DEFAULT_TTL 60
#define STREAM_SAFETY_MS 2000
/* Error texts are translation keys, shown through TR() by the player. */
#define N_(text) text

#define REMOTE_TRACK_MAX_ENTRIES 500 /* same bound as PLUGIN_MAX_LIST_ITEMS (plugin_manager.c), the Lua-facing cap on any queue-list plugin API -- kept in sync by comment, not #include, to avoid a library->plugins dependency */

/* Guards remote_track_table/remote_track_table_count -- written from the UI
 * thread only (gui_plugin_play_remote_tracks() -> remote_track_meta_set_all()),
 * read from the UI thread AND the audio/seek-worker threads (decoder_open()
 * -> remote_track_meta_copy_for_path()). See remote_track_meta_copy_for_path()'s
 * own header comment for the exact race this closes. */
static pthread_mutex_t remote_track_mutex = PTHREAD_MUTEX_INITIALIZER;
static remote_track_meta_t * remote_track_table = NULL;
static int remote_track_table_count = 0;

typedef enum { SLOT_EMPTY, SLOT_QUEUED, SLOT_SENT, SLOT_DONE, SLOT_FAILED } slot_state_t;
typedef struct {
    slot_state_t state;
    uint64_t id, generation, created_ms;
    char path[256], error[256];
} stream_slot_t;
static stream_slot_t stream_slots[STREAM_REQUEST_SLOTS];
static uint64_t next_request_id = 1;
static char active_path[256];
static uint64_t active_generation;
static pthread_cond_t stream_cond;
static pthread_once_t stream_once = PTHREAD_ONCE_INIT;
static clockid_t stream_cond_clock = CLOCK_REALTIME;
static void (*stream_request_notify)(void);
#ifdef REMOTE_TRACK_TESTING
static bool test_clock_set;
static uint64_t test_clock_ms;
static unsigned int test_wait_timeout_ms = STREAM_WAIT_MS;
static unsigned int test_waiters;
#endif

static void stream_init(void) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_cond_init(&stream_cond, NULL);
        return;
    }
#ifdef CLOCK_MONOTONIC
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) stream_cond_clock = CLOCK_MONOTONIC;
#endif
    pthread_cond_init(&stream_cond, &attr);
    pthread_condattr_destroy(&attr);
}

static uint64_t stream_now_ms(void) {
#ifdef REMOTE_TRACK_TESTING
    if (test_clock_set) return test_clock_ms;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t cond_clock_ms(void) {
    struct timespec ts;
    clock_gettime(stream_cond_clock, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int find_meta_locked(const char *path) {
    if (!remote_track_path_is_remote(path)) return -1;
    const char *rest = path + 9, *slash = strchr(rest, '/');
    if (!slash) return -1;
    size_t pn = (size_t)(slash - rest);
    for (int i = 0; i < remote_track_table_count; ++i)
        if (strlen(remote_track_table[i].provider) == pn &&
            !strncmp(remote_track_table[i].provider, rest, pn) &&
            !strcmp(remote_track_table[i].track_id, slash + 1)) return i;
    return -1;
}

static int slot_for_locked(const char *path, uint64_t gen) {
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i)
        if (stream_slots[i].state != SLOT_EMPTY && stream_slots[i].generation == gen &&
            !strcmp(stream_slots[i].path, path)) return i;
    return -1;
}

static bool fresh_locked(int ix) {
    if (ix < 0) return false;
    remote_track_meta_t *m = &remote_track_table[ix];
    if (!m->stream_url[0]) return false;
    if (!m->resolve_stream) return true;
    uint64_t ttl = (uint64_t)(m->url_ttl_s ? m->url_ttl_s : STREAM_DEFAULT_TTL) * 1000u;
    uint64_t now = stream_now_ms();
    uint64_t guard = ttl / 4u;
    if (guard > STREAM_SAFETY_MS) guard = STREAM_SAFETY_MS;
    if (!guard) guard = 1;
    return now >= m->stream_fetched_ms && now - m->stream_fetched_ms < ttl - guard;
}

static uint64_t request_lifetime_ms(void) {
#ifdef REMOTE_TRACK_TESTING
    return test_wait_timeout_ms;
#else
    return STREAM_WAIT_MS;
#endif
}

static void expire_slots_locked(void) {
    uint64_t now = stream_now_ms(), life = request_lifetime_ms();
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i) {
        stream_slot_t *s = &stream_slots[i];
        if ((s->state == SLOT_QUEUED || s->state == SLOT_SENT) && now >= s->created_ms && now - s->created_ms >= life) {
            s->state = SLOT_FAILED;
            snprintf(s->error, sizeof(s->error), N_("The streaming service did not answer"));
        }
    }
}

bool remote_track_make_key(const char * provider, const char * track_id, char * out, size_t out_size) {
    if (!provider || !provider[0] || !track_id || !track_id[0]) return false;
    for (const char * p = provider; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    for (const char * p = track_id; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    int n = snprintf(out, out_size, "remote://%s/%s", provider, track_id);
    return n > 0 && (size_t) n < out_size;
}

bool remote_track_path_is_remote(const char * path) {
    return path && strncmp(path, "remote://", 9) == 0;
}

bool remote_track_meta_set_all(const remote_track_meta_t * entries, int count) {
    pthread_once(&stream_once, stream_init);
    if (count < 0) count = 0;
    if (count > REMOTE_TRACK_MAX_ENTRIES) count = REMOTE_TRACK_MAX_ENTRIES;

    remote_track_meta_t * new_table = NULL;
    if (count > 0) {
        /* Validate every entry's key BEFORE touching any shared state or
         * allocating -- an all-or-nothing publish, never a partially
         * replaced table (a plugin's own malformed entry shouldn't be able
         * to wipe out a previously-working remote queue). */
        char key[256];
        for (int i = 0; i < count; i++) {
            if (!remote_track_make_key(entries[i].provider, entries[i].track_id, key, sizeof(key))) return false;
        }
        new_table = malloc(sizeof(remote_track_meta_t) * (size_t) count);
        if (!new_table) return false;
        memcpy(new_table, entries, sizeof(remote_track_meta_t) * (size_t) count);
        uint64_t now = stream_now_ms();
        for (int i = 0; i < count; ++i) {
            if (new_table[i].resolve_stream && new_table[i].stream_url[0]) {
                if (!new_table[i].url_ttl_s) new_table[i].url_ttl_s = STREAM_DEFAULT_TTL;
                new_table[i].stream_fetched_ms = now;
            }
        }
    }

    pthread_mutex_lock(&remote_track_mutex);
    remote_track_meta_t * old_table = remote_track_table;
    remote_track_table = new_table;
    remote_track_table_count = new_table ? count : 0;
    memset(stream_slots, 0, sizeof(stream_slots));
    active_path[0] = 0;
    active_generation = 0;
    pthread_cond_broadcast(&stream_cond);
    pthread_mutex_unlock(&remote_track_mutex);

    /* Freed AFTER the swap, outside the lock -- any reader that was mid-
     * copy under the lock already finished (and released it) before this
     * unlock could have happened; remote_track_meta_copy_for_path() never
     * hands out a pointer that could still be in use here. */
    free(old_table);

    return true;
}

bool remote_track_meta_copy_for_path(const char * path, remote_track_meta_t * out) {
    if (!remote_track_path_is_remote(path)) return false;
    const char * rest = path + 9; /* skip the "remote://" prefix already confirmed present */
    const char * slash = strchr(rest, '/');
    if (!slash) return false; /* malformed -- no provider/track_id separator */
    size_t provider_len = (size_t) (slash - rest);
    const char * track_id = slash + 1;
    bool found = false;
    pthread_mutex_lock(&remote_track_mutex);
    for (int i = 0; i < remote_track_table_count; i++) {
        if (strlen(remote_track_table[i].provider) == provider_len &&
            strncmp(remote_track_table[i].provider, rest, provider_len) == 0 &&
            strcmp(remote_track_table[i].track_id, track_id) == 0) {
            *out = remote_track_table[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&remote_track_mutex);
    return found;
}

bool remote_track_meta_copy_fresh_for_path(const char *path, remote_track_meta_t *out) {
    pthread_once(&stream_once, stream_init);
    if (!out) return false;
    pthread_mutex_lock(&remote_track_mutex);
    int ix = find_meta_locked(path);
    bool ok = fresh_locked(ix);
    if (ok) *out = remote_track_table[ix];
    pthread_mutex_unlock(&remote_track_mutex);
    return ok;
}

void remote_track_stream_begin_playback(const char *path, uint64_t generation) {
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    char old_path[sizeof(active_path)];
    snprintf(old_path, sizeof(old_path), "%s", active_path);
    bool promote = path && path[0] && strcmp(old_path, path) != 0;
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i) {
        stream_slot_t *s = &stream_slots[i];
        if (promote && !strcmp(s->path, path) && (s->state == SLOT_QUEUED || s->state == SLOT_SENT)) {
            s->generation = generation;
            s->created_ms = stream_now_ms();
        }
        else memset(s, 0, sizeof(*s));
    }
    active_generation = generation;
    snprintf(active_path, sizeof(active_path), "%s", path ? path : "");
    pthread_cond_broadcast(&stream_cond);
    pthread_mutex_unlock(&remote_track_mutex);
}

bool remote_track_stream_is_fresh(const char *path) {
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    bool ok = fresh_locked(find_meta_locked(path));
    pthread_mutex_unlock(&remote_track_mutex);
    return ok;
}

bool remote_track_stream_prefetch(const char *path, uint64_t generation) {
    pthread_once(&stream_once, stream_init);
    if (!remote_track_path_is_remote(path) || !generation) return false;
    pthread_mutex_lock(&remote_track_mutex);
    expire_slots_locked();
    if (!generation || generation != active_generation) { pthread_mutex_unlock(&remote_track_mutex); return false; }
    int ix = find_meta_locked(path);
    if (ix < 0 || fresh_locked(ix)) { pthread_mutex_unlock(&remote_track_mutex); return ix >= 0; }
    if (!remote_track_table[ix].resolver_owner) { pthread_mutex_unlock(&remote_track_mutex); return false; }
    int existing = slot_for_locked(path, generation);
    if (existing >= 0) {
        if (stream_slots[existing].state == SLOT_DONE) memset(&stream_slots[existing], 0, sizeof(stream_slots[existing]));
        else { pthread_mutex_unlock(&remote_track_mutex); return true; }
    }
    int slot = -1;
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i)
        if (stream_slots[i].state == SLOT_EMPTY || stream_slots[i].state == SLOT_DONE || stream_slots[i].state == SLOT_FAILED) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i)
            if (strcmp(stream_slots[i].path, active_path) != 0) { slot = i; break; }
    }
    if (slot < 0) { pthread_mutex_unlock(&remote_track_mutex); return false; }
    stream_slot_t *s = &stream_slots[slot];
    memset(s, 0, sizeof(*s)); s->state = SLOT_QUEUED; s->generation = generation;
    s->id = next_request_id++;
    s->created_ms = stream_now_ms();
    if (!next_request_id) next_request_id = 1;
    snprintf(s->path, sizeof(s->path), "%s", path);
    void (*notify)(void) = stream_request_notify;
    pthread_mutex_unlock(&remote_track_mutex);
    if (notify) notify();
    return true;
}

void remote_track_stream_set_request_notify(void (*notify)(void)) {
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    stream_request_notify = notify;
    pthread_mutex_unlock(&remote_track_mutex);
}

void remote_track_stream_adopt_owner(const char *plugin_id, uintptr_t owner) {
    if (!plugin_id || !plugin_id[0] || !owner) return;
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    for (int i = 0; i < remote_track_table_count; ++i) {
        remote_track_meta_t *m = &remote_track_table[i];
        if (!m->resolver_owner && m->resolve_stream && !strcmp(m->resolver_plugin, plugin_id))
            m->resolver_owner = owner;
    }
    pthread_cond_broadcast(&stream_cond);
    pthread_mutex_unlock(&remote_track_mutex);
}

bool remote_track_stream_take_request(remote_stream_request_t *out) {
    if (!out) return false;
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    expire_slots_locked();
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i) {
        stream_slot_t *s = &stream_slots[i];
        if (s->state != SLOT_QUEUED) continue;
        int ix = find_meta_locked(s->path);
        if (ix < 0 || !remote_track_table[ix].resolver_owner) {
            s->state = SLOT_FAILED; snprintf(s->error, sizeof(s->error), N_("The streaming plugin is not available")); continue;
        }
        s->state = SLOT_SENT;
        out->request_id = s->id;
        out->owner = remote_track_table[ix].resolver_owner;
        snprintf(out->provider, sizeof(out->provider), "%s", remote_track_table[ix].provider);
        snprintf(out->track_id, sizeof(out->track_id), "%s", remote_track_table[ix].track_id);
        pthread_mutex_unlock(&remote_track_mutex);
        return true;
    }
    pthread_mutex_unlock(&remote_track_mutex);
    return false;
}

bool remote_track_stream_provide(uintptr_t owner, uint64_t request_id, const remote_track_meta_t *stream, const char *error) {
    pthread_once(&stream_once, stream_init);
    pthread_mutex_lock(&remote_track_mutex);
    expire_slots_locked();
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i) {
        stream_slot_t *s = &stream_slots[i];
        if (s->state != SLOT_SENT || s->id != request_id) continue;
        int ix = find_meta_locked(s->path);
        if (ix < 0 || s->generation != active_generation) {
            s->state = SLOT_EMPTY; pthread_cond_broadcast(&stream_cond); pthread_mutex_unlock(&remote_track_mutex); return false;
        }
        if (remote_track_table[ix].resolver_owner != owner) { pthread_mutex_unlock(&remote_track_mutex); return false; }
        if (error && error[0]) {
            snprintf(s->error, sizeof(s->error), "%s", error); s->state = SLOT_FAILED;
        } else if (stream && stream->stream_url[0]) {
            remote_track_meta_t *m = &remote_track_table[ix];
            snprintf(m->stream_url, sizeof(m->stream_url), "%s", stream->stream_url);
            if (stream->codec[0]) snprintf(m->codec, sizeof(m->codec), "%s", stream->codec);
            if (stream->sample_rate) m->sample_rate = stream->sample_rate;
            if (stream->bit_depth) m->bit_depth = stream->bit_depth;
            if (stream->channels) m->channels = stream->channels;
            if (stream->bitrate_kbps) m->bitrate_kbps = stream->bitrate_kbps;
            if (stream->url_ttl_s) m->url_ttl_s = stream->url_ttl_s;
            if (!m->url_ttl_s) m->url_ttl_s = STREAM_DEFAULT_TTL;
            m->stream_fetched_ms = stream_now_ms(); m->resolve_stream = true;
            s->state = SLOT_DONE;
        } else { snprintf(s->error, sizeof(s->error), N_("Could not get a stream link")); s->state = SLOT_FAILED; }
        pthread_cond_broadcast(&stream_cond); pthread_mutex_unlock(&remote_track_mutex); return true;
    }
    pthread_mutex_unlock(&remote_track_mutex); return false;
}

bool remote_track_stream_wait(const char *path, uint64_t generation, remote_track_meta_t *out) {
    pthread_once(&stream_once, stream_init);
    if (!out || !path) return false;
    uint64_t wait_deadline_ms;
    unsigned int timeout_ms = STREAM_WAIT_MS;
#ifdef REMOTE_TRACK_TESTING
    timeout_ms = test_wait_timeout_ms;
#endif
    wait_deadline_ms = cond_clock_ms() + timeout_ms;
    pthread_mutex_lock(&remote_track_mutex);
    for (;;) {
        expire_slots_locked();
        if (active_generation != generation || strcmp(active_path, path)) { pthread_mutex_unlock(&remote_track_mutex); return false; }
        int ix = find_meta_locked(path);
        if (ix < 0) { pthread_mutex_unlock(&remote_track_mutex); return false; }
        if (fresh_locked(ix)) { *out = remote_track_table[ix]; pthread_mutex_unlock(&remote_track_mutex); return true; }
        if (!remote_track_table[ix].resolver_owner) { pthread_mutex_unlock(&remote_track_mutex); return false; }
        int si = slot_for_locked(path, generation);
        if (si >= 0 && stream_slots[si].state == SLOT_DONE) {
            memset(&stream_slots[si], 0, sizeof(stream_slots[si]));
            si = -1;
        }
        if (si < 0) {
            pthread_mutex_unlock(&remote_track_mutex);
            if (!remote_track_stream_prefetch(path, generation)) return false;
            pthread_mutex_lock(&remote_track_mutex); continue;
        }
        if (stream_slots[si].state == SLOT_FAILED || stream_slots[si].state == SLOT_EMPTY) { pthread_mutex_unlock(&remote_track_mutex); return false; }
        uint64_t until_ms = wait_deadline_ms;
        uint64_t request_left = request_lifetime_ms();
        uint64_t now_stream = stream_now_ms();
        uint64_t now_cond = cond_clock_ms();
        if (now_cond >= wait_deadline_ms) {
            si = slot_for_locked(path, generation);
            if (si >= 0 && (stream_slots[si].state == SLOT_QUEUED || stream_slots[si].state == SLOT_SENT)) {
                stream_slots[si].state = SLOT_FAILED;
                snprintf(stream_slots[si].error, sizeof(stream_slots[si].error), N_("The streaming service did not answer"));
            }
            pthread_mutex_unlock(&remote_track_mutex); return false;
        }
        if (stream_slots[si].created_ms <= now_stream) {
            uint64_t elapsed = now_stream - stream_slots[si].created_ms;
            request_left = elapsed >= request_lifetime_ms() ? 0 : request_lifetime_ms() - elapsed;
        }
        if (request_left < until_ms - now_cond) until_ms = now_cond + request_left;
        struct timespec deadline = {(time_t)(until_ms / 1000u), (long)(until_ms % 1000u) * 1000000L};
#ifdef REMOTE_TRACK_TESTING
        ++test_waiters; pthread_cond_broadcast(&stream_cond);
#endif
        int e = pthread_cond_timedwait(&stream_cond, &remote_track_mutex, &deadline);
#ifdef REMOTE_TRACK_TESTING
        --test_waiters; pthread_cond_broadcast(&stream_cond);
#endif
        if (e == ETIMEDOUT) {
            /* A provider may have completed at the deadline boundary. Loop
             * and recheck freshness/state before deciding this is a timeout. */
            continue;
        }
    }
}

void remote_track_stream_cancel_owner(uintptr_t owner) {
    pthread_once(&stream_once, stream_init); pthread_mutex_lock(&remote_track_mutex);
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i) {
        int ix = find_meta_locked(stream_slots[i].path);
        if (stream_slots[i].state != SLOT_EMPTY && ix >= 0 && remote_track_table[ix].resolver_owner == owner) memset(&stream_slots[i], 0, sizeof(stream_slots[i]));
    }
    for (int i = 0; i < remote_track_table_count; ++i) if (remote_track_table[i].resolver_owner == owner) {
        remote_track_table[i].resolver_owner = 0;
        if (remote_track_table[i].resolve_stream) {
            remote_track_table[i].stream_url[0] = 0; remote_track_table[i].stream_fetched_ms = 0;
        }
    }
    pthread_cond_broadcast(&stream_cond); pthread_mutex_unlock(&remote_track_mutex);
}

void remote_track_stream_cancel_all(void) {
    pthread_once(&stream_once, stream_init); pthread_mutex_lock(&remote_track_mutex);
    active_generation = 0; active_path[0] = 0; memset(stream_slots, 0, sizeof(stream_slots));
    pthread_cond_broadcast(&stream_cond); pthread_mutex_unlock(&remote_track_mutex);
}

bool remote_track_stream_error_for_path(const char *path, uint64_t generation, char *out, size_t size) {
    pthread_once(&stream_once, stream_init); if (!out || !size) return false;
    pthread_mutex_lock(&remote_track_mutex); expire_slots_locked(); int i = slot_for_locked(path, generation);
    bool ok = i >= 0 && stream_slots[i].state == SLOT_FAILED && stream_slots[i].error[0];
    if (ok) snprintf(out, size, "%s", stream_slots[i].error);
    pthread_mutex_unlock(&remote_track_mutex); return ok;
}

#ifdef REMOTE_TRACK_TESTING
void remote_track_test_set_clock(uint64_t now_ms) { test_clock_ms = now_ms; test_clock_set = true; }
void remote_track_test_use_real_clock(void) { test_clock_set = false; }
void remote_track_test_set_wait_timeout_ms(unsigned int timeout_ms) { test_wait_timeout_ms = timeout_ms; }
unsigned int remote_track_test_waiter_count(void) {
    pthread_once(&stream_once, stream_init); pthread_mutex_lock(&remote_track_mutex);
    unsigned int n = test_waiters; pthread_mutex_unlock(&remote_track_mutex); return n;
}
unsigned int remote_track_test_pending_count(void) {
    pthread_once(&stream_once, stream_init); pthread_mutex_lock(&remote_track_mutex);
    unsigned int n = 0;
    for (int i = 0; i < STREAM_REQUEST_SLOTS; ++i)
        if (stream_slots[i].state == SLOT_QUEUED || stream_slots[i].state == SLOT_SENT) ++n;
    pthread_mutex_unlock(&remote_track_mutex); return n;
}
bool remote_track_test_wait_for_waiters(unsigned int count) {
    pthread_once(&stream_once, stream_init); pthread_mutex_lock(&remote_track_mutex);
    uint64_t until = cond_clock_ms() + 5000u;
    while (test_waiters < count) {
        struct timespec deadline = {(time_t)(until / 1000u), (long)(until % 1000u) * 1000000L};
        int e = pthread_cond_timedwait(&stream_cond, &remote_track_mutex, &deadline);
        if (e == ETIMEDOUT) { pthread_mutex_unlock(&remote_track_mutex); return false; }
    }
    pthread_mutex_unlock(&remote_track_mutex); return true;
}
#endif
