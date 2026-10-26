#define _GNU_SOURCE
#include "../../../../src/library/metadata/db/metadata_db.h"
#include "../../../../src/library/tagcache/core/tagcache.h"
#include "../../../../src/library/remote/subsonic/subsonic_saved_servers.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *p, int32_t *r, int32_t *c, int32_t *l) { (void)p; (void)r; (void)c; (void)l; return false; }
bool remote_state_get(const char *p, int32_t *r, int32_t *c, int32_t *l) { (void)p; (void)r; (void)c; (void)l; return false; }
void remote_state_set_rating(const char *p, int32_t r) { (void)p; (void)r; }
void remote_state_add_play(const char *p, int32_t n) { (void)p; (void)n; }
void path_cache_replace(const char *n, char *const *p, int c) { (void)n; (void)p; (void)c; }
void path_cache_load(const char *n, char ***p, int *c) { (void)n; *p = NULL; *c = 0; }
void path_cache_load_matching(const char *n, const char *f, char ***p, int *c) { (void)n; (void)f; *p = NULL; *c = 0; }
void path_cache_insert(const char *n, const char *p) { (void)n; (void)p; }
void path_cache_delete(const char *n, const char *p) { (void)n; (void)p; }
bool path_cache_has(const char *n, const char *p) { (void)n; (void)p; return false; }
void subsonic_saved_servers_upsert(const char *u, const char *n, const char *p, bool v) { (void)u; (void)n; (void)p; (void)v; }
void subsonic_saved_servers_load(subsonic_saved_server_t **r, int *c) { *r = NULL; *c = 0; }
static pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static bool pause_enabled;
static bool paused;
static bool resume_write;
static bool fail_write;
static bool pause_staging;

ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *buf, size_t size) {
    char link[64], path[512];
    ssize_t n = readlink((snprintf(link, sizeof(link), "/proc/self/fd/%d", fd), link), path, sizeof(path) - 1);
    if (pause_enabled && !pause_staging && n > 0) {
        path[n] = '\0';
        if (strstr(path, ".tcd.g2") && strstr(path, "database_")) {
            pthread_mutex_lock(&pause_mutex);
            if (!paused) {
                paused = true;
                pthread_cond_broadcast(&pause_cond);
                while (!resume_write) pthread_cond_wait(&pause_cond, &pause_mutex);
            }
            pthread_mutex_unlock(&pause_mutex);
            if (fail_write) { errno = EIO; return -1; }
        }
    }
    return __real_write(fd, buf, size);
}

ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __wrap_pwrite(int fd, const void *buf, size_t size, off_t offset) {
    char link[64], path[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (pause_enabled && pause_staging && n > 0) {
        path[n] = '\0';
        if (strstr(path, ".tagcache-work-")) {
            pthread_mutex_lock(&pause_mutex);
            if (!paused) {
                paused = true;
                pthread_cond_broadcast(&pause_cond);
                while (!resume_write) pthread_cond_wait(&pause_cond, &pause_mutex);
            }
            pthread_mutex_unlock(&pause_mutex);
            if (fail_write) { errno = EIO; return -1; }
        }
    }
    return __real_pwrite(fd, buf, size, offset);
}

static void alarm_handler(int signo) { (void)signo; _exit(124); }

static void must(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "concurrent metadata test: %s\n", what); exit(EXIT_FAILURE); }
}

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    must(fd >= 0 && close(fd) == 0, "touch");
}

static void put(const char *path, const char *title, const char *artist) {
    cached_tags_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.title, sizeof(t.title), "%s", title);
    snprintf(t.artist, sizeof(t.artist), "%s", artist);
    snprintf(t.album, sizeof(t.album), "Album");
    snprintf(t.album_artist, sizeof(t.album_artist), "%s", artist);
    snprintf(t.genre, sizeof(t.genre), "Genre");
    t.track_number = 1;
    t.disc_number = 1;
    metadata_db_put(path, 1, 1, &t);
}

static void *scan_worker(void *arg) {
    (void)arg;
    metadata_db_begin_update();
    put("song.flac", "Changed title", "Changed artist");
    put("new.flac", "New title", "New artist");
    (void)metadata_db_end_update();
    return NULL;
}

