#include "../sdtool.h"
#include "../output/json_out.h"
#include "../../../library/metadata/scan/library_scan_decision.h"
#include "../../../library/metadata/scan/library_scan_tags.h"
#include "../parse/parse_pool.h"
#include "../../../library/metadata/scan/library_walk.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../library/tagcache/core/tagcache.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The player scans from its UI thread through a spooled path list; this is the
 * same decision procedure on the shared primitives (walk, metadata_db_get/put,
 * tag mapping). An unchanged library is proven with reads alone and writes
 * nothing; otherwise the database is staged, changed files are read and the
 * generation committed. */

typedef struct {
    char **paths; /* files found, as the computer names them */
    long count;
    long capacity;
} file_list_t;

typedef struct {
    long parsed;
    long changed;          /* --full: rows whose tags differ */
    long failed;
    long broken;           /* failed files whose worker left silently */
    long matched_existing; /* --full: files that already had a row */
    bool inserted;
} scan_result_t;

static bool list_add(const char *host, void *user) {
    file_list_t *list = user;
    if (list->count == list->capacity) {
        long capacity = list->capacity ? list->capacity * 2 : 1024;
        char **grown = realloc(list->paths, sizeof(*grown) * (size_t) capacity);
        if (!grown) return false;
        list->paths = grown;
        list->capacity = capacity;
    }
    list->paths[list->count] = strdup(host);
    if (!list->paths[list->count]) return false;
    list->count++;
    json_progress("walk", list->count, 0);
    return true;
}

static void list_free(file_list_t *list) {
    for (long i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
}

/* The path the player stores for a file found at host. */
static void stored_path(const char *host, char *out, size_t size) {
    snprintf(out, size, "%s%s", SD_DEVICE_MOUNT, host + strlen(sdtool_root()));
}

static bool file_signature(const char *host, int64_t *mtime, int64_t *size) {
    struct stat st;
    if (stat(host, &st) != 0) return false;
    *mtime = (int64_t) st.st_mtime;
    *size = (int64_t) st.st_size;
    return true;
}

/* True when every file found already has an up-to-date row and no row is left
 * over: the player's unchanged fast path. */
static bool library_unchanged(const file_list_t *list) {
    if (!metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT) ||
        !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR) ||
        !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR))
        return false;
    for (long i = 0; i < list->count; i++) {
        char path[PATH_MAX];
        int64_t mtime, size;
        cached_tags_t cached;
        stored_path(list->paths[i], path, sizeof(path));
        if (!file_signature(list->paths[i], &mtime, &size) ||
            !metadata_db_get(path, mtime, size, &cached))
            return false;
        json_progress("tags", i + 1, list->count);
    }
    return true;
}

enum { PARSE_BATCH = 64 };

/* Reads the tags of the files that need it, a batch at a time, in worker
 * processes. A file the worker could not parse is failed and gets no row, so
 * an older row of it survives, as in the player. */
static bool update_batch(const file_list_t *list, long first, long count, bool full, scan_result_t *result) {
    const char *todo[PARSE_BATCH];
    static char stored[PARSE_BATCH][PATH_MAX];
    int64_t mtimes[PARSE_BATCH], sizes[PARSE_BATCH];
    static track_metadata_t metas[PARSE_BATCH];
    bool parsed[PARSE_BATCH];
    static song_row_t previous[PARSE_BATCH];
    bool had_previous[PARSE_BATCH];
    size_t wanted = 0;

    for (long i = first; i < first + count; i++) {
        char path[PATH_MAX];
        cached_tags_t cached;
        const char *host = list->paths[i];
        stored_path(host, path, sizeof(path));
        if (!file_signature(host, &mtimes[wanted], &sizes[wanted])) {
            result->failed++;
            continue;
        }
        if (!full && metadata_db_get(path, mtimes[wanted], sizes[wanted], &cached)) continue;
        snprintf(stored[wanted], PATH_MAX, "%s", path);
        had_previous[wanted] = full && metadata_db_get_song_by_path(path, &previous[wanted]);
        if (had_previous[wanted]) result->matched_existing++;
        todo[wanted++] = host;
    }
    if (wanted == 0) return true;
    size_t broken;
    if (sdtool_parse_files(todo, wanted, metas, parsed, &broken) != SDTOOL_PARSE_OK) return false;
    result->broken += (long) broken;

    for (size_t k = 0; k < wanted; k++) {
        cached_tags_t tags;
        if (!parsed[k]) {
            sdtool_log("could not read tags of %s", todo[k]);
            result->failed++;
            continue;
        }
        library_scan_tags_from_track(&metas[k], &tags);
        result->parsed++;
        /* A forced refresh writes only rows whose tags differ; leaving same-tag
         * rows alone keeps an unchanged library byte-identical. */
        if (full && had_previous[k] && library_scan_tags_equal(&previous[k].tags, &tags)) continue;
        bool stored_changed = metadata_db_put(stored[k], mtimes[k], sizes[k], &tags);
        if (full && had_previous[k] && stored_changed) result->changed++;
        if (full && !had_previous[k]) result->inserted = true;
    }
    return true;
}

