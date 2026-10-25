#include "library_scan.h"

#include "db_log.h"
#include "file_browser.h"
#include "library_artwork_internal.h"
#include "library_paths.h"
#include "library_scan_decision.h"
#include "library_scan_storage.h"
#include "library_scan_tags.h"
#include "metadata.h"
#include "metadata_refresh_targets.h"
#include "tagcache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIBRARY_SCAN_FILE_TIMEOUT_MS 5000
#define LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS 30000
#define SCAN_WALK_THREAD_STACK_SIZE (1024 * 1024)

static struct {
    library_scan_mode_t mode;
    bool allow_rebuild;
    char **target_paths;
    int target_count;
    library_scan_scope_t scope;
    char scope_name[METADATA_DB_TEXT_MAX];
    char scope_album_artist[METADATA_DB_TEXT_MAX];
    void (*books_rescan)(void);
    void (*playlists_rescan)(void);
    library_scan_result_t result;
} scan_job;

static atomic_int scan_progress_total = 0;
static atomic_int scan_progress_done = 0;
static atomic_int scan_progress_walk_items = 0;
static atomic_int scan_progress_phase = LIBRARY_SCAN_PHASE_WALK;
static atomic_bool scan_cancel_requested = false;

/* Written before parsing each song so main.c's signal handler can include the
 * current path in crash diagnostics. This stays a plain global because the
 * handler cannot safely call a getter or acquire a lock. */
char g_scan_last_path[PATH_MAX] = "";

static metadata_db_load_outcome_t combine_load_outcomes(metadata_db_load_outcome_t a,
                                                         metadata_db_load_outcome_t b) {
    if (a == METADATA_DB_LOAD_FAILED || b == METADATA_DB_LOAD_FAILED) return METADATA_DB_LOAD_FAILED;
    if (a == METADATA_DB_LOAD_UNMOUNTED || b == METADATA_DB_LOAD_UNMOUNTED) return METADATA_DB_LOAD_UNMOUNTED;
    if (a == METADATA_DB_LOAD_SUCCESS_RECOVERED || b == METADATA_DB_LOAD_SUCCESS_RECOVERED) return METADATA_DB_LOAD_SUCCESS_RECOVERED;
    if (a == METADATA_DB_LOAD_SUCCESS_NORMAL || b == METADATA_DB_LOAD_SUCCESS_NORMAL) return METADATA_DB_LOAD_SUCCESS_NORMAL;
    return METADATA_DB_LOAD_SUCCESS_FRESH;
}

static bool library_scan_open_database(bool allow_rebuild,
                                       metadata_db_load_outcome_t *out_open_outcome,
                                       bool *out_preserve_rebuild);

typedef struct {
    char root[600];
    int root_fd;
    int spool_fd;
    atomic_int refs;
    atomic_bool cancel;
    atomic_bool done;
    bool ok;
    int count;
    int skipped; /* entries the walk could not read and stepped over */
    atomic_int progress;
} scan_walk_work_t;

static void scan_walk_release(scan_walk_work_t * w) {
    if (atomic_fetch_sub_explicit(&w->refs, 1, memory_order_acq_rel) == 1) {
        close(w->root_fd);
        close(w->spool_fd);
        free(w);
    }
}

typedef struct {
    FILE *spool;
    int root_fd;
    atomic_bool *cancel;
} scan_spool_context_t;


/* Binary length-prefixed spool: paths can contain whitespace and, unlike a
 * newline-delimited temporary file, this remains correct even for odd names.
 * The spool lives on the SD card, so discovery storage scales with library
 * size without consuming the device's RAM. */
static bool scan_spool_visit_cb(const char * path, void * user) {
    scan_spool_context_t * ctx = user;
    FILE * f = ctx->spool;
    if (atomic_load_explicit(ctx->cancel, memory_order_acquire)) return false;
    char logical[PATH_MAX];
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "/proc/self/fd/%d", ctx->root_fd);
    const char * p = path;
    if (strncmp(path, prefix, strlen(prefix)) == 0) {
        snprintf(logical, sizeof(logical), "%s%s", MUSIC_ROOT_DIR, path + strlen(prefix));
        p = logical;
    }
    size_t n = strlen(p);
    if (n > UINT32_MAX) return false;
    uint32_t len = (uint32_t) n;
    return fwrite(&len, sizeof(len), 1, f) == 1 && fwrite(p, 1, n, f) == n;
}


static void * scan_walk_worker(void * arg) {
    scan_walk_work_t * w = (scan_walk_work_t *) arg;
    int writer_fd = dup(w->spool_fd);
    FILE * spool = writer_fd >= 0 ? fdopen(writer_fd, "wb") : NULL;
    if (!spool) {
        if (writer_fd >= 0) close(writer_fd);
        w->ok = false;
        w->done = true;
        atomic_store_explicit(&w->done, true, memory_order_release);
        scan_walk_release(w);
        return NULL;
    }
    scan_spool_context_t context = { spool, w->root_fd, &w->cancel };
    w->ok = file_browser_walk_all_songs_excluding_top_level(
        w->root, AUDIOBOOKS_LIBRARY_DIR_NAME, scan_spool_visit_cb, &context, &w->count, &w->progress,
        &w->skipped);
    if (fclose(spool) != 0) w->ok = false;
    atomic_store_explicit(&w->done, true, memory_order_release);
    scan_walk_release(w);
    return NULL;
}

