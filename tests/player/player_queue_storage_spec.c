#define _GNU_SOURCE
#include "player_queue_storage.h"
#include "playlist_files.h"
#include "metadata_db.h"
#include "file_browser.h"
#include "remote_track.h"
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>

struct tagcache_snapshot {
    atomic_int refs;
    const char ** paths;
    int count;
    bool block_path;
};
struct file_browser_index {
    atomic_int refs;
    const char ** paths;
    int count;
    bool block_path;
};

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool gate_enabled, gate_entered, gate_open;
static atomic_int snapshot_retains, snapshot_closes, index_retains, index_closes;
static bool force_checkpoint_failure, fail_next_thread_create, fail_index_retain;
static int renameat2_error;
static bool create_collision_on_renameat2;
static bool fail_next_fsync;
static bool fail_next_renameat, replace_reservation_before_rename_failure;
static bool fail_reservation_close;
static int fail_reservation_close_fd = -1;
static int replacement_hold_fd = -1;
static bool allow_file_op_gate = true, allow_storage_gate = true;
static int file_op_begins, file_op_ends, storage_begins, storage_ends;
static char fixture_root[PATH_MAX];
static void * cancel_save_thread(void * unused) {
    (void) unused;
    player_queue_storage_save_cancel_and_join();
    return NULL;
}