static int finish(const file_list_t *list, const scan_result_t *result, int skipped, bool written,
                  bool migrated) {
    json_line_t line;
    json_begin(&line);
    json_string(&line, "event", "done");
    json_string(&line, "command", "scan");
    json_integer(&line, "files", list->count);
    json_integer(&line, "parsed", result->parsed);
    json_integer(&line, "unchanged", list->count - result->parsed - result->failed);
    json_integer(&line, "failed", result->failed);
    json_integer(&line, "skipped", skipped);
    json_integer(&line, "songs", metadata_db_get_song_count());
    json_integer(&line, "generation", metadata_db_get_generation());
    /* An index migration at open rewrites files whatever the scan then finds. */
    json_boolean(&line, "written", written || migrated);
    json_boolean(&line, "migrated", migrated);
    json_integer(&line, "mtime_offset", sdtool_mtime_extra());
    json_emit(&line);
    return SDTOOL_OK;
}

static int fail(file_list_t *list, const char *code, const char *message) {
    json_error(code, message);
    list_free(list);
    metadata_db_close();
    return SDTOOL_FAILED;
}

int sdtool_cmd_scan(const sdtool_options_t *options) {
    sdtool_database_t db;
    if (!sdtool_open_database(&db)) return SDTOOL_FAILED;
    sdtool_mtime_calibrate();
    bool migrated = tagcache_indexes_migrated();
    file_list_t list = {0};
    scan_result_t result = {0};
    int count = 0, skipped = 0;
    atomic_int progress = 0;

    json_progress("walk", 0, 0);
    /* A root that cannot be read is a void scan, not an empty library. */
    if (!file_browser_walk_all_songs_excluding_top_level(sdtool_root(), AUDIOBOOKS_LIBRARY_DIR_NAME,
                                                         list_add, &list, &count, &progress, &skipped))
        return fail(&list, "walk_failed", "the card could not be read");

    if (library_scan_rebuild_rejected(db.preserve_rebuild, (int) list.count))
        return fail(&list, "no_music_found", "the database cannot be read and no music was found to rebuild it");

    if (!options->full &&
        library_scan_may_skip_update(db.outcome, db.preserve_rebuild, skipped, (int) list.count,
                                     metadata_db_get_song_count()) &&
        library_unchanged(&list)) {
        int rc = finish(&list, &result, 0, false, migrated);
        list_free(&list);
        metadata_db_close();
        return rc;
    }
    if (list.count == 0 && metadata_db_get_song_count() == 0) {
        int rc = finish(&list, &result, skipped, false, migrated);
        list_free(&list);
        metadata_db_close();
        return rc;
    }

    metadata_db_begin_update();
    for (long first = 0; first < list.count; first += PARSE_BATCH) {
        long batch = list.count - first < PARSE_BATCH ? list.count - first : PARSE_BATCH;
        if (!update_batch(&list, first, batch, options->full, &result)) {
            metadata_db_abort_update();
            return fail(&list, "worker_failed", "the tag parser process could not be started");
        }
        json_progress("tags", first + batch, list.count);
    }

    /* Several files whose worker left without a word and nothing readable: the
     * parser is broken, not the music. Corrupt files that crash or hang it are
     * only themselves failed, as in the player. */
    if (result.broken >= 3 && result.parsed == 0) {
        metadata_db_abort_update();
        return fail(&list, "worker_failed", "no file could be read by the tag parser process");
    }

    bool missing = options->full && result.matched_existing != metadata_db_get_song_count();
    if (library_scan_refresh_skips_commit(options->full, db.preserve_rebuild, skipped, (int) result.changed,
                                          result.inserted, missing)) {
        metadata_db_abort_update();
        int rc = finish(&list, &result, skipped, false, migrated);
        list_free(&list);
        metadata_db_close();
        return rc;
    }

    json_progress("save", 0, 1);
    if (!metadata_db_end_update())
        return fail(&list, "write_failed", "the music database could not be written");
    json_progress("save", 1, 1);
    int rc = finish(&list, &result, skipped, true, migrated);
    list_free(&list);
    metadata_db_close();
    return rc;
}