static bool scan_all_songs_with_timeout(const char * root, int * out_spool_fd,
                                        scan_walk_work_t ** out_owner,
                                        int * out_count, int * out_skipped) {
    scan_walk_work_t * w = calloc(1, sizeof(*w));
    if (!w) return false;
    w->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->root_fd < 0) { free(w); return false; }
    snprintf(w->root, sizeof(w->root), "/proc/self/fd/%d", w->root_fd);
    w->spool_fd = openat(w->root_fd, ".compas_scan.tmp", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (w->spool_fd < 0) { close(w->root_fd); free(w); return false; }
    unlinkat(w->root_fd, ".compas_scan.tmp", 0);
    atomic_store(&w->refs, 2);
    atomic_store(&w->cancel, false);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, SCAN_WALK_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }

    pthread_t thread;
    bool created = pthread_create(&thread, attr_ptr, scan_walk_worker, w) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        close(w->spool_fd); close(w->root_fd);
        free(w);
        return false;
    }
    pthread_detach(thread);

    int last_seen_progress = 0;
    int stalled_ms = 0;
    for (;;) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            bool ok = w->ok;
            if (ok) {
                *out_spool_fd = w->spool_fd;
                *out_owner = w;
                *out_count = w->count;
                if (out_skipped) *out_skipped = w->skipped;
            } else {
                /* The worker has already released its owner on done. */
            }
            if (!ok) scan_walk_release(w);
            return ok;
        }
        if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire))
            atomic_store_explicit(&w->cancel, true, memory_order_release);
        int progress = w->progress;
        scan_progress_walk_items = progress;
        if (progress != last_seen_progress) {
            last_seen_progress = progress;
            stalled_ms = 0;
        } else {
            stalled_ms += 20;
            if (stalled_ms >= LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS) break;
        }
        usleep(20000);
    }

    /* The worker may be in uninterruptible I/O. Do not touch/free its state.
     * Its uniquely named spool can be orphaned safely and removed on a later
     * maintenance pass; critically, it owns no tagcache lock or GUI memory. */
    fprintf(stderr, "Warning: scan of %s stalled with no progress for %ds (possible filesystem corruption)\n",
            root, LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS / 1000);
    atomic_store_explicit(&w->cancel, true, memory_order_release);
    scan_walk_release(w);
    return false;
}

static bool scan_spool_read_path(FILE * f, char * path, size_t path_size) {
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    if (len == 0 || len >= path_size) return false;
    if (fread(path, 1, len, f) != len) return false;
    if (memchr(path, '\0', len) != NULL) return false;
    path[len] = '\0';
    return true;
}

/* Prove that discovery still describes the complete unchanged library before
 * entering the staging path. Any malformed or ambiguous spool data falls
 * through to the normal update so the existing recovery semantics remain. */
static bool library_scan_unchanged_fast_path(int spool_fd, int root_fd, int discovered_count,
                                              bool force_metadata) {
    if (force_metadata || !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT) ||
        !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR) ||
        !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR) ||
        lseek(spool_fd, 0, SEEK_SET) < 0) return false;
    int read_fd = dup(spool_fd);
    FILE * spool = read_fd >= 0 ? fdopen(read_fd, "rb") : NULL;
    if (!spool && read_fd >= 0) close(read_fd);
    if (!spool) return false;

    bool unchanged = true;
    char path[PATH_MAX];
    for (int i = 0; i < discovered_count; i++) {
        struct stat st;
        cached_tags_t cached;
        char actual[PATH_MAX];
        size_t root_len = strlen(MUSIC_ROOT_DIR);
        if (!scan_spool_read_path(spool, path, sizeof(path)) || path[0] == '\0' ||
            strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 ||
            (path[root_len] != '/' && path[root_len] != '\0') ||
            snprintf(actual, sizeof(actual), "/proc/self/fd/%d%s", root_fd, path + root_len) >= (int) sizeof(actual) ||
            stat(actual, &st) != 0 || !metadata_db_storage_current() ||
            atomic_load_explicit(&scan_cancel_requested, memory_order_acquire) ||
            !metadata_db_get(path, (int64_t) st.st_mtime, (int64_t) st.st_size, &cached)) {
            unchanged = false;
            break;
        }
    }

    /* The count must describe the whole spool, including no trailing record
     * or read error, before it can authorize skipping the update. */
    if (unchanged && (fgetc(spool) != EOF || ferror(spool))) unchanged = false;
    if (fclose(spool) != 0) unchanged = false;
    if (!unchanged) return false;
    scan_progress_done = discovered_count;
    scan_progress_phase = LIBRARY_SCAN_PHASE_SAVE;
    scan_job.result.succeeded = true;
    scan_job.result.incomplete = false;
    scan_job.result.saved = false;
    DB_LOG("DB", "update_unchanged files=%d rss_kb=%ld", discovered_count, db_log_rss_kb());
    return true;
}

