#include "metadata_db_internal.h"

#include "../../browser/paths/path_cache.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../remote/remote_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define METADATA_DB_DIR storage_sd_path(COMPAS_STORAGE_DIR)
#define CATALOG_INCARNATION_NAME "catalog.incarnation"
#define CATALOG_INCARNATION_TEMP_NAME "catalog.incarnation.tmp"

bool metadata_db_db_ready;
/* Bumped whenever the catalog is closed or (re)opened, under the database lock:
 * a cursor kept across lock releases is only valid within one incarnation. */
uint32_t metadata_db_db_incarnation;

static int catalog_dir_fd = -1;
char metadata_db_catalog_library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE];
static bool catalog_library_id_ready;
static bool catalog_library_id_force_rotation;
/* A database rebuilt after corruption is a provisional, empty tagcache until
 * its replacement generation commits. Never let a catalog client publish
 * that transient state under the old library identity. */
bool metadata_db_catalog_rebuild_pending;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
static pthread_mutex_t metadata_update_mutex;
_Thread_local unsigned metadata_query_depth;

#define DISPLAY_SONG_CACHE_SIZE 32
typedef struct {
    bool valid;
    song_row_t row;
} display_song_cache_entry_t;
static display_song_cache_entry_t display_song_cache[DISPLAY_SONG_CACHE_SIZE];
static size_t display_song_cache_next;
static pthread_mutex_t display_song_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds the database guard. Cache readers never acquire the database
 * mutex, so mutation may safely invalidate in DB -> cache lock order. */
static void display_song_cache_invalidate(void) {
    pthread_mutex_lock(&display_song_cache_mutex);
    memset(display_song_cache, 0, sizeof(display_song_cache));
    display_song_cache_next = 0;
    pthread_mutex_unlock(&display_song_cache_mutex);
}

static void display_song_cache_store(const song_row_t *row) {
    size_t path_len = strnlen(row->path, sizeof(row->path));
    if (path_len == 0) return;
    pthread_mutex_lock(&display_song_cache_mutex);
    size_t slot = DISPLAY_SONG_CACHE_SIZE;
    for (size_t i = 0; i < DISPLAY_SONG_CACHE_SIZE; i++) {
        if (display_song_cache[i].valid &&
            strcmp(display_song_cache[i].row.path, row->path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot == DISPLAY_SONG_CACHE_SIZE) {
        slot = display_song_cache_next;
        display_song_cache_next = (display_song_cache_next + 1) % DISPLAY_SONG_CACHE_SIZE;
    }
    display_song_cache[slot].row = *row;
    display_song_cache[slot].valid = true;
    pthread_mutex_unlock(&display_song_cache_mutex);
}

void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutex_init(&metadata_update_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void metadata_update_guard_release(metadata_db_guard_t *guard) {
    if (guard->locked) pthread_mutex_unlock(&metadata_update_mutex);
}
#define METADATA_UPDATE_GUARD \
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init); \
    pthread_mutex_lock(&metadata_update_mutex); \
    metadata_db_guard_t metadata_update_guard __attribute__((cleanup(metadata_update_guard_release))) = { true }

static void metadata_query_unlock(void) {
    for (unsigned i = 0; i < metadata_query_depth; i++) pthread_mutex_unlock(&metadata_db_mutex);
}
static void metadata_query_lock(void) {
    for (unsigned i = 0; i < metadata_query_depth; i++) pthread_mutex_lock(&metadata_db_mutex);
}

/* Non-blocking acquire of one database lock level, counted in
 * metadata_query_depth like METADATA_DB_GUARD. Pair with metadata_db_query_unlock. */
bool metadata_db_query_try_lock(void) {
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init);
    if (pthread_mutex_trylock(&metadata_db_mutex) != 0) return false;
    metadata_query_depth++;
    return true;
}

void metadata_db_query_unlock(void) {
    metadata_query_depth--;
    pthread_mutex_unlock(&metadata_db_mutex);
}

static bool catalog_pin_directory(void) {
    if (catalog_dir_fd >= 0) close(catalog_dir_fd);
    catalog_dir_fd = tagcache_dup_directory_fd();
    return catalog_dir_fd >= 0;
}

static void catalog_unpin_directory(void) {
    if (catalog_dir_fd >= 0) close(catalog_dir_fd);
    catalog_dir_fd = -1;
}

#ifndef HOST_BUILD
static bool music_root_is_mounted(void) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}
#endif

