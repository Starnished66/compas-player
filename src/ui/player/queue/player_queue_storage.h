#ifndef PLAYER_QUEUE_STORAGE_H
#define PLAYER_QUEUE_STORAGE_H

#include <stdbool.h>
#include "../../../library/playlists/queue_resume.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/browser/files/file_browser.h"

/* UI-thread only. The prior job must already be reaped/joined. The player has
 * materialized this owned queue and pinned the target directory; the service
 * consumes both on every outcome, including pthread_create failure. */
bool player_queue_storage_checkpoint_start(queue_resume_t queue, int target_dirfd,
                                           const char * target_name);
/* Like checkpoint_start(), but rewrites only the header (position and cursor)
 * of the file the last full checkpoint wrote. If that file cannot be reused the
 * job fails quietly and checkpoint_take_header_fallback() reports it, so the
 * caller does a full write next time. Consumes target_dirfd on every outcome. */
bool player_queue_storage_checkpoint_start_header(queue_resume_header_t header, int target_dirfd,
                                                  const char * target_name);
bool player_queue_storage_checkpoint_take_header_fallback(void);
bool player_queue_storage_checkpoint_busy(void);
/* Returns false only while a worker is still running. Otherwise joins any
 * completed worker and reports whether one was reaped and whether it failed. */
bool player_queue_storage_checkpoint_reap_if_done(bool * completed, bool * failed);
void player_queue_storage_checkpoint_join(void);
bool player_queue_storage_checkpoint_failed(void);
void player_queue_storage_checkpoint_mark_failed(void);
void player_queue_storage_checkpoint_defer_unlink(void);
void player_queue_storage_checkpoint_clear_deferred_unlink(void);
void player_queue_storage_remove_checkpoint_at(int dirfd, const char * name);

/* Borrowed player state is read only during save_start. order ownership is
 * transferred on every call; arrays and the SD descriptor are synchronous-only.
 * The service copies paths/ranks and retains its own DB/file-index references. */
typedef struct {
    const char * name;
    int count;
    int * order;
    const int * lazy_ranks;
    char * const * paths;
    metadata_db_snapshot_t * snapshot;
    file_browser_index_t * file_index;
    int queue_sd_dirfd;
    bool has_sd;
} player_queue_storage_save_source_t;

/* output_dirfd and leaf are borrowed until save_release_completed(). */
typedef struct {
    bool worker_ok;
    int output_dirfd;
    const char * leaf;
} player_queue_storage_save_completion_t;

bool player_queue_storage_save_active(void);
bool player_queue_storage_save_start(player_queue_storage_save_source_t source);
/* UI-thread only. A false return means there is no active save. If active,
 * done reports whether the worker was joined and completion is valid then. */
bool player_queue_storage_save_poll(player_queue_storage_save_completion_t * completion,
                                    bool * done);
/* UI-thread only; releases completed-poll resources in their established order. */
void player_queue_storage_save_release_completed(void);
/* UI-thread only; sets cancellation, joins the worker, then releases resources. */
void player_queue_storage_save_cancel_and_join(void);

#endif