typedef enum {
    SCAN_SONG_CACHED,   /* metadata_db_get() hit -- no parse attempted */
    SCAN_SONG_PARSED,   /* isolated read + DB upsert succeeded */
    SCAN_SONG_FAILED,   /* metadata_read_isolated() failed -- isolated child timeout/crash/malformed input */
    SCAN_SONG_NO_STAT,  /* stat() failed -- metadata was parsed but never upserted (no mtime/size to key the row by) */
} scan_song_result_t;

static int compare_scan_target_paths(const void * a, const void * b) {
    return strcmp(*(char * const *) a, *(char * const *) b);
}

static int library_refresh_scope_song_page(int offset, song_row_t * rows, int count) {
    switch (scan_job.scope) {
        case LIBRARY_SCAN_SCOPE_ALBUM:
            return metadata_db_get_album_songs(scan_job.scope_name,
                scan_job.scope_album_artist, offset, rows, count);
        case LIBRARY_SCAN_SCOPE_ARTIST:
            return metadata_db_get_artist_songs(scan_job.scope_name, offset, rows, count);
        case LIBRARY_SCAN_SCOPE_ALBUM_ARTIST:
            return metadata_db_get_album_artist_songs(scan_job.scope_name, offset, rows, count);
        case LIBRARY_SCAN_SCOPE_NONE:
        case LIBRARY_SCAN_SCOPE_PATHS:
            return 0;
    }
    return 0;
}

static bool library_scan_collect_refresh_scope(void) {
    enum { PAGE_SIZE = 64 };
    song_row_t rows[PAGE_SIZE];
    char ** paths = NULL;
    int count = 0, capacity = 0;
    for (int offset = 0; ; ) {
        int got = library_refresh_scope_song_page(offset, rows, PAGE_SIZE);
        if (got <= 0) break;
        if (count > INT_MAX - got) goto fail;
        int needed = count + got;
        if (needed > capacity) {
            int grown_capacity = capacity ? capacity : PAGE_SIZE;
            while (grown_capacity < needed) {
                if (grown_capacity > INT_MAX / 2) { grown_capacity = needed; break; }
                grown_capacity *= 2;
            }
            char ** grown = realloc(paths, sizeof(*paths) * (size_t) grown_capacity);
            if (!grown) goto fail;
            paths = grown;
            capacity = grown_capacity;
        }
        for (int i = 0; i < got; i++) {
            paths[count] = strdup(rows[i].path);
            if (!paths[count]) goto fail;
            count++;
        }
        if (got < PAGE_SIZE) break;
        if (offset > INT_MAX - got) goto fail;
        offset += got;
    }
    if (count > 1) qsort(paths, (size_t) count, sizeof(*paths), compare_scan_target_paths);
    int unique = 0;
    for (int i = 0; i < count; i++) {
        if (unique > 0 && strcmp(paths[unique - 1], paths[i]) == 0) {
            free(paths[i]);
            continue;
        }
        paths[unique++] = paths[i];
    }
    scan_job.target_paths = paths;
    scan_job.target_count = unique;
    return true;
fail:
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    return false;
}

static scan_song_result_t scan_song_stat_into_db(const char * path, const char * db_path,
                                                   const struct stat * source_stat,
                                                   bool force_metadata, bool * out_changed,
                                                   bool * out_identity_changed, bool * out_inserted) {
    if (out_changed) *out_changed = false;
    if (out_identity_changed) *out_identity_changed = false;
    if (out_inserted) *out_inserted = false;
    bool have_stat = source_stat != NULL;
    int64_t mtime = have_stat ? (int64_t) source_stat->st_mtime : 0;
    int64_t size = have_stat ? (int64_t) source_stat->st_size : 0;

    song_row_t previous = {0};
    bool had_previous = force_metadata && metadata_db_get_song_by_path(db_path, &previous);
    /* Only a path stat() just found counts as present on the card. */
    if (had_previous && have_stat) scan_job.result.matched_existing++;

    /* Record breadcrumb path prior to reading metadata so crash diagnostics
     * can report the specific file being parsed if an unhandled signal occurs. */
    snprintf(g_scan_last_path, sizeof(g_scan_last_path), "%s", path);

    track_metadata_t meta;
    if (!metadata_read_isolated(path, &meta, LIBRARY_SCAN_FILE_TIMEOUT_MS)) return SCAN_SONG_FAILED;

    cached_tags_t fresh;
    library_scan_tags_from_track(&meta, &fresh);
    free(meta.picture_data);
    free(meta.lyrics); /* always NULL here (metadata_read_isolated() itself already frees/NULLs it before the pipe write), freeing defensively for symmetry */

    if (!have_stat) return SCAN_SONG_NO_STAT;
    /* A forced refresh only writes rows whose tags differ. Leaving same-tag
     * rows untouched also leaves a changed mtime/size stale; the next ordinary
     * scan misses that old cache key and reads the file once. A parse that
     * differs only in a spelling tagcache interns back to the stored one is
     * written but reported unchanged by tagcache, so it cannot force a commit. */
    bool exact_match = force_metadata && had_previous && library_scan_tags_equal(&previous.tags, &fresh);
    bool stored_changed = !exact_match && metadata_db_put(db_path, mtime, size, &fresh);
    if (force_metadata) {
        if (out_changed) *out_changed = had_previous && stored_changed;
        if (out_inserted) *out_inserted = !had_previous;
        if (out_identity_changed) {
            /* Judged from the parse: a spelling tagcache interns back can only
             * over-report here, and only for a row that changed anyway. */
            song_row_t refreshed = {0};
            size_t path_len = strlen(db_path);
            if (path_len >= sizeof(refreshed.path)) path_len = sizeof(refreshed.path) - 1;
            memcpy(refreshed.path, db_path, path_len);
            refreshed.path[path_len] = '\0';
            refreshed.tags = fresh;
            *out_identity_changed = !had_previous ||
                (stored_changed && (strcmp(previous.tags.artist, fresh.artist) != 0 ||
                                    song_album_thumbnail_key(&previous) != song_album_thumbnail_key(&refreshed)));
        }
    }
    return SCAN_SONG_PARSED;
}