static void wait_paused(void) {
    pthread_mutex_lock(&pause_mutex);
    while (!paused) pthread_cond_wait(&pause_cond, &pause_mutex);
    pthread_mutex_unlock(&pause_mutex);
}

static void resume_worker(void) {
    pthread_mutex_lock(&pause_mutex);
    resume_write = true;
    pthread_cond_broadcast(&pause_cond);
    pthread_mutex_unlock(&pause_mutex);
}

static void assert_old_view(void) {
    song_row_t row;
    must(metadata_db_get_song_by_path("song.flac", &row), "old song query while commit paused");
    must(strcmp(row.tags.title, "Original title") == 0, "old title remains visible while commit paused");
    must(!metadata_db_get_song_by_path("new.flac", &row), "new song hidden while commit paused");
    must(metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, 0, 1, &row) == 1 &&
         !strcmp(row.tags.title, "Original title"), "committed title page while worker paused");
    group_row_t group;
    must(metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, 0, 1, &group) == 1 &&
         !strcmp(group.name, "Original artist"), "committed artist page while worker paused");
}

static void setup_library(void) {
    must(mkdir("music", 0700) == 0, "music directory");
    touch("song.flac");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh metadata db");
    metadata_db_begin_update();
    put("song.flac", "Original title", "Original artist");
    must(metadata_db_end_update(), "initial metadata commit");
}

int main(int argc, char **argv) {
    signal(SIGALRM, alarm_handler);
    alarm(20);
    must(argc == 2, "mode argument");
    char fixture[] = ".metadata-concurrent.XXXXXX";
    must(mkdtemp(fixture) != NULL && chdir(fixture) == 0, "fixture");
    setup_library();
    const bool abort_mode = strcmp(argv[1], "abort") == 0;
    const bool failure_mode = strstr(argv[1], "failure") != NULL;
    pause_staging = strstr(argv[1], "staging") != NULL;
    if (abort_mode) {
        metadata_db_begin_update();
        put("song.flac", "Aborted title", "Aborted artist");
        metadata_db_song_favorite_set("song.flac", true);
        metadata_db_song_play_count_increment("song.flac");
        metadata_db_abort_update();
        song_row_t row;
        must(metadata_db_get_song_by_path("song.flac", &row) && strcmp(row.tags.title, "Original title") == 0,
             "abort restores metadata");
        must(metadata_db_song_favorite_is_set("song.flac"), "favorite survives abort");
        metadata_db_close();
        must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen after abort");
        tagcache_song_t stats;
        must(tagcache_song_by_path("song.flac", &stats) && stats.playcount == 1 && stats.rating == 1,
             "rating and play count survive abort and reopen");
        metadata_db_close();
        return 0;
    }
    touch("new.flac");
    pause_enabled = true;
    fail_write = failure_mode;
    pthread_t worker;
    must(pthread_create(&worker, NULL, scan_worker, NULL) == 0, "create scan worker");
    wait_paused();
    assert_old_view();
    metadata_db_song_favorite_set("song.flac", true);
    metadata_db_song_play_count_increment("song.flac");
    resume_worker();
    must(pthread_join(worker, NULL) == 0, "join scan worker");
    pause_enabled = false;
    song_row_t row;
    bool new_visible = metadata_db_get_song_by_path("new.flac", &row);
    if (failure_mode) {
        must(!new_visible, "failed commit hides new song");
        must(metadata_db_get_song_by_path("song.flac", &row) && strcmp(row.tags.title, "Original title") == 0,
             "failed commit restores old title");
    } else {
        must(new_visible && strcmp(row.tags.title, "New title") == 0, "successful commit publishes new song");
        must(metadata_db_get_song_by_path("song.flac", &row) && strcmp(row.tags.title, "Changed title") == 0,
             "successful commit publishes changed title");
    }
    must(metadata_db_song_favorite_is_set("song.flac"), "favorite persists after worker");
    metadata_db_close();
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen metadata db");
    must(metadata_db_song_favorite_is_set("song.flac"), "favorite persists after reopen");
    tagcache_song_t stats;
    must(tagcache_song_by_path("song.flac", &stats) && stats.playcount == 1,
         "play count persists after worker and reopen");
    metadata_db_close();
    return 0;
}