static void gate_reset(void) {
    pthread_mutex_lock(&gate_mu);
    gate_enabled = true;
    gate_entered = false;
    gate_open = false;
    pthread_mutex_unlock(&gate_mu);
}
static void gate_arrive_and_wait(void) {
    pthread_mutex_lock(&gate_mu);
    if (gate_enabled) {
        gate_entered = true;
        pthread_cond_broadcast(&gate_cv);
        while (!gate_open) pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
}
static void gate_wait_entered(void) {
    pthread_mutex_lock(&gate_mu);
    while (!gate_entered) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
static void gate_release(void) {
    pthread_mutex_lock(&gate_mu);
    gate_open = true;
    gate_enabled = false;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

bool remote_track_path_is_remote(const char * path) {
    return path && strncmp(path, "remote:", 7) == 0;
}
bool file_op_external_begin(void) { file_op_begins++; return allow_file_op_gate; }
void file_op_external_end(void) { file_op_ends++; }
bool usb_mode_control_storage_write_begin(void) { storage_begins++; return allow_storage_gate; }
void usb_mode_control_storage_write_end(void) { storage_ends++; }
metadata_db_snapshot_t * metadata_db_snapshot_retain(metadata_db_snapshot_t * snapshot) {
    if (!snapshot) return NULL;
    struct tagcache_snapshot * s = (struct tagcache_snapshot *) snapshot;
    atomic_fetch_add(&s->refs, 1);
    atomic_fetch_add(&snapshot_retains, 1);
    return snapshot;
}
void metadata_db_snapshot_close(metadata_db_snapshot_t * snapshot) {
    if (!snapshot) return;
    atomic_fetch_sub(&((struct tagcache_snapshot *) snapshot)->refs, 1);
    atomic_fetch_add(&snapshot_closes, 1);
}
bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t * snapshot, int rank,
                                  char * out, size_t out_size) {
    struct tagcache_snapshot * s = (struct tagcache_snapshot *) snapshot;
    if (s->block_path) gate_arrive_and_wait();
    if (!s->paths || rank < 0 || rank >= s->count) return false;
    return snprintf(out, out_size, "%s", s->paths[rank]) < (int) out_size;
}
bool file_browser_index_retain(const file_browser_index_t * source, file_browser_index_t ** out) {
    if (fail_index_retain || !source || !out) return false;
    *out = (file_browser_index_t *) source;
    atomic_fetch_add(&((struct file_browser_index *) source)->refs, 1);
    atomic_fetch_add(&index_retains, 1);
    return true;
}
void file_browser_index_close(file_browser_index_t * index) {
    if (!index) return;
    atomic_fetch_sub(&((struct file_browser_index *) index)->refs, 1);
    atomic_fetch_add(&index_closes, 1);
}
bool file_browser_index_playable_path_at(const file_browser_index_t * index, unsigned rank,
                                         char * out, size_t out_size) {
    struct file_browser_index * i = (struct file_browser_index *) index;
    if (i->block_path) gate_arrive_and_wait();
    if (!i->paths || rank >= (unsigned) i->count) return false;
    return snprintf(out, out_size, "%s", i->paths[rank]) < (int) out_size;
}

extern int __real_pthread_create(pthread_t *, const pthread_attr_t *, void * (*)(void *), void *);
int __wrap_pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                          void * (*start)(void *), void * arg) {
    if (fail_next_thread_create) {
        fail_next_thread_create = false;
        return EAGAIN;
    }
    return __real_pthread_create(thread, attr, start, arg);
}
extern bool __real_queue_resume_write_at(int, const char *, const queue_resume_t *);
bool __wrap_queue_resume_write_at(int dirfd, const char * name, const queue_resume_t * queue) {
    if (force_checkpoint_failure) return false;
    gate_arrive_and_wait();
    return __real_queue_resume_write_at(dirfd, name, queue);
}
extern int __real_openat(int, const char *, int, ...);
int __wrap_openat(int dirfd, const char *path, int flags, ...) {
    int fd;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        va_end(args);
        fd = __real_openat(dirfd, path, flags, mode);
    } else {
        fd = __real_openat(dirfd, path, flags);
    }
    if (fd >= 0 && fail_reservation_close && strcmp(path, "close-failure.m3u") == 0)
        fail_reservation_close_fd = fd;
    return fd;
}
extern int __real_close(int);
int __wrap_close(int fd) {
    if (fd == fail_reservation_close_fd) {
        fail_reservation_close_fd = -1;
        assert(__real_close(fd) == 0);
        errno = EIO;
        return -1;
    }
    return __real_close(fd);
}
extern long __real_syscall(long, ...);
long __wrap_syscall(long number, ...) {
#ifdef SYS_renameat2
    if (number == SYS_renameat2 && renameat2_error) {
        va_list args;
        va_start(args, number);
        int dirfd = va_arg(args, int);
        const char *source = va_arg(args, const char *);
        int target_dirfd = va_arg(args, int);
        const char *target = va_arg(args, const char *);
        (void) va_arg(args, unsigned);
        va_end(args);
        (void) source;
        assert(dirfd == target_dirfd);
        if (create_collision_on_renameat2) {
            int fd = openat(dirfd, target, O_CREAT | O_EXCL | O_WRONLY, 0600);
            assert(fd >= 0 && write(fd, "racer", 5) == 5);
            close(fd);
            create_collision_on_renameat2 = false;
        }
        errno = renameat2_error;
        return -1;
    }
#endif
    va_list args;
    va_start(args, number);
    long a1 = va_arg(args, long), a2 = va_arg(args, long), a3 = va_arg(args, long);
    long a4 = va_arg(args, long), a5 = va_arg(args, long), a6 = va_arg(args, long);
    va_end(args);
    return __real_syscall(number, a1, a2, a3, a4, a5, a6);
}
extern int __real_fsync(int);
int __wrap_fsync(int fd) {
    if (fail_next_fsync) { fail_next_fsync = false; errno = ENOSPC; return -1; }
    return __real_fsync(fd);
}
extern int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    if (fail_next_renameat) {
        fail_next_renameat = false;
        if (replace_reservation_before_rename_failure) {
            replace_reservation_before_rename_failure = false;
            replacement_hold_fd = openat(newdirfd, newpath, O_RDONLY);
            assert(replacement_hold_fd >= 0);
            assert(unlinkat(newdirfd, newpath, 0) == 0);
            int fd = openat(newdirfd, newpath, O_CREAT | O_EXCL | O_WRONLY, 0600);
            assert(fd >= 0 && write(fd, "new owner", 9) == 9);
            close(fd);
        }
        errno = EIO;
        return -1;
    }
    return __real_renameat(olddirfd, oldpath, newdirfd, newpath);
}

