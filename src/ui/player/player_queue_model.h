#ifndef PLAYER_QUEUE_MODEL_H
#define PLAYER_QUEUE_MODEL_H

/* Singleton owner for the live player queue. Every operation is UI-thread-only.
 * `mode` arguments use canonical play_mode.h PLAY_MODE_* values and are passed
 * explicitly; the model never reads settings or GUI state. Returned path
 * pointers are synchronous borrows and must not be retained asynchronously;
 * they remain valid until the next queue mutation or clear. */
#include <stdbool.h>
#include <stdint.h>
#include "file_browser.h"
#include "metadata_db.h"
#include "queue_resume.h"
#include "play_mode.h"

typedef struct {
    /* One-shot transfer into an empty model after clear-finish. Takes every
     * pointer/reference/fd without allocating. Set queue_sd_dirfd to an owned
     * pin or explicitly to -1; zero-initialization would transfer stdin. */
    char **paths;
    int count;
    int current;
    int *lazy_ranks;
    bool lazy_order_is_recency;
    metadata_db_snapshot_t *snapshot;
    file_browser_index_t *file_index;
    int *shuffle_order;          /* optional permutation of count physical slots */
    int *shuffle_continuation;   /* optional next-cycle permutation of count slots */
    int shuffle_pos;
    int pending_count;
    int next_insert_index;
    int queue_sd_dirfd;
} player_queue_install_t;

typedef enum {
    PLAYER_QUEUE_EDIT_OK,
    PLAYER_QUEUE_EDIT_STALE,
    PLAYER_QUEUE_EDIT_INVALID
} player_queue_edit_result_t;

typedef struct {
    int *display_order; /* caller owns and frees */
    int count;
    int current; /* shuffle display offset, otherwise physical current */
    uint64_t revision;
} player_queue_display_snapshot_t;

/* Paired lifecycle preserves the exact free_playlist interleaving: begin
 * advances revision and frees pending/shuffle state; GUI resets controller
 * failure/audio generation; finish frees paths, fd, ranks and retained refs.
 * No failure or reentrant call is allowed between the pair. */
void player_queue_model_clear_begin(void);
void player_queue_model_clear_finish(void);
void player_queue_model_install_take(player_queue_install_t owned);

int player_queue_model_count(void);
int player_queue_model_current(void); /* always physical */
int player_queue_model_pending_count(void);
uint64_t player_queue_model_revision(void);
const char *player_queue_model_path_at(int physical_index);
const char *player_queue_model_path_if_resolved(int physical_index);
bool player_queue_model_has_lazy_order(void);
bool player_queue_model_has_resolved_sd_path(const char *music_root);
bool player_queue_model_is_stream_cache_path(const char *path);
int player_queue_model_dup_sd_dirfd(void);
bool player_queue_model_has_sd_dirfd_pin(void);
void player_queue_model_remove_sd_checkpoint(const char *name);
int player_queue_model_random_index(int count);
bool player_queue_model_make_shuffle_order(int count, int **order_out, int *first_out);
/* Takes ownership of dirfd; closes it if the model already owns a live pin. */
void player_queue_model_adopt_sd_dirfd(int dirfd);

bool player_queue_model_display_snapshot(int mode, player_queue_display_snapshot_t *out);
void player_queue_model_display_snapshot_free(player_queue_display_snapshot_t *snapshot);

/* Active selected queue only. The GUI keeps the no-current queue-ready case as
 * a preflighted install transaction with its existing UI/audio effects. */
bool player_queue_model_try_add_many(const char *const *paths, int count,
                                     int mode, int *added_out);
bool player_queue_model_remove_pending(int offset);
player_queue_edit_result_t player_queue_model_edit_displayed(
    uint64_t expected_revision, int from_display, int to_display, int mode);
bool player_queue_model_select_displayed(uint64_t expected_revision,
                                         int display_index, int mode, int *physical_out);
bool player_queue_model_clear_to_current(int mode);
bool player_queue_model_restart_displayed(int display_index, int mode, int *physical_out);
/* Delete current path/rank physically; do not change shuffle or pending state.
 * If nonempty, current remains on its old physical slot until caller starts
 * the chosen replacement. If empty, current becomes -1. */
bool player_queue_model_delete_current(int *next_physical_out);
/* Called after GUI audio-stop and synchronous stopped subscribers when the
 * preceding delete emptied the queue; preserves their legacy observation point. */
void player_queue_model_finish_empty_delete(void);
/* For a valid current: inserts at pending-prefix head and leaves cursor at the
 * resulting prefix end. Failure restores the old cursor. Caller consumes any
 * confirmed audio advance first. No-current stays on the GUI install path. */
bool player_queue_model_play_next(const char *path, int mode);

/* Prediction never consumes a bag. Confirmed pending auto consume steps a
 * same-length bag regardless of mode and has no tail gate. Manual pending Next
 * consumes only when current + 1 < count; Manual Prev only clears prefix/cursor.
 * Manual looping-shuffle wrap reshuffles the active bag in place and retains a
 * prepared continuation; automatic wrap promotes it. */
int player_queue_model_predict_auto_advance(int mode, int current);
void player_queue_model_commit_auto_advance(int mode);
int player_queue_model_manual_step(int mode, int current, int direction);
/* Caller validates and calls at the existing current-index write site, before
 * resolving path; this operation has no other side effects. */
void player_queue_model_select_physical(int physical_index);
const char *player_queue_model_note_next_armed(int mode, int current,
                                               int *next_physical_out,
                                               uint64_t *revision_out);
/* Internally ensures the active shuffle bag, eagerly attempts a looping
 * continuation allocation, plans and commits only successful targets. Partial
 * pending consume retains cursor; full consume clears it. Wrap promotes with
 * (old_pos + steps) % count; nonwrap steps add and clamp to count - 1. No target
 * leaves prepared continuation but commits no plan/current change. */
int player_queue_model_advance_past_failed(int mode,
                                           const char *const *failed_paths,
                                           int failed_count);

/* Produces an owned physical-order queue_resume_t; caller frees/transfers it. */
bool player_queue_model_checkpoint_snapshot(int mode, double position,
                                             queue_resume_t *out);
bool player_queue_model_save_as(const char *name, int mode, bool has_sd);

#endif