static scan_song_result_t scan_one_song_into_db(const char * path, const char * db_path,
                                                 bool force_metadata, bool * out_changed,
                                                 bool * out_identity_changed, bool * out_inserted) {
    if (out_changed) *out_changed = false;
    if (out_identity_changed) *out_identity_changed = false;
    if (out_inserted) *out_inserted = false;
    struct stat st;
    bool have_stat = stat(path, &st) == 0;
    int64_t mtime = have_stat ? (int64_t) st.st_mtime : 0;
    int64_t size = have_stat ? (int64_t) st.st_size : 0;
    cached_tags_t cached;
    if (!force_metadata && have_stat && metadata_db_get(db_path, mtime, size, &cached)) return SCAN_SONG_CACHED;
    return scan_song_stat_into_db(path, db_path, have_stat ? &st : NULL, force_metadata,
                                  out_changed, out_identity_changed, out_inserted);
}

static bool library_scan_target_actual_path(const char * path, int root_fd,
                                            char * actual_path, size_t actual_path_size) {
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (!path || strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 || path[root_len] != '/') return false;
    const char * relative = path + root_len + 1;
    for (const char * component = relative; ; ) {
        const char * slash = strchr(component, '/');
        size_t length = slash ? (size_t) (slash - component) : strlen(component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) return false;
        if (!slash) break;
        component = slash + 1;
    }
    int n = snprintf(actual_path, actual_path_size, "/proc/self/fd/%d/%s", root_fd, relative);
    return n > 0 && (size_t) n < actual_path_size;
}