static queue_resume_t one_track(const char * path) {
    queue_resume_t q = {.count = 1, .current = 0, .position = 3.5};
    q.paths = calloc(1, sizeof(*q.paths));
    assert(q.paths);
    q.paths[0] = strdup(path);
    assert(q.paths[0]);
    return q;
}
static void enter_fixture_case(const char * name) {
    assert(chdir(fixture_root) == 0);
    assert(mkdir(name, 0700) == 0);
    assert(chdir(name) == 0);
}
static void make_dirs(void) {
    assert(mkdir("queue-storage-spec", 0700) == 0);
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/Playlists", 0700) == 0);
}
static void test_checkpoint_owns_snapshot_and_pinned_directory(void) {
    enter_fixture_case("checkpoint");
    make_dirs();
    int failed_fd = open("./queue-storage-spec", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(failed_fd >= 0);
    fail_next_thread_create = true;
    assert(!player_queue_storage_checkpoint_start(one_track("/media/not-started.flac"), failed_fd, "not-started.bin"));
    assert(!player_queue_storage_checkpoint_busy());
    assert(fcntl(failed_fd, F_GETFD) < 0 && errno == EBADF);

    int live = open("./queue-storage-spec", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(live >= 0);
    int workerfd = dup(live);
    assert(workerfd >= 0);
    gate_reset();
    assert(player_queue_storage_checkpoint_start(one_track("/media/original.flac"), workerfd, "queue.bin"));
    gate_wait_entered();
    close(live);
    assert(rename("./queue-storage-spec", "./queue-storage-spec-old") == 0);
    assert(mkdir("./queue-storage-spec", 0755) == 0);
    gate_release();
    bool completed = false, failed = true;
    while (!player_queue_storage_checkpoint_reap_if_done(&completed, &failed)) usleep(1000);
    assert(completed && !failed);
    assert(access("./queue-storage-spec-old/queue.bin", F_OK) == 0);
    assert(access("./queue-storage-spec/queue.bin", F_OK) != 0);
    queue_resume_t read = {0};
    assert(queue_resume_read("./queue-storage-spec-old/queue.bin", &read));
    assert(strcmp(read.paths[0], "/media/original.flac") == 0);
    queue_resume_free(&read);
}
static void test_checkpoint_failure_and_large_cleanup(void) {
    int fd = open("./queue-storage-spec-old", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(fd >= 0);
    force_checkpoint_failure = true;
    assert(player_queue_storage_checkpoint_start(one_track("/media/fail.flac"), dup(fd), "failed.bin"));
    player_queue_storage_checkpoint_join();
    assert(player_queue_storage_checkpoint_failed());
    assert(access("./queue-storage-spec-old/failed.bin", F_OK) != 0);
    force_checkpoint_failure = false;
    gate_reset();
    assert(player_queue_storage_checkpoint_start(one_track("/media/cleanup.flac"), dup(fd), "cleanup.bin"));
    gate_wait_entered();
    player_queue_storage_checkpoint_defer_unlink();
    gate_release();
    player_queue_storage_checkpoint_join();
    assert(access("./queue-storage-spec-old/cleanup.bin", F_OK) != 0);
    player_queue_storage_checkpoint_clear_deferred_unlink();
    close(fd);
}
static void test_save_as_retains_inputs_and_writes_ordered_m3u(void) {
    enter_fixture_case("save-ordered");
    make_dirs();
    static const char * paths[] = {"/media/db-a.flac", "/media/db-b.flac"};
    struct tagcache_snapshot snapshot = {.refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 2};
    char * live_paths[] = {NULL, strdup("/media/explicit.flac")};
    int * order = malloc(2 * sizeof(*order));
    int ranks[] = {0, 1};
    assert(order && live_paths[1]);
    order[0] = 1; order[1] = 0;
    assert(player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "owned-snapshot", .count = 2, .order = order,
        .lazy_ranks = ranks, .paths = live_paths,
        .snapshot = (metadata_db_snapshot_t *) &snapshot, .queue_sd_dirfd = -1
    }));
    assert(atomic_load(&snapshot_retains) == 1 && atomic_load(&snapshot.refs) == 2);
    free(live_paths[1]);
    live_paths[1] = NULL;
    assert(!player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "replacement", .count = 0, .order = NULL, .queue_sd_dirfd = -1
    }));
    metadata_db_snapshot_close((metadata_db_snapshot_t *) &snapshot);
    bool active = true, done = false;
    player_queue_storage_save_completion_t completion;
    while ((active = player_queue_storage_save_poll(&completion, &done)) && !done) usleep(1000);
    assert(active && done && completion.worker_ok);
    char ** read_paths = NULL;
    int read_count = 0;
    assert(playlist_files_read("./music/Playlists/owned-snapshot.m3u", &read_paths, &read_count));
    assert(read_count == 2);
    assert(strcmp(read_paths[0], "/media/explicit.flac") == 0);
    assert(strcmp(read_paths[1], "/media/db-a.flac") == 0);
    for (int i = 0; i < read_count; i++) free(read_paths[i]);
    free(read_paths);
    player_queue_storage_save_release_completed();
    assert(atomic_load(&snapshot.refs) == 0);
    assert(atomic_load(&snapshot_closes) == 2);
}
static void test_save_cancel_joins_before_release_and_start_failures(void) {
    enter_fixture_case("save-cancel");
    make_dirs();
    static const char * paths[] = {"/media/blocked.flac"};
    struct tagcache_snapshot snapshot = {.refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 1, .block_path = true};
    char * live_paths[] = {NULL};
    int ranks[] = {0};
    int file_begins_before = file_op_begins, file_ends_before = file_op_ends;
    int storage_begins_before = storage_begins, storage_ends_before = storage_ends;
    gate_reset();
    assert(player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "cancelled", .count = 1, .order = calloc(1, sizeof(int)),
        .lazy_ranks = ranks, .paths = live_paths,
        .snapshot = (metadata_db_snapshot_t *) &snapshot, .queue_sd_dirfd = -1
    }));
    gate_wait_entered();
    metadata_db_snapshot_close((metadata_db_snapshot_t *) &snapshot);
    pthread_t cancel_thread;
    assert(pthread_create(&cancel_thread, NULL, cancel_save_thread, NULL) == 0);
    while (atomic_load(&snapshot.refs) != 1) usleep(1000);
    gate_release();
    pthread_join(cancel_thread, NULL);
    assert(atomic_load(&snapshot.refs) == 0);
    assert(file_op_begins == file_begins_before + 1 && file_op_ends == file_ends_before + 1);
    assert(storage_begins == storage_begins_before + 1 && storage_ends == storage_ends_before + 1);
    /* The provider passed its cancellation check before the barrier; like the
     * original worker, this in-flight item may still be published. */

    struct tagcache_snapshot fail_snapshot = {.refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 1};
    int retains_before = atomic_load(&snapshot_retains);
    int closes_before = atomic_load(&snapshot_closes);
    fail_next_thread_create = true;
    assert(!player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "create-fail", .count = 1, .order = calloc(1, sizeof(int)),
        .lazy_ranks = ranks, .paths = live_paths,
        .snapshot = (metadata_db_snapshot_t *) &fail_snapshot, .queue_sd_dirfd = -1
    }));
    assert(atomic_load(&fail_snapshot.refs) == 1);
    assert(atomic_load(&snapshot_retains) == retains_before + 1);
    assert(atomic_load(&snapshot_closes) == closes_before + 1);

    struct tagcache_snapshot retained_snapshot = {.refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 1};
    struct file_browser_index index = {.refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 1};
    retains_before = atomic_load(&snapshot_retains);
    closes_before = atomic_load(&snapshot_closes);
    fail_index_retain = true;
    assert(!player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "index-retain-fail", .count = 1, .order = calloc(1, sizeof(int)),
        .lazy_ranks = ranks, .paths = live_paths,
        .snapshot = (metadata_db_snapshot_t *) &retained_snapshot,
        .file_index = (file_browser_index_t *) &index, .queue_sd_dirfd = -1
    }));
    fail_index_retain = false;
    assert(atomic_load(&retained_snapshot.refs) == 1);
    assert(atomic_load(&index.refs) == 1);
    assert(atomic_load(&snapshot_retains) == retains_before + 1);
    assert(atomic_load(&snapshot_closes) == closes_before + 1);
}