static int32_t clamp_i32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < 0) return 0;
    return (int32_t) v;
}

static void copy_tags(const tagcache_song_t * src, cached_tags_t * dst) {
    snprintf(dst->title, sizeof(dst->title), "%s", src->title);
    snprintf(dst->artist, sizeof(dst->artist), "%s", src->artist);
    snprintf(dst->album, sizeof(dst->album), "%s", src->album);
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist);
    snprintf(dst->genre, sizeof(dst->genre), "%s", src->genre);
    dst->track_number = src->track_number;
    dst->disc_number = src->disc_number;
    dst->release_year = src->release_year;
}

void metadata_db_copy_song(const tagcache_song_t * src, song_row_t * dst) {
    dst->id = src->id;
    snprintf(dst->path, sizeof(dst->path), "%s", src->path);
    copy_tags(src, &dst->tags);
    /* Do not let a truncated path become a cache key for another song. */
    if (strnlen(src->path, sizeof(dst->path)) < sizeof(dst->path))
        display_song_cache_store(dst);
}

void metadata_db_copy_group(const tagcache_group_t * src, group_row_t * dst) {
    snprintf(dst->name, sizeof(dst->name), "%s", src->name ? src->name : "");
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist ? src->album_artist : "");
    dst->song_count = src->song_count;
    dst->first_song_id = src->first_song_id;
}

static metadata_db_load_outcome_t last_outcome = METADATA_DB_LOAD_UNMOUNTED;

typedef enum {
    CATALOG_ID_READ_ERROR = -1,
    CATALOG_ID_MISSING = 0,
    CATALOG_ID_INVALID = 1,
    CATALOG_ID_VALID = 2
} catalog_id_read_result_t;