static void library_scan_targeted_once(void) {
    scan_job.result.refreshed_count = 0;
    scan_job.result.changed_count = 0;
    scan_job.result.failed_count = 0;
    scan_job.result.matched_existing = 0;
    scan_job.result.identity_changed = false;
    scan_job.result.succeeded = false;
    scan_job.result.incomplete = false;
    scan_job.result.load_blocked = false;
    scan_job.result.saved = false;
    scan_job.result.published = false;
    scan_progress_done = 0;
    scan_progress_total = scan_job.scope == LIBRARY_SCAN_SCOPE_PATHS
                                      ? scan_job.target_count : 0;
    scan_progress_walk_items = 0;
    scan_progress_phase = scan_job.scope == LIBRARY_SCAN_SCOPE_PATHS
                             ? LIBRARY_SCAN_PHASE_TAGS : LIBRARY_SCAN_PHASE_WALK;
    bool db_logging = db_log_enabled();
    uint64_t scan_started_ms = db_logging ? db_log_now_ms() : 0;

    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) return;

    DB_LOG("DB", "targeted_refresh_begin targets=%d rss_kb=%ld",
           scan_job.target_count, db_log_rss_kb());
    bool preserve_rebuild = false;
    if (!library_scan_open_database(scan_job.allow_rebuild, NULL,
                                    &preserve_rebuild)) return;
    if (preserve_rebuild) {
        /* A preserved rebuild is a provisional empty cache. A partial update
         * must never publish only the selected rows as a replacement library. */
        scan_job.result.load_blocked = true;
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }
    if (!sd_card_root_is_mounted() || !metadata_db_storage_current()) {
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
        return;
    }
    if (scan_job.scope != LIBRARY_SCAN_SCOPE_NONE &&
        scan_job.scope != LIBRARY_SCAN_SCOPE_PATHS &&
        !library_scan_collect_refresh_scope()) {
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }

    int target_count = scan_job.target_count;
    scan_progress_total = target_count;
    scan_progress_done = 0;
    scan_progress_phase = LIBRARY_SCAN_PHASE_TAGS;
    if (target_count <= 0) {
        scan_job.result.succeeded = true;
        return;
    }

    int root_fd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome,
            sd_card_root_is_mounted() ? METADATA_DB_LOAD_FAILED : METADATA_DB_LOAD_UNMOUNTED);
        return;
    }

    metadata_db_begin_targeted_update();
    bool update_ok = true;
    bool storage_lost = false;
    bool target_deleted = false;
    bool row_inserted = false;
    char actual_path[PATH_MAX];
    for (int done = 0; done < target_count; done++) {
        if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) break;
        if (!sd_card_root_is_mounted() || !metadata_db_storage_current()) {
            update_ok = false;
            storage_lost = true;
            break;
        }
        const char * path = scan_job.target_paths[done];
        if (!library_scan_target_actual_path(path, root_fd, actual_path, sizeof(actual_path))) {
            update_ok = false;
            break;
        }

        struct stat st;
        if (stat(actual_path, &st) != 0) {
            if (errno == ENOENT) {
                song_row_t removed_song;
                bool had_previous = metadata_db_get_song_by_path(path, &removed_song);
                if (!metadata_db_delete_target_path(path)) {
                    update_ok = false;
                    break;
                }
                if (had_previous) {
                    target_deleted = true;
                    scan_job.result.identity_changed = true;
                }
            } else {
                scan_job.result.incomplete = true;
            }
        } else {
            uint64_t file_started_ms = db_logging ? db_log_now_ms() : 0;
            bool tags_changed = false, identity_changed = false, inserted = false;
            scan_song_result_t result = scan_song_stat_into_db(actual_path, path, &st, true,
                                                               &tags_changed, &identity_changed, &inserted);
            if (result == SCAN_SONG_PARSED) {
                scan_job.result.refreshed_count++;
                if (tags_changed) scan_job.result.changed_count++;
                if (inserted) row_inserted = true;
                if (identity_changed) scan_job.result.identity_changed = true;
            } else if (result == SCAN_SONG_FAILED) {
                scan_job.result.incomplete = true;
                scan_job.result.failed_count++;
            }
            if (db_logging) {
                uint64_t file_ms = db_log_now_ms() - file_started_ms;
                static const char * const scan_song_result_name[] = { "cached", "parsed", "failed", "no_stat" };
                DB_LOG("DB_FILE", "target_end index=%d result=%s elapsed_ms=%llu path=%s", done,
                       scan_song_result_name[result], (unsigned long long) file_ms, path);
            }
        }
        scan_progress_done = done + 1;
    }
    close(root_fd);

    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) {
        metadata_db_abort_update();
        return;
    }
    if (!update_ok || storage_lost || !sd_card_root_is_mounted() || !metadata_db_storage_current()) {
        metadata_db_abort_update();
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome,
            storage_lost || !sd_card_root_is_mounted() || !metadata_db_storage_current()
                ? METADATA_DB_LOAD_UNMOUNTED : METADATA_DB_LOAD_FAILED);
        return;
    }

    if (metadata_refresh_should_skip_commit(scan_job.result.changed_count, target_deleted, row_inserted)) {
        int32_t generation_before = tagcache_generation();
        metadata_db_abort_update();
        scan_job.result.published = tagcache_generation() != generation_before;
        scan_job.result.saved = false;
        scan_job.result.succeeded = true;
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome,
                                                           metadata_db_get_load_outcome());
        library_scan_reset_progress();
        DB_LOG("DB", "targeted_refresh_end refreshed=%d changed=%d ok=1 published=%d no_op=1 elapsed_ms=%llu songs=%lld rss_kb=%ld",
               scan_job.result.refreshed_count, scan_job.result.changed_count, scan_job.result.published,
               (unsigned long long) (db_log_now_ms() - scan_started_ms),
               (long long) metadata_db_get_song_count(), db_log_rss_kb());
        return;
    }

    scan_progress_phase = LIBRARY_SCAN_PHASE_SAVE;
    int32_t generation_before = tagcache_generation();
    bool committed = metadata_db_end_update();
    scan_job.result.published = tagcache_generation() != generation_before;
    scan_job.result.saved = committed && !preserve_rebuild;
    scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, metadata_db_get_load_outcome());
    if (!committed)
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
    scan_job.result.succeeded = committed;

    library_scan_reset_progress();
    metadata_db_load_outcome_t reload_outcome = metadata_db_storage_current()
        ? metadata_db_get_load_outcome() : METADATA_DB_LOAD_UNMOUNTED;
    scan_job.result.saved = scan_job.result.saved && reload_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL;
    scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, reload_outcome);
    DB_LOG("DB", "targeted_refresh_end refreshed=%d changed=%d ok=%d elapsed_ms=%llu songs=%lld rss_kb=%ld",
           scan_job.result.refreshed_count, scan_job.result.changed_count, committed,
           (unsigned long long) (db_log_now_ms() - scan_started_ms),
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
}

static bool library_scan_open_database(bool allow_rebuild, metadata_db_load_outcome_t * out_open_outcome,
                                       bool * out_preserve_rebuild) {
    metadata_db_load_outcome_t open_outcome = metadata_db_open();
    bool preserve_rebuild = false;
    if (open_outcome == METADATA_DB_LOAD_FAILED && allow_rebuild) {
        bool prepared = metadata_db_prepare_rebuild();
        open_outcome = metadata_db_get_load_outcome();
        preserve_rebuild = prepared && open_outcome == METADATA_DB_LOAD_FAILED;
        if (preserve_rebuild)
            open_outcome = METADATA_DB_LOAD_SUCCESS_NORMAL;
    }
    if (out_open_outcome) *out_open_outcome = open_outcome;
    if (out_preserve_rebuild) *out_preserve_rebuild = preserve_rebuild;
    scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, open_outcome);
    if (scan_job.result.operation_outcome == METADATA_DB_LOAD_FAILED ||
        scan_job.result.operation_outcome == METADATA_DB_LOAD_UNMOUNTED) {
        scan_job.result.load_blocked = !allow_rebuild;
        return false;
    }
    if (!allow_rebuild && open_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        scan_job.result.load_blocked = true;
        return false;
    }
    return true;
}

