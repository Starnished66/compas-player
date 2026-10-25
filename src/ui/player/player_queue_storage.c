#include "player_queue_storage.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "playlist_files.h"
#include "library_paths.h"
#include "remote_track.h"
#include "file_ops.h"
#include "usb_mode_control.h"

#define PLAYER_QUEUE_PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
/* This player-local prefix intentionally matches gui_player.c's existing
 * filter; it is not the path used by the Subsonic download UI. */
#define PLAYER_QUEUE_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"

typedef struct {
    int target_dirfd;
    char target_name[NAME_MAX];
    queue_resume_t queue;
} checkpoint_worker_ctx_t;

static checkpoint_worker_ctx_t checkpoint_ctx;
static pthread_t checkpoint_thread;
static bool checkpoint_running;
static atomic_bool checkpoint_done;
static atomic_bool checkpoint_large_lazy_cleanup_pending;
static atomic_bool checkpoint_failure_pending;

void player_queue_storage_remove_checkpoint_at(int dirfd, const char * name) {
    if (dirfd >= 0 && name) (void) unlinkat(dirfd, name, 0);
}

static void * queue_checkpoint_worker(void * arg) {
    checkpoint_worker_ctx_t * ctx = (checkpoint_worker_ctx_t *) arg;
    bool wrote = queue_resume_write_at(ctx->target_dirfd, ctx->target_name, &ctx->queue);
    if (!wrote) {
        /* Retry on the next checkpoint; never replace a valid file on failure. */
        atomic_store(&checkpoint_failure_pending, true);
    } else {
        atomic_store(&checkpoint_failure_pending, false);
    }
    queue_resume_free(&ctx->queue);
    if (atomic_load(&checkpoint_large_lazy_cleanup_pending)) {
        player_queue_storage_remove_checkpoint_at(ctx->target_dirfd, ctx->target_name);
    }
    close(ctx->target_dirfd);
    ctx->target_dirfd = -1;
    atomic_store(&checkpoint_done, true);
    return NULL;
}

bool player_queue_storage_checkpoint_start(queue_resume_t queue, int target_dirfd,
                                           const char * target_name) {
    checkpoint_ctx.target_dirfd = target_dirfd;
    snprintf(checkpoint_ctx.target_name, sizeof(checkpoint_ctx.target_name), "%s", target_name);
    checkpoint_ctx.queue = queue;

    atomic_store(&checkpoint_done, false);
    checkpoint_running = pthread_create(&checkpoint_thread, NULL, queue_checkpoint_worker,
                                        &checkpoint_ctx) == 0;
    if (!checkpoint_running) {
        queue_resume_free(&checkpoint_ctx.queue);
        close(target_dirfd);
        checkpoint_ctx.target_dirfd = -1;
    }
    return checkpoint_running;
}

bool player_queue_storage_checkpoint_busy(void) {
    return checkpoint_running && !atomic_load(&checkpoint_done);
}

bool player_queue_storage_checkpoint_reap_if_done(bool * completed, bool * failed) {
    if (completed) *completed = false;
    if (failed) *failed = false;
    if (!checkpoint_running) return true;
    if (!atomic_load(&checkpoint_done)) return false;
    pthread_join(checkpoint_thread, NULL);
    checkpoint_running = false;
    if (completed) *completed = true;
    if (failed) *failed = atomic_load(&checkpoint_failure_pending);
    return true;
}

void player_queue_storage_checkpoint_join(void) {
    if (!checkpoint_running) return;
    pthread_join(checkpoint_thread, NULL);
    checkpoint_running = false;
}

bool player_queue_storage_checkpoint_failed(void) {
    return atomic_load(&checkpoint_failure_pending);
}

void player_queue_storage_checkpoint_mark_failed(void) {
    atomic_store(&checkpoint_failure_pending, true);
}

void player_queue_storage_checkpoint_defer_unlink(void) {
    atomic_store(&checkpoint_large_lazy_cleanup_pending, true);
}

void player_queue_storage_checkpoint_clear_deferred_unlink(void) {
    atomic_store(&checkpoint_large_lazy_cleanup_pending, false);
}

typedef struct {
    int * order, * ranks;
    char ** explicit_paths;
    metadata_db_snapshot_t * snapshot;
    file_browser_index_t * file_index;
    atomic_bool * cancel;
    char resolved[PATH_MAX];
} queue_save_provider_ctx_t;