static void test_save_uses_pinned_sd_playlists_directory(void) {
    enter_fixture_case("save-sd-pinned");
    assert(mkdir("sd_mount", 0700) == 0);
    assert(mkdir("sd_mount/.compas", 0700) == 0);
    int queue_dirfd = open("sd_mount/.compas", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(queue_dirfd >= 0);
    static const char * paths[] = {"/data/mnt/sd_0/song.flac"};
    struct tagcache_snapshot snapshot = {
        .refs = ATOMIC_VAR_INIT(1), .paths = paths, .count = 1, .block_path = true
    };
    int closes_before = atomic_load(&snapshot_closes);
    char * live_paths[] = {NULL};
    int ranks[] = {0};
    gate_reset();
    assert(player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = "pinned-sd", .count = 1, .order = calloc(1, sizeof(int)),
        .lazy_ranks = ranks, .paths = live_paths,
        .snapshot = (metadata_db_snapshot_t *) &snapshot,
        .queue_sd_dirfd = queue_dirfd, .has_sd = true
    }));
    gate_wait_entered();
    close(queue_dirfd);
    metadata_db_snapshot_close((metadata_db_snapshot_t *) &snapshot);
    assert(rename("sd_mount", "sd_mount-old") == 0);
    assert(mkdir("sd_mount", 0700) == 0);
    assert(mkdir("sd_mount/.compas", 0700) == 0);
    gate_release();
    bool active = true, done = false;
    player_queue_storage_save_completion_t completion;
    while ((active = player_queue_storage_save_poll(&completion, &done)) && !done) usleep(1000);
    assert(active && done && completion.worker_ok);
    assert(access("sd_mount-old/Playlists/pinned-sd.m3u", F_OK) == 0);
    assert(access("sd_mount/Playlists/pinned-sd.m3u", F_OK) != 0);
    player_queue_storage_save_release_completed();
    assert(atomic_load(&snapshot.refs) == 0);
    assert(atomic_load(&snapshot_closes) == closes_before + 2);
}