static void library_scan_once_with_options(bool force_all_metadata) {
    scan_job.result.published = false;
    scan_job.result.refreshed_count = 0;
    scan_job.result.changed_count = 0;
    scan_job.result.failed_count = 0;
    scan_job.result.matched_existing = 0;
    scan_job.result.identity_changed = false;
    scan_job.result.succeeded = false;
    scan_job.result.incomplete = false;
    scan_job.result.load_blocked = false;
    scan_job.result.saved = false;
    bool db_logging = db_log_enabled();
    uint64_t scan_started_ms = db_logging ? db_log_now_ms() : 0;
    uint64_t phase_started_ms = scan_started_ms;
    scan_progress_done = 0;
    scan_progress_total = 0;
    scan_progress_walk_items = 0;
    scan_progress_phase = LIBRARY_SCAN_PHASE_WALK;

    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) return;

    DB_LOG("DB", "scan_begin root=%s rss_kb=%ld", MUSIC_ROOT_DIR, db_log_rss_kb());
    metadata_db_load_outcome_t open_outcome;
    bool preserve_rebuild = false;
    if (!library_scan_open_database(scan_job.allow_rebuild,
                                    &open_outcome, &preserve_rebuild)) return;
    int64_t song_count_before_scan = metadata_db_get_song_count();
    DB_LOG("DB", "db_open elapsed_ms=%llu songs=%lld rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), (long long) song_count_before_scan,
           db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    scan_job.books_rescan();
    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) return;
    DB_LOG("DB", "books_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    scan_job.playlists_rescan();
    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) return;
    DB_LOG("DB", "playlists_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());

    /* An unmounted card leaves the mount point readable and empty, which walks
     * cleanly to zero files. */
    if (!sd_card_root_is_mounted()) {
        DB_LOG("DB", "discover_skipped reason=not_mounted rss_kb=%ld", db_log_rss_kb());
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
        return;
    }

    int spool_fd = -1;
    scan_walk_work_t * spool_owner = NULL;
    int discovered_count = 0;
    int skipped_count = 0;
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    if (!scan_all_songs_with_timeout(MUSIC_ROOT_DIR, &spool_fd, &spool_owner, &discovered_count,
                                     &skipped_count)) {
        DB_LOG("DB", "discover_failed elapsed_ms=%llu rss_kb=%ld",
               (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }
    if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) {
        scan_walk_release(spool_owner);
        metadata_db_abort_update();
        return;
    }
    DB_LOG("DB", "discover_end files=%d skipped=%d elapsed_ms=%llu spool=%s rss_kb=%ld",
           discovered_count, skipped_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           "fd-backed", db_log_rss_kb());

    if (library_scan_rebuild_rejected(preserve_rebuild, discovered_count)) {
        DB_LOG("DB", "rebuild_rejected reason=no_files");
        metadata_db_abort_update();
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        scan_walk_release(spool_owner);
        return;
    }

    scan_progress_total = discovered_count;
    scan_progress_phase = LIBRARY_SCAN_PHASE_TAGS;
    bool force_metadata_anywhere = force_all_metadata;
    if (library_scan_may_skip_update(open_outcome, preserve_rebuild, skipped_count, discovered_count,
                                     metadata_db_get_song_count()) &&
        library_scan_unchanged_fast_path(spool_fd, spool_owner->root_fd, discovered_count,
                                         force_metadata_anywhere)) {
        scan_walk_release(spool_owner);
        return;
    }

    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    metadata_db_begin_update();
    DB_LOG("DB", "update_begin files=%d rss_kb=%ld", discovered_count, db_log_rss_kb());

    if (lseek(spool_fd, 0, SEEK_SET) < 0) {
        metadata_db_abort_update();
        scan_walk_release(spool_owner);
        return;
    }
    int spool_read_fd = dup(spool_fd);
    FILE * spool = spool_read_fd >= 0 ? fdopen(spool_read_fd, "rb") : NULL;
    if (!spool) {
        if (spool_read_fd >= 0) close(spool_read_fd);
        DB_LOG("DB", "spool_open_failed rss_kb=%ld", db_log_rss_kb());
        metadata_db_abort_update();
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, metadata_db_get_load_outcome());
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        scan_walk_release(spool_owner);
        return;
    }

    bool complete = true;
    bool row_inserted = false;
    char path[PATH_MAX];
    int done = 0;
    /* Cached once, not read fresh every song: db_log_now_ms() is a real
     * clock_gettime() syscall on this target (no vDSO), and DB_LOG's own
     * internal enabled-check can't save that cost since its arguments (the
     * elapsed-time expressions below) are already evaluated before the call
     * is even made. Skipping db_log_now_ms()/DB_LOG entirely when off is the
     * only way a 10,000+ song scan doesn't pay two syscalls per file even
     * with logging disabled (the default). */
    while (done < discovered_count) {
        if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) {
            complete = false;
            break;
        }
        if (!scan_spool_read_path(spool, path, sizeof(path))) {
            complete = false;
            break;
        }
        if (path[0] == '\0') {
            complete = false;
            break;
        }
        if (path[0] != '\0') {
            if (!metadata_db_storage_current() || atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) {
                complete = false;
                break;
            }
            char actual_path[PATH_MAX];
            size_t root_len = strlen(MUSIC_ROOT_DIR);
            if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 ||
                (path[root_len] != '/' && path[root_len] != '\0') ||
                snprintf(actual_path, sizeof(actual_path), "/proc/self/fd/%d%s", spool_owner->root_fd,
                         path + root_len) >= (int) sizeof(actual_path)) {
                complete = false;
                break;
            }
            uint64_t file_started_ms = db_logging ? db_log_now_ms() : 0;
            /* No rss_kb here -- see db_log_rss_kb()'s own comment; RSS is
             * only worth the /proc read at phase boundaries, progress
             * intervals, slow files, and failures, not on every song. Plain
             * buffered logging, no per-file flush -- crash forensics for the
             * parent are already covered by g_scan_last_path (set inside
             * scan_one_song_into_db() itself, see its own comment) plus
             * main.c's crash handler, which fsyncs reload_diag.log; the
             * actual metadata parse also runs in an isolated child process
             * (metadata_read_isolated()), so a malformed file yields
             * SCAN_SONG_FAILED below rather than crashing this process. */
            if (db_logging) DB_LOG("DB_FILE", "file_begin index=%d total=%d path=%s", done, discovered_count, path);
            bool tags_changed = false, identity_changed = false, inserted = false;
            scan_song_result_t result = scan_one_song_into_db(actual_path, path, force_all_metadata,
                                                               force_all_metadata ? &tags_changed : NULL,
                                                               force_all_metadata ? &identity_changed : NULL,
                                                               force_all_metadata ? &inserted : NULL);
            /* NO_STAT: the file was parsed but its tags were discarded because
             * stat() failed, so the song was not refreshed either. */
            if (force_all_metadata && (result == SCAN_SONG_FAILED || result == SCAN_SONG_NO_STAT))
                scan_job.result.failed_count++;
            if (force_all_metadata && result == SCAN_SONG_PARSED) {
                scan_job.result.refreshed_count++;
                if (tags_changed) scan_job.result.changed_count++;
                if (inserted) row_inserted = true;
                if (identity_changed) scan_job.result.identity_changed = true;
            }
            if (db_logging) {
                uint64_t file_ms = db_log_now_ms() - file_started_ms;
                static const char * const scan_song_result_name[] = { "cached", "parsed", "failed", "no_stat" };
                DB_LOG("DB_FILE", "file_end index=%d result=%s elapsed_ms=%llu path=%s", done,
                       scan_song_result_name[result], (unsigned long long) file_ms, path);
                if (file_ms >= 250)
                    DB_LOG("DB", "slow_file index=%d result=%s elapsed_ms=%llu rss_kb=%ld path=%s", done,
                           scan_song_result_name[result], (unsigned long long) file_ms, db_log_rss_kb(), path);
            }
        }
        scan_progress_done = ++done;
        if ((done % 1000) == 0 || done == discovered_count)
            DB_LOG("DB", "tag_progress done=%d total=%d elapsed_ms=%llu rss_kb=%ld",
                   done, discovered_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
                   db_log_rss_kb());
    }
    if (complete && (fgetc(spool) != EOF || ferror(spool))) complete = false;
    fclose(spool);
    scan_walk_release(spool_owner);

    if (!metadata_db_storage_current() || atomic_load_explicit(&scan_cancel_requested, memory_order_acquire))
        complete = false;

    if (!complete) {
        DB_LOG("DB", "spool_read_incomplete done=%d total=%d rss_kb=%ld", done, discovered_count, db_log_rss_kb());
        metadata_db_abort_update();
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, metadata_db_get_load_outcome());
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }

    DB_LOG("DB", "update_end skipped=%d", skipped_count);
    if (!metadata_db_storage_current() || atomic_load_explicit(&scan_cancel_requested, memory_order_acquire)) {
        metadata_db_abort_update();
        return;
    }
    /* The walk already stat()ed every file on the card. With nothing
     * inserted and no folder skipped, a row is missing exactly when fewer
     * found files matched a row than the (guarded) live count; otherwise the
     * mismatch over-reports. A missing row may be deleted at commit, taking
     * a song (and maybe its cover source) out of an album, so the publication
     * treats it as an identity change. */
    bool missing_paths = force_all_metadata &&
                         (int64_t) scan_job.result.matched_existing != metadata_db_get_song_count();
    if (missing_paths) scan_job.result.identity_changed = true;
    if (library_scan_refresh_skips_commit(force_all_metadata, preserve_rebuild, skipped_count,
                                          scan_job.result.changed_count, row_inserted, false)) {
        if (atomic_load_explicit(&scan_cancel_requested, memory_order_acquire) ||
            !sd_card_root_is_mounted() || !metadata_db_storage_current()) {
            metadata_db_abort_update();
            if (!sd_card_root_is_mounted() || !metadata_db_storage_current())
                scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
            return;
        }
        if (!missing_paths) {
            int32_t generation_before = tagcache_generation();
            metadata_db_abort_update();
            scan_job.result.published = tagcache_generation() != generation_before;
            scan_job.result.saved = false;
            scan_job.result.succeeded = true;
            scan_job.result.incomplete = false;
            scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome,
                                                               metadata_db_get_load_outcome());
            library_scan_reset_progress();
            DB_LOG("DB", "refresh_all_end refreshed=%d changed=%d ok=1 published=%d no_op=1 elapsed_ms=%llu songs=%lld rss_kb=%ld",
                   scan_job.result.refreshed_count, scan_job.result.changed_count, scan_job.result.published,
                   (unsigned long long) (db_log_now_ms() - scan_started_ms),
                   (long long) metadata_db_get_song_count(), db_log_rss_kb());
            return;
        }
    }

    scan_progress_phase = LIBRARY_SCAN_PHASE_SAVE;
    int32_t generation_before = tagcache_generation();
    bool committed = metadata_db_end_update();
    scan_job.result.published = tagcache_generation() != generation_before;
    metadata_db_load_outcome_t commit_outcome = metadata_db_get_load_outcome();
    scan_job.result.saved = committed && !preserve_rebuild;
    scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, commit_outcome);
    if (!committed)
        scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, METADATA_DB_LOAD_FAILED);
    scan_job.result.succeeded = committed;
    scan_job.result.incomplete = committed && skipped_count > 0;
    if (!committed)
        fprintf(stderr, "Warning: music database commit failed -- keeping last on-disk library\n");
    DB_LOG("DB", "commit_end ok=%d files=%d previous_songs=%lld elapsed_ms=%llu songs=%lld rss_kb=%ld", committed,
           done, (long long) song_count_before_scan, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (long long) metadata_db_get_song_count(), db_log_rss_kb());

    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    library_scan_reset_progress();
    metadata_db_load_outcome_t reload_outcome = metadata_db_storage_current()
        ? metadata_db_get_load_outcome() : METADATA_DB_LOAD_UNMOUNTED;
    scan_job.result.saved = scan_job.result.saved && reload_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL;
    scan_job.result.operation_outcome = combine_load_outcomes(scan_job.result.operation_outcome, reload_outcome);
    DB_LOG("DB", "reload_end songs=%lld elapsed_ms=%llu total_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (unsigned long long) (db_log_now_ms() - scan_started_ms), db_log_rss_kb());
}