static bool queue_save_path_provider(void * context, int index, const char ** path) {
    queue_save_provider_ctx_t * ctx = context;
    if (ctx->cancel && atomic_load(ctx->cancel)) return false;
    int physical = ctx->order[index];
    const char * p = ctx->explicit_paths[physical];
    if (!p && ctx->snapshot &&
        !metadata_db_snapshot_path_at(ctx->snapshot, ctx->ranks[physical],
                                      ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && ctx->file_index &&
        !file_browser_index_playable_path_at(ctx->file_index, (unsigned) ctx->ranks[physical],
                                             ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && !ctx->snapshot && !ctx->file_index) return false;
    if (!p) p = ctx->resolved;
    if (!p || !p[0] || remote_track_path_is_remote(p) ||
        strncmp(p, PLAYER_QUEUE_STREAM_CACHE_DIR, strlen(PLAYER_QUEUE_STREAM_CACHE_DIR)) == 0) return false;
    *path = p;
    return true;
}

typedef struct queue_save_worker_state {
    int dirfd, count;
    char name[NAME_MAX], leaf[NAME_MAX];
    queue_save_provider_ctx_t provider;
    atomic_bool done;
    atomic_bool cancel;
    bool ok;
    bool joined;
    pthread_t thread;
} queue_save_worker_state_t;

static queue_save_worker_state_t * queue_save_worker;

bool player_queue_storage_save_active(void) {
    return queue_save_worker != NULL;
}

static void * queue_save_worker_func(void * arg) {
    queue_save_worker_state_t * w = arg;
    bool file_op_held = file_op_external_begin();
    bool storage_held = file_op_held && usb_mode_control_storage_write_begin();
    w->ok = storage_held && playlist_files_write_new_stream_at(w->dirfd, w->name, w->count,
                                                                queue_save_path_provider, &w->provider,
                                                                w->leaf, sizeof(w->leaf));
    if (storage_held) usb_mode_control_storage_write_end();
    if (file_op_held) file_op_external_end();
    atomic_store(&w->done, true);
    return NULL;
}

static void queue_save_free_explicit_paths(queue_save_worker_state_t * w) {
    for (int i = 0; i < w->count; i++) free(w->provider.explicit_paths[i]);
    free(w->provider.explicit_paths);
    free(w->provider.ranks);
    free(w->provider.order);
}

bool player_queue_storage_save_start(player_queue_storage_save_source_t source) {
    if (queue_save_worker) { free(source.order); return false; }
    queue_save_worker_state_t * w = calloc(1, sizeof(*w));
    if (!w) { free(source.order); return false; }
    if (!source.name || !source.name[0] || strlen(source.name) >= sizeof(w->name)) {
        free(w); free(source.order); return false;
    }
    w->count = source.count;
    snprintf(w->name, sizeof(w->name), "%s", source.name);
    atomic_store(&w->cancel, false);
    w->provider.cancel = &w->cancel;
    w->provider.order = source.order;
    w->provider.ranks = calloc((size_t) source.count, sizeof(int));
    w->provider.explicit_paths = calloc((size_t) source.count, sizeof(char *));
    if (!w->provider.ranks || !w->provider.explicit_paths) {
        free(w->provider.ranks); free(w->provider.explicit_paths); free(source.order); free(w); return false;
    }
    for (int i = 0; i < source.count; i++) {
        w->provider.ranks[i] = source.lazy_ranks ? source.lazy_ranks[i] : -1;
        if (source.paths[i]) {
            w->provider.explicit_paths[i] = strdup(source.paths[i]);
            if (!w->provider.explicit_paths[i]) {
                for (int j = 0; j < i; j++) free(w->provider.explicit_paths[j]);
                free(w->provider.explicit_paths); free(w->provider.ranks);
                free(source.order); free(w); return false;
            }
        }
    }
    if (source.snapshot) {
        w->provider.snapshot = metadata_db_snapshot_retain(source.snapshot);
        if (!w->provider.snapshot) {
            queue_save_free_explicit_paths(w);
            free(w); return false;
        }
    }
    if (source.file_index &&
        !file_browser_index_retain(source.file_index, &w->provider.file_index)) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        queue_save_free_explicit_paths(w);
        free(w); return false;
    }
    if (source.has_sd && source.queue_sd_dirfd < 0) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        queue_save_free_explicit_paths(w);
        free(w); return false;
    }
    if (source.queue_sd_dirfd >= 0) {
        int parent = openat(source.queue_sd_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) (void) mkdirat(parent, "Playlists", 0755);
        w->dirfd = parent >= 0 ? openat(parent, "Playlists", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (parent >= 0) close(parent);
    } else w->dirfd = open(PLAYER_QUEUE_PLAYLISTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->dirfd < 0 || pthread_create(&w->thread, NULL, queue_save_worker_func, w) != 0) {
        if (w->dirfd >= 0) close(w->dirfd);
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        queue_save_free_explicit_paths(w);
        free(w); return false;
    }
    queue_save_worker = w;
    return true;
}

bool player_queue_storage_save_poll(player_queue_storage_save_completion_t * completion,
                                    bool * done) {
    if (!queue_save_worker) { if (done) *done = true; return false; }
    queue_save_worker_state_t * w = queue_save_worker;
    if (!atomic_load(&w->done)) { if (done) *done = false; return true; }
    if (!w->joined) {
        pthread_join(w->thread, NULL);
        w->joined = true;
    }
    if (completion) *completion = (player_queue_storage_save_completion_t) {
        .worker_ok = w->ok, .output_dirfd = w->dirfd, .leaf = w->leaf
    };
    if (done) *done = true;
    return true;
}

void player_queue_storage_save_release_completed(void) {
    queue_save_worker_state_t * w = queue_save_worker;
    if (!w || !w->joined) return;
    if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
    if (w->dirfd >= 0) close(w->dirfd);
    if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
    queue_save_free_explicit_paths(w);
    free(w);
    queue_save_worker = NULL;
}

void player_queue_storage_save_cancel_and_join(void) {
    queue_save_worker_state_t * w = queue_save_worker;
    if (!w) return;
    atomic_store(&w->cancel, true);
    if (!w->joined) pthread_join(w->thread, NULL);
    if (w->dirfd >= 0) close(w->dirfd);
    if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
    if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
    queue_save_free_explicit_paths(w);
    free(w);
    queue_save_worker = NULL;
}