static void run_gate_save(const char * name, bool expected_ok) {
    char *paths[] = {"/media/gated.flac"};
    int *order = malloc(sizeof(*order));
    assert(order); order[0] = 0;
    assert(player_queue_storage_save_start((player_queue_storage_save_source_t) {
        .name = name, .count = 1, .order = order, .paths = paths,
        .queue_sd_dirfd = -1
    }));
    bool active = true, done = false;
    player_queue_storage_save_completion_t completion;
    while ((active = player_queue_storage_save_poll(&completion, &done)) && !done) usleep(1000);
    assert(active && done && completion.worker_ok == expected_ok);
    player_queue_storage_save_release_completed();
}
static void test_save_as_uses_file_and_usb_gates(void) {
    enter_fixture_case("save-gates"); make_dirs();
    file_op_begins = file_op_ends = storage_begins = storage_ends = 0;
    allow_file_op_gate = false;
    run_gate_save("file-gate-denied", false);
    assert(file_op_begins == 1 && file_op_ends == 0 && storage_begins == 0 && storage_ends == 0);
    allow_file_op_gate = true; allow_storage_gate = false;
    run_gate_save("usb-gate-denied", false);
    assert(file_op_begins == 2 && file_op_ends == 1 && storage_begins == 1 && storage_ends == 0);
    allow_storage_gate = true;
    run_gate_save("publish-ok", true);
    assert(file_op_begins == 3 && file_op_ends == 2 && storage_begins == 2 && storage_ends == 1);
    int fd = open("music/Playlists/publish-fail.m3u", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0); close(fd);
    run_gate_save("publish-fail", false);
    assert(file_op_begins == 4 && file_op_ends == 3 && storage_begins == 3 && storage_ends == 2);
    allow_file_op_gate = true; allow_storage_gate = true;
}