void library_scan_prepare(const library_scan_request_t *request) {
    if (!request) return;
    scan_job.mode = request->mode;
    scan_job.allow_rebuild = request->allow_rebuild;
    scan_job.target_paths = request->target_paths;
    scan_job.target_count = request->target_count;
    scan_job.scope = request->scope;
    snprintf(scan_job.scope_name, sizeof(scan_job.scope_name), "%s", request->scope_name ? request->scope_name : "");
    snprintf(scan_job.scope_album_artist, sizeof(scan_job.scope_album_artist), "%s",
             request->scope_album_artist ? request->scope_album_artist : "");
    scan_job.books_rescan = request->books_rescan;
    scan_job.playlists_rescan = request->playlists_rescan;
    scan_job.result = (library_scan_result_t){
        .operation_outcome = request->outcome_seed
    };
}

void library_scan_run(void) {
    switch (scan_job.mode) {
        case LIBRARY_SCAN_MODE_FORCE_METADATA:
            library_scan_once_with_options(true);
            break;
        case LIBRARY_SCAN_MODE_TARGETED:
            library_scan_targeted_once();
            break;
        case LIBRARY_SCAN_MODE_NORMAL:
        default:
            library_scan_once_with_options(false);
            break;
    }
}

void library_scan_run_full_current(void (*books_rescan)(void),
                                   void (*playlists_rescan)(void)) {
    scan_job.books_rescan = books_rescan;
    scan_job.playlists_rescan = playlists_rescan;
    library_scan_once_with_options(false);
}

