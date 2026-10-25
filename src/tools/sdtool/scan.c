#include "sdtool.h"
#include "json_out.h"
#include "library_scan_tags.h"
#include "library_walk.h"
#include "metadata.h"
#include "metadata_db.h"
#include "metadata_refresh_targets.h"
#include "storage_paths.h"
#include "tagcache.h"
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
static bool library_unchanged(const file_list_t *list, int skipped) {
    if (skipped != 0 || metadata_db_get_song_count() != list->count ||
        !metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT) ||
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

static void update_file(const char *host, bool full, scan_result_t *result) {
    char path[PATH_MAX];
    int64_t mtime, size;
    cached_tags_t tags;
    stored_path(host, path, sizeof(path));
    if (!file_signature(host, &mtime, &size)) {
        result->failed++;
        return;
    }
    if (!full && metadata_db_get(path, mtime, size, &tags)) return;

    song_row_t previous = {0};
    bool had_previous = full && metadata_db_get_song_by_path(path, &previous);
    if (had_previous) result->matched_existing++;
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read_scan_tags(host, &meta);
    library_scan_tags_from_track(&meta, &tags);
    result->parsed++;
    /* A forced refresh writes only rows whose tags differ; leaving same-tag
     * rows alone keeps an unchanged library byte-identical. */
    if (full && had_previous && library_scan_tags_equal(&previous.tags, &tags)) return;
    bool stored_changed = metadata_db_put(path, mtime, size, &tags);
    if (full && had_previous && stored_changed) result->changed++;
    if (full && !had_previous) result->inserted = true;
}

static int finish(const file_list_t *list, const scan_result_t *result, int skipped, bool written) {
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
    json_boolean(&line, "written", written);
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
    if (!sdtool_open_database()) return SDTOOL_FAILED;
    file_list_t list = {0};
    scan_result_t result = {0};
    int count = 0, skipped = 0;
    atomic_int progress = 0;

    json_progress("walk", 0, 0);
    /* A root that cannot be read is a void scan, not an empty library. */
    if (!file_browser_walk_all_songs_excluding_top_level(sdtool_root(), AUDIOBOOKS_LIBRARY_DIR_NAME,
                                                         list_add, &list, &count, &progress, &skipped))
        return fail(&list, "walk_failed", "the card could not be read");

    if (!options->full && library_unchanged(&list, skipped)) {
        int rc = finish(&list, &result, 0, false);
        list_free(&list);
        metadata_db_close();
        return rc;
    }
    if (list.count == 0 && metadata_db_get_song_count() == 0) {
        int rc = finish(&list, &result, skipped, false);
        list_free(&list);
        metadata_db_close();
        return rc;
    }

    metadata_db_begin_update();
    for (long i = 0; i < list.count; i++) {
        update_file(list.paths[i], options->full, &result);
        json_progress("tags", i + 1, list.count);
    }

    /* As the player does for a forced refresh: with nothing changed,
     * inserted or missing, committing would only rewrite the same library. */
    bool missing = options->full && result.matched_existing != metadata_db_get_song_count();
    if (options->full && skipped == 0 && !missing &&
        metadata_refresh_should_skip_commit((int) result.changed, false, result.inserted)) {
        metadata_db_abort_update();
        int rc = finish(&list, &result, skipped, false);
        list_free(&list);
        metadata_db_close();
        return rc;
    }

    json_progress("save", 0, 1);
    if (!metadata_db_end_update())
        return fail(&list, "write_failed", "the music database could not be written");
    json_progress("save", 1, 1);
    int rc = finish(&list, &result, skipped, true);
    list_free(&list);
    metadata_db_close();
    return rc;
}