static bool test_path_provider(void *ctx, int rank, const char **out) {
    (void) rank;
    if (!ctx) return false;
    *out = (const char *) ctx;
    return true;
}
static void test_stream_save_rename_fallback(void) {
    enter_fixture_case("save-rename-fallback");
    assert(mkdir("Playlists", 0700) == 0);
    int dirfd = open("Playlists", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dirfd >= 0);
    char leaf[NAME_MAX] = "unchanged";
    renameat2_error = EINVAL;
    assert(playlist_files_write_new_stream_at(dirfd, "fallback-einval", 1,
        test_path_provider, "/media/fallback.flac", leaf, sizeof(leaf)));
    assert(strcmp(leaf, "fallback-einval.m3u") == 0);
    FILE *f = fdopen(openat(dirfd, leaf, O_RDONLY), "r");
    assert(f);
    char content[128] = {0};
    assert(fread(content, 1, sizeof(content) - 1, f) > 0);
    fclose(f);
    assert(strcmp(content, "#EXTM3U\n/media/fallback.flac\n") == 0);

    renameat2_error = ENOSYS;
    leaf[0] = '\0';
    assert(playlist_files_write_new_stream_at(dirfd, "fallback-enosys", 1,
        test_path_provider, "/media/fallback.flac", leaf, sizeof(leaf)));
    assert(strcmp(leaf, "fallback-enosys.m3u") == 0);
    f = fdopen(openat(dirfd, leaf, O_RDONLY), "r");
    assert(f);
    memset(content, 0, sizeof(content));
    assert(fread(content, 1, sizeof(content) - 1, f) > 0);
    fclose(f);
    assert(strcmp(content, "#EXTM3U\n/media/fallback.flac\n") == 0);

    renameat2_error = EINVAL;
    fail_reservation_close = true;
    assert(!playlist_files_write_new_stream_at(dirfd, "close-failure", 1,
        test_path_provider, "/media/close-failure.flac", NULL, 0));
    fail_reservation_close = false;
    assert(fail_reservation_close_fd == -1);
    assert(faccessat(dirfd, "close-failure.m3u", F_OK, 0) != 0 && errno == ENOENT);

    /* Existing files and a destination created after renameat2 fails must both
     * survive unchanged; O_EXCL on the fallback reservation rejects them. */
    int fd = openat(dirfd, "collision.m3u", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "existing", 8) == 8); close(fd);
    assert(!playlist_files_write_new_stream_at(dirfd, "collision", 1,
        test_path_provider, "/media/no-overwrite.flac", NULL, 0));
    fd = openat(dirfd, "collision.m3u", O_RDONLY);
    assert(fd >= 0);
    char collision[9] = {0}; assert(read(fd, collision, 8) == 8); close(fd);
    assert(memcmp(collision, "existing", 8) == 0);
    create_collision_on_renameat2 = true;
    assert(!playlist_files_write_new_stream_at(dirfd, "raced", 1,
        test_path_provider, "/media/no-overwrite.flac", NULL, 0));
    fd = openat(dirfd, "raced.m3u", O_RDONLY);
    assert(fd >= 0);
    char raced[8] = {0}; assert(read(fd, raced, 5) == 5); close(fd);
    assert(memcmp(raced, "racer", 5) == 0);
    fail_next_renameat = true;
    assert(!playlist_files_write_new_stream_at(dirfd, "rename-cleanup", 1,
        test_path_provider, "/media/fail.flac", NULL, 0));
    assert(faccessat(dirfd, "rename-cleanup.m3u", F_OK, 0) != 0);
    fail_next_renameat = true;
    replace_reservation_before_rename_failure = true;
    assert(!playlist_files_write_new_stream_at(dirfd, "reservation-replaced", 1,
        test_path_provider, "/media/fail.flac", NULL, 0));
    assert(replacement_hold_fd >= 0);
    close(replacement_hold_fd); replacement_hold_fd = -1;
    fd = openat(dirfd, "reservation-replaced.m3u", O_RDONLY);
    assert(fd >= 0);
    char owner[10] = {0}; assert(read(fd, owner, 9) == 9); close(fd);
    assert(memcmp(owner, "new owner", 9) == 0);
    assert(!playlist_files_write_new_stream_at(dirfd, "provider-fail", 1,
        test_path_provider, NULL, NULL, 0));
    fail_next_fsync = true;
    assert(!playlist_files_write_new_stream_at(dirfd, "full-card", 1,
        test_path_provider, "/media/no-space.flac", NULL, 0));
    assert(faccessat(dirfd, "full-card.m3u", F_OK, 0) != 0);
    int scanfd = dup(dirfd);
    assert(scanfd >= 0);
    DIR *entries = fdopendir(scanfd);
    assert(entries);
    struct dirent *entry;
    while ((entry = readdir(entries)))
        assert(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
               strcmp(entry->d_name, "fallback-einval.m3u") == 0 ||
               strcmp(entry->d_name, "fallback-enosys.m3u") == 0 ||
               strcmp(entry->d_name, "collision.m3u") == 0 || strcmp(entry->d_name, "raced.m3u") == 0 ||
               strcmp(entry->d_name, "reservation-replaced.m3u") == 0);
    closedir(entries);
    close(dirfd);
    renameat2_error = 0;
}

