#define _GNU_SOURCE
#include <assert.h>
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
#include <sys/types.h>
#include <unistd.h>

#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/metadata/catalog/metadata_refresh_targets.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../library/metadata/scan/library_scan.h"
#include "../../../core/system/logging/db_log.h"

#undef DB_LOG
#define DB_LOG(...) ((void)0)

static bool storage_available = true;
static bool cancel_during_walk, cancel_during_parse, lose_storage_during_parse, parse_failure;
static unsigned walk_visits, parser_calls, begin_calls, end_calls, abort_calls;
static unsigned books_rescan_calls, playlists_rescan_calls;
static int64_t song_count(void);

#include "library_scan_spec_support.inc"
#include "../../../library/metadata/scan/library_scan.c"

static void rescan_playlists(void) { playlists_rescan_calls++; }

static int count_open_fds(void) {
    DIR *dir = opendir("/proc/self/fd");
    assert(dir);
    int count = 0;
    while (readdir(dir)) count++;
    closedir(dir);
    return count;
}

static void write_song(const char *path, const char *title, const char *artist, const char *album) {
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "title=%s\nartist=%s\nalbum=%s\n", title, artist, album);
    fclose(file);
}

static library_scan_result_t run_scan(library_scan_mode_t mode, bool allow_rebuild,
                                      char **paths, int path_count, library_scan_scope_t scope) {
    library_scan_request_t request = {
        .mode = mode,
        .allow_rebuild = allow_rebuild,
        .target_paths = paths,
        .target_count = path_count,
        .scope = scope,
        .outcome_seed = METADATA_DB_LOAD_SUCCESS_NORMAL,
        .books_rescan = gui_books_rescan,
        .playlists_rescan = rescan_playlists,
    };
    storage_available = true;
    library_scan_reset_cancel();
    library_scan_prepare(&request);
    library_scan_run();
    library_scan_result_t result;
    library_scan_get_result(&result);
    library_scan_release();
    return result;
}

static char **one_path(const char *path) {
    char **paths = calloc(1, sizeof(*paths));
    assert(paths);
    paths[0] = strdup(path);
    assert(paths[0]);
    return paths;
}

int main(void) {
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/Album", 0700) == 0);
    write_song("music/Album/one.mp3", "One", "Artist", "Album");
    write_song("music/Album/two.mp3", "Two", "Artist", "Album");

    library_scan_reset_cancel();
    library_scan_run_full_current(gui_books_rescan, rescan_playlists);
    library_scan_result_t result;
    library_scan_get_result(&result);
    assert(song_count() == 2 && end_calls == 1 && result.succeeded);
    assert(books_rescan_calls == 1 && playlists_rescan_calls == 1);
    unsigned parsed = parser_calls;
    int32_t generation = test_generation;

    result = run_scan(LIBRARY_SCAN_MODE_NORMAL, true, NULL, 0, LIBRARY_SCAN_SCOPE_NONE);
    assert(parser_calls == parsed && end_calls == 1 && test_generation == generation && result.succeeded);
    write_song("music/Album/two.mp3", "Two changed", "Artist", "Album");
    result = run_scan(LIBRARY_SCAN_MODE_NORMAL, true, NULL, 0, LIBRARY_SCAN_SCOPE_NONE);
    assert(parser_calls == parsed + 1 && end_calls == 2 && test_generation > generation && song_count() == 2);

    parsed = parser_calls;
    generation = test_generation;
    char **legacy_target = one_path("./music/Album/one.mp3");
    library_scan_request_t target_request = {
        .mode = LIBRARY_SCAN_MODE_TARGETED,
        .allow_rebuild = true,
        .target_paths = legacy_target,
        .target_count = 1,
        .scope = LIBRARY_SCAN_SCOPE_PATHS,
        .outcome_seed = METADATA_DB_LOAD_SUCCESS_NORMAL,
        .books_rescan = gui_books_rescan,
        .playlists_rescan = rescan_playlists,
    };
    library_scan_prepare(&target_request);
    library_scan_run_full_current(gui_books_rescan, rescan_playlists);
    library_scan_run();
    library_scan_get_result(&result);
    assert(result.succeeded && end_calls == 2 && parser_calls == parsed + 1 && test_generation == generation);
    library_scan_release();

    char **failed_target = one_path("./music/Album/one.mp3");
    target_request.target_paths = failed_target;
    library_scan_prepare(&target_request);
    library_scan_record_start_failure();
    library_scan_release();
    assert(scan_job.target_paths == NULL && scan_job.target_count == 0);
    unsigned old_walks = walk_visits;
    unsigned old_parsers = parser_calls;
    unsigned old_books = books_rescan_calls;
    unsigned old_playlists = playlists_rescan_calls;
    unsigned old_begins = begin_calls;
    library_scan_run_full_current(gui_books_rescan, rescan_playlists);
    library_scan_get_result(&result);
    assert(result.operation_outcome == METADATA_DB_LOAD_FAILED);
    assert(walk_visits == old_walks && parser_calls == old_parsers);
    assert(books_rescan_calls == old_books && playlists_rescan_calls == old_playlists);
    assert(begin_calls == old_begins);
    library_scan_release();

    write_song("music/Album/one.mp3", "One revised", "Artist", "Album");
    result = run_scan(LIBRARY_SCAN_MODE_TARGETED, true, one_path("./music/Album/one.mp3"), 1,
                      LIBRARY_SCAN_SCOPE_PATHS);
    assert(result.succeeded && end_calls == 3 && test_generation > generation && result.changed_count == 1);

    parse_failure = true;
    result = run_scan(LIBRARY_SCAN_MODE_TARGETED, true, one_path("./music/Album/one.mp3"), 1,
                      LIBRARY_SCAN_SCOPE_PATHS);
    assert(result.incomplete && result.failed_count == 1 && song_count() == 2);

    unsigned aborts = abort_calls;
    cancel_during_parse = true;
    result = run_scan(LIBRARY_SCAN_MODE_TARGETED, true, one_path("./music/Album/one.mp3"), 1,
                      LIBRARY_SCAN_SCOPE_PATHS);
    assert(abort_calls == aborts + 1 && library_scan_cancel_pending());

    aborts = abort_calls;
    cancel_during_parse = false;
    lose_storage_during_parse = true;
    result = run_scan(LIBRARY_SCAN_MODE_TARGETED, true, one_path("./music/Album/two.mp3"), 1,
                      LIBRARY_SCAN_SCOPE_PATHS);
    assert(abort_calls == aborts + 1 && result.operation_outcome == METADATA_DB_LOAD_UNMOUNTED);

    aborts = abort_calls;
    result = run_scan(LIBRARY_SCAN_MODE_TARGETED, true, one_path("./music/../outside.mp3"), 1,
                      LIBRARY_SCAN_SCOPE_PATHS);
    assert(abort_calls == aborts + 1);

    library_scan_reset_cancel();
    lose_storage_during_parse = false;
    cancel_during_parse = false;
    cancel_during_walk = true;
    int old_fds = count_open_fds();
    unsigned old_ends = end_calls;
    result = run_scan(LIBRARY_SCAN_MODE_NORMAL, true, NULL, 0, LIBRARY_SCAN_SCOPE_NONE);
    assert(end_calls == old_ends && library_scan_cancel_pending());
    assert(access("music/.compas_scan.tmp", F_OK) != 0 && count_open_fds() == old_fds);

    printf("library-scan rows=%lld parser_calls=%u generation=%d begin=%u end=%u abort=%u walked=%u\n",
           (long long)song_count(), parser_calls, test_generation, begin_calls, end_calls, abort_calls, walk_visits);
    (void)result;
    return 0;
}