void library_scan_record_start_failure(void) {
    scan_job.result.operation_outcome = METADATA_DB_LOAD_FAILED;
}

void library_scan_release(void) {
    for (int i = 0; i < scan_job.target_count; i++) free(scan_job.target_paths[i]);
    free(scan_job.target_paths);
    scan_job.target_paths = NULL;
    scan_job.target_count = 0;
    scan_job.scope = LIBRARY_SCAN_SCOPE_NONE;
    scan_job.scope_name[0] = '\0';
    scan_job.scope_album_artist[0] = '\0';
}

void library_scan_get_result(library_scan_result_t *out) {
    if (out) *out = scan_job.result;
}

void library_scan_get_progress(library_scan_progress_t *out) {
    if (!out) return;
    out->total = atomic_load(&scan_progress_total);
    out->done = atomic_load(&scan_progress_done);
    out->phase = atomic_load(&scan_progress_phase);
    out->walk_items = atomic_load(&scan_progress_walk_items);
}

void library_scan_reset_progress(void) {
    scan_progress_done = 0;
    scan_progress_total = 0;
    scan_progress_walk_items = 0;
}

void library_scan_request_cancel(void) {
    atomic_store_explicit(&scan_cancel_requested, true, memory_order_release);
}

void library_scan_reset_cancel(void) {
    atomic_store_explicit(&scan_cancel_requested, false, memory_order_release);
}

bool library_scan_cancel_pending(void) {
    return atomic_load_explicit(&scan_cancel_requested, memory_order_acquire);
}