static bool catalog_id_format_valid(char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    if (strlen(id) != METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1) return false;
    for (size_t i = 0; i < METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
            continue;
        }
        unsigned char c = (unsigned char) id[i];
        if (c >= 'A' && c <= 'F') id[i] = (char) (c + ('a' - 'A'));
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static catalog_id_read_result_t catalog_read_id(char out[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    if (catalog_dir_fd < 0) return CATALOG_ID_READ_ERROR;
    int fd = openat(catalog_dir_fd, CATALOG_INCARNATION_NAME,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? CATALOG_ID_MISSING : CATALOG_ID_READ_ERROR;

    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              (st.st_size == 36 || st.st_size == 37);
    char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
    size_t needed = ok ? (size_t) st.st_size : 0;
    size_t received = 0;
    while (ok && received < needed) {
        ssize_t n = read(fd, id + received, needed - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        received += (size_t) n;
    }
    if (close(fd) != 0) ok = false;
    if (!ok) return CATALOG_ID_READ_ERROR;
    if (needed == 37) {
        if (id[36] != '\n') return CATALOG_ID_INVALID;
        id[36] = '\0';
    }
    if (!catalog_id_format_valid(id)) return CATALOG_ID_INVALID;
    memcpy(out, id, sizeof(id));
    return CATALOG_ID_VALID;
}

static bool catalog_write_id(const char * id) {
    if (catalog_dir_fd < 0) return false;
    int fd = openat(catalog_dir_fd, CATALOG_INCARNATION_TEMP_NAME,
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t length = strlen(id);
    size_t written = 0;
    bool ok = true;
    while (written < length) {
        ssize_t n = write(fd, id + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && renameat(catalog_dir_fd, CATALOG_INCARNATION_TEMP_NAME,
                       catalog_dir_fd, CATALOG_INCARNATION_NAME) != 0) ok = false;
    if (ok && fsync(catalog_dir_fd) != 0) ok = false;
    if (!ok) unlinkat(catalog_dir_fd, CATALOG_INCARNATION_TEMP_NAME, 0);
    return ok;
}

static bool catalog_generate_id(char out[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    uint8_t bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t received = 0;
    while (received < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + received, sizeof(bytes) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        received += (size_t) n;
    }
    if (close(fd) != 0) return false;
    bytes[6] = (uint8_t) ((bytes[6] & 0x0f) | 0x40); /* UUID v4 */
    bytes[8] = (uint8_t) ((bytes[8] & 0x3f) | 0x80); /* RFC 4122 variant */
    int n = snprintf(out, METADATA_DB_CATALOG_LIBRARY_ID_SIZE,
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                     bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return n == METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1;
}

/* Called only with metadata_db_mutex held. Missing/invalid identity files are
 * replaced; I/O errors fail closed so a catalog revision is never served with
 * a possibly reused identity. */
bool metadata_db_catalog_identity_prepare(void) {
    if (catalog_library_id_ready && !catalog_library_id_force_rotation) return true;
    char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
    if (!catalog_library_id_force_rotation) {
        catalog_id_read_result_t read_result = catalog_read_id(id);
        if (read_result == CATALOG_ID_READ_ERROR) return false;
        if (read_result == CATALOG_ID_VALID) {
            memcpy(metadata_db_catalog_library_id, id, sizeof(id));
            catalog_library_id_ready = true;
            return true;
        }
    }
    if (!catalog_generate_id(id) || !catalog_write_id(id)) return false;
    memcpy(metadata_db_catalog_library_id, id, sizeof(id));
    catalog_library_id_ready = true;
    catalog_library_id_force_rotation = false;
    return true;
}

static metadata_db_load_outcome_t capture_tagcache_outcome(bool update_ready) {
    switch (tagcache_get_load_outcome()) {
        case TAGCACHE_LOAD_SUCCESS_NORMAL: last_outcome = METADATA_DB_LOAD_SUCCESS_NORMAL; break;
        case TAGCACHE_LOAD_SUCCESS_FRESH: last_outcome = METADATA_DB_LOAD_SUCCESS_FRESH; break;
        case TAGCACHE_LOAD_SUCCESS_RECOVERED: last_outcome = METADATA_DB_LOAD_SUCCESS_RECOVERED; break;
        case TAGCACHE_LOAD_FAILED: last_outcome = METADATA_DB_LOAD_FAILED; break;
    }
    if (update_ready) { metadata_db_db_ready = last_outcome != METADATA_DB_LOAD_FAILED; metadata_db_db_incarnation++; metadata_db_genre_cache_free(); }
    return last_outcome;
}

static metadata_db_load_outcome_t open_database(bool read_only) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    display_song_cache_invalidate();
    if (metadata_db_db_ready) {
        return last_outcome;
    }

#ifndef HOST_BUILD
    if (!music_root_is_mounted()) {
        last_outcome = METADATA_DB_LOAD_UNMOUNTED;
        return last_outcome;
    }
#endif

    struct stat directory_stat;
    if (stat(METADATA_DB_DIR, &directory_stat) != 0 &&
        (read_only || errno != ENOENT || mkdir(METADATA_DB_DIR, 0755) != 0)) {
        last_outcome = METADATA_DB_LOAD_FAILED;
        return last_outcome;
    }
    if (!(read_only ? tagcache_open_read_only(METADATA_DB_DIR) : tagcache_open(METADATA_DB_DIR)) ||
        !catalog_pin_directory()) {
        tagcache_close();
        catalog_unpin_directory();
        last_outcome = METADATA_DB_LOAD_FAILED;
        return last_outcome;
    }
    metadata_db_load_outcome_t outcome = capture_tagcache_outcome(true);
    if (read_only) return outcome;
    if (outcome == METADATA_DB_LOAD_SUCCESS_FRESH ||
        outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        catalog_library_id_ready = false;
        catalog_library_id_force_rotation = true;
    }
    /* A fresh or recovered tagcache starts a new catalog incarnation. Older
     * installs without the marker get one stable ID on their first open. */
    metadata_db_catalog_identity_prepare();
    return outcome;
}

metadata_db_load_outcome_t metadata_db_open(void) { return open_database(false); }

metadata_db_load_outcome_t metadata_db_open_read_only(void) { return open_database(true); }

bool metadata_db_storage_current(void) {
    METADATA_DB_GUARD;
    return metadata_db_db_ready && tagcache_storage_current();
}

bool metadata_db_numeric_write_failed(void) {
    METADATA_DB_GUARD;
    return tagcache_numeric_write_failed();
}

metadata_db_snapshot_t *metadata_db_snapshot_open(bool recency) {
    METADATA_DB_GUARD;
    return metadata_db_db_ready ? tagcache_snapshot_open(recency) : NULL;
}

int metadata_db_snapshot_count(const metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_count(snapshot);
}

bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t *snapshot, int rank, char *out, size_t out_size) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_path_at(snapshot, rank, out, out_size);
}

void metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    tagcache_snapshot_close(snapshot);
}

metadata_db_snapshot_t *metadata_db_snapshot_retain(metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_retain(snapshot);
}

int metadata_db_snapshot_dup_directory_fd(const metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_dup_directory_fd(snapshot);
}

metadata_db_load_outcome_t metadata_db_reload(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    bool had_saved_db = metadata_db_db_ready &&
                        (last_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL ||
                         last_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED);
    metadata_db_close();
    metadata_db_load_outcome_t outcome = metadata_db_open();
    if (had_saved_db && outcome == METADATA_DB_LOAD_SUCCESS_FRESH) {
        metadata_db_close();
        last_outcome = METADATA_DB_LOAD_FAILED;
        metadata_db_db_ready = false;
        return last_outcome;
    }
    return outcome;
}

bool metadata_db_prepare_rebuild(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    metadata_db_load_outcome_t outcome = metadata_db_open();
    if (outcome == METADATA_DB_LOAD_SUCCESS_NORMAL ||
        outcome == METADATA_DB_LOAD_SUCCESS_FRESH ||
        outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED)
        return metadata_db_db_ready;
    if (outcome == METADATA_DB_LOAD_UNMOUNTED) return false;

    mkdir(METADATA_DB_DIR, 0755);
    bool prepared = tagcache_open_for_rebuild(METADATA_DB_DIR);
    if (prepared) prepared = catalog_pin_directory();
    if (!prepared) {
        tagcache_close();
        catalog_unpin_directory();
    }
    capture_tagcache_outcome(false);
    metadata_db_db_ready = prepared;
    metadata_db_db_incarnation++;
    metadata_db_genre_cache_free();
    if (prepared && outcome == METADATA_DB_LOAD_FAILED) {
        /* Keep catalog reads unavailable until the explicit replacement
         * generation commits and receives a fresh persistent identity. */
        catalog_library_id_ready = false;
        catalog_library_id_force_rotation = true;
        metadata_db_catalog_rebuild_pending = true;
    }
    return metadata_db_db_ready;
}

metadata_db_load_outcome_t metadata_db_get_load_outcome(void) {
    METADATA_DB_GUARD;
    return last_outcome;
}

int32_t metadata_db_get_generation(void) {
    METADATA_DB_GUARD;
    return tagcache_generation();
}

bool metadata_db_try_get_generation(int32_t * out_generation) {
    if (!out_generation) return false;
    if (!metadata_db_query_try_lock()) return false;
    *out_generation = tagcache_generation();
    metadata_db_query_unlock();
    return true;
}

bool metadata_db_try_get_catalog_library_id(char * out_id, size_t out_size) {
    if (!out_id || out_size == 0) return false;
    out_id[0] = '\0';
    if (!metadata_db_query_try_lock()) return false;
    bool available = metadata_db_db_ready && !metadata_db_catalog_rebuild_pending && catalog_library_id_ready &&
                     !catalog_library_id_force_rotation && metadata_db_catalog_library_id[0] != '\0';
    if (available) {
        size_t id_len = strnlen(metadata_db_catalog_library_id, sizeof(metadata_db_catalog_library_id));
        if (id_len >= out_size) {
            available = false;
        } else {
            memcpy(out_id, metadata_db_catalog_library_id, id_len + 1);
        }
    }
    metadata_db_query_unlock();
    if (!available) out_id[0] = '\0';
    return available;
}

/* Which Artists row a track belongs to, resolved in ONE lock scope: the name
 * is derived from the tag and looked up in the index without releasing in
 * between, so it cannot be split under one delimiter set and searched in an
 * index built from another.
 *
 * Does not wait. This runs on the UI refresh path, and a split rebuild holds
 * the lock for its whole run, so blocking here would freeze the screen for as
 * long as the rebuild takes. A highlight is cosmetic: when the lock is busy
 * this reports no row, and the refresh that follows the rebuild sets it. */
bool metadata_db_try_artist_row(const char * raw_artist, int64_t * out_offset) {
    if (!out_offset) return false;
    if (!metadata_db_query_try_lock()) return false;

    char primary[TAGCACHE_TAG_MAX];
    tagcache_artist_primary(raw_artist, primary, sizeof(primary));
    /* Safe to call while holding the lock: it takes the same recursive mutex. */
    *out_offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ARTIST, primary, NULL);

    metadata_db_query_unlock();
    return true;
}


void metadata_db_close(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    display_song_cache_invalidate();
    tagcache_close();
    catalog_unpin_directory();
    metadata_db_db_ready = false;
    metadata_db_db_incarnation++;
    metadata_db_genre_cache_free();
    last_outcome = METADATA_DB_LOAD_UNMOUNTED;
    metadata_db_catalog_library_id[0] = '\0';
    catalog_library_id_ready = false;
    catalog_library_id_force_rotation = false;
    metadata_db_catalog_rebuild_pending = false;
    path_cache_drop();
    remote_state_drop();
}

void metadata_db_begin_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return;
    display_song_cache_invalidate();
    tagcache_begin_update_with_lock(metadata_query_unlock, metadata_query_lock);
}

void metadata_db_begin_targeted_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return;
    display_song_cache_invalidate();
    tagcache_begin_targeted_update_with_lock(metadata_query_unlock, metadata_query_lock);
}

bool metadata_db_delete_target_path(const char *path) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return false;
    bool deleted = tagcache_delete_target_path(path);
    if (deleted) display_song_cache_invalidate();
    return deleted;
}

bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_lookup(path, clamp_i32(mtime), clamp_i32(size), &song)) return false;
    /* Databases written before album-order support left Rockbox's numeric
     * track/disc slots at zero. New scans persist -1 for a genuinely absent
     * tag, so zero/zero is an unambiguous one-time upgrade miss. */
    if ((song.track_number == 0 && song.disc_number == 0) || song.release_year == 0) return false;
    copy_tags(&song, out);
    return true;
}

bool metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !tags) return true;
    display_song_cache_invalidate();
    bool tags_changed = true;
    tagcache_upsert_year_changed(path, clamp_i32(mtime), clamp_i32(size), tags->title, tags->artist, tags->album,
                                 tags->album_artist, tags->genre, tags->track_number, tags->disc_number,
                                 tags->release_year, &tags_changed);
    int32_t rating = 0, playcount = 0, last_played = 0;
    if (remote_state_take(path, &rating, &playcount, &last_played))
        tagcache_overlay_stats(path, rating, playcount, last_played);
    return tags_changed;
}

bool metadata_db_end_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return false;
    bool ok = tagcache_end_update_with_lock(metadata_query_unlock, metadata_query_lock);
    display_song_cache_invalidate();
    capture_tagcache_outcome(true);
    if (ok && catalog_library_id_force_rotation && metadata_db_catalog_identity_prepare())
        metadata_db_catalog_rebuild_pending = false;
    return ok;
}

void metadata_db_abort_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return;
    tagcache_abort_update();
    display_song_cache_invalidate();
    capture_tagcache_outcome(true);
}

bool metadata_db_try_get_cached_song_by_path(const char *path, song_row_t *out_row) {
    if (!path || !out_row) return false;
    size_t path_len = strnlen(path, sizeof(out_row->path));
    if (path_len == 0 || path_len >= sizeof(out_row->path)) return false;
    if (pthread_mutex_trylock(&display_song_cache_mutex) != 0) return false;
    bool found = false;
    for (size_t i = 0; i < DISPLAY_SONG_CACHE_SIZE; i++) {
        if (display_song_cache[i].valid && strcmp(display_song_cache[i].row.path, path) == 0) {
            *out_row = display_song_cache[i].row;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&display_song_cache_mutex);
    return found;
}