static int remove_fixture_entry(const char * path, const struct stat * stat_buf,
                                int type, struct FTW * walk) {
    (void) stat_buf;
    (void) type;
    (void) walk;
    return remove(path);
}

static void create_fixture_root(void) {
    const char * base = getenv("PLAYER_QUEUE_STORAGE_SPEC_TMPDIR");
    if (!base || !base[0]) base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    char * absolute_base = realpath(base, NULL);
    assert(absolute_base);
    char template[PATH_MAX];
    int n = snprintf(template, sizeof(template), "%s/player-queue-storage-spec.XXXXXX", absolute_base);
    free(absolute_base);
    assert(n > 0 && (size_t) n < sizeof(template));
    char * root = mkdtemp(template);
    assert(root && strlen(root) < sizeof(fixture_root));
    snprintf(fixture_root, sizeof(fixture_root), "%s", root);
}

int main(void) {
    create_fixture_root();
    test_checkpoint_owns_snapshot_and_pinned_directory();
    test_checkpoint_failure_and_large_cleanup();
    test_save_as_retains_inputs_and_writes_ordered_m3u();
    test_save_cancel_joins_before_release_and_start_failures();
    test_save_uses_pinned_sd_playlists_directory();
    test_save_as_uses_file_and_usb_gates();
    test_stream_save_rename_fallback();
    assert(chdir("/") == 0);
    assert(nftw(fixture_root, remove_fixture_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Player queue storage: linked checkpoint/save workers, pinned descriptors, retained snapshots, ordered output, failure, cancel and SD destination identity passed");
    return 0;
}
