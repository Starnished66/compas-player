#include "player_queue_model.h"

#include "../../../audio/engine/audio_helpers.h"
#include "../../../library/playlists/playback_order.h"
#include "player_queue_storage.h"
#include "../../../library/remote/remote_track.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"

static char **queue_paths;
static int queue_count;
static int queue_current = -1;
static int *queue_lazy_ranks;
static bool queue_lazy_order_is_recency;
static metadata_db_snapshot_t *queue_snapshot;
static file_browser_index_t *queue_file_index;
static int queue_sd_dirfd = -1;
static int *queue_shuffle_order;
static int queue_shuffle_order_count;
static int queue_shuffle_pos = -1;
static bool shuffle_rng_seeded;
static int *queue_shuffle_continuation;
static int queue_pending_count;
static int queue_next_insert_index = -1;
static uint64_t queue_revision = 1;

static bool mode_uses_shuffle_bag(int mode) {
    return mode == PLAY_MODE_SHUFFLE || mode == PLAY_MODE_SHUFFLE_ONCE;
}

static void fisher_yates_shuffle(int *order, int count) {
    if (!order) return;
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

static void ensure_shuffle_rng_seeded(void) {
    if (shuffle_rng_seeded) return;
    srand((unsigned)time(NULL));
    shuffle_rng_seeded = true;
}

int player_queue_model_random_index(int count) {
    if (count <= 0) return -1;
    ensure_shuffle_rng_seeded();
    return rand() % count;
}

bool player_queue_model_make_shuffle_order(int count, int **order_out, int *first_out) {
    if (!order_out || !first_out || count <= 0) return false;
    *order_out = NULL;
    *first_out = 0;
    int *order = malloc(sizeof(*order) * (size_t)count);
    if (!order) return false;
    ensure_shuffle_rng_seeded();
    for (int i = 0; i < count; i++) order[i] = i;
    fisher_yates_shuffle(order, count);
    *first_out = order[0];
    *order_out = order;
    return true;
}

static void ensure_shuffle_order_current(void) {
    if (queue_shuffle_order && queue_shuffle_order_count == queue_count) return;

    ensure_shuffle_rng_seeded();
    free(queue_shuffle_order);
    queue_shuffle_order = malloc(sizeof(int) * (size_t)queue_count);
    if (!queue_shuffle_order) {
        queue_shuffle_order_count = 0;
        queue_shuffle_pos = -1;
        return;
    }
    for (int i = 0; i < queue_count; i++) queue_shuffle_order[i] = i;
    fisher_yates_shuffle(queue_shuffle_order, queue_count);
    queue_shuffle_order_count = queue_count;

    /* A continuation belongs to the prior bag length/order. */
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
    queue_shuffle_pos = 0;
    if (queue_current >= 0) {
        for (int i = 0; i < queue_count; i++) {
            if (queue_shuffle_order[i] == queue_current) {
                int first = queue_shuffle_order[0];
                queue_shuffle_order[0] = queue_shuffle_order[i];
                queue_shuffle_order[i] = first;
                queue_shuffle_pos = 0;
                break;
            }
        }
    }
    for (int n = 0; n < queue_pending_count; n++) {
        int physical = queue_current + 1 + n;
        int found = 0;
        while (found < queue_count && queue_shuffle_order[found] != physical) found++;
        if (found == queue_count) continue;
        memmove(queue_shuffle_order + found, queue_shuffle_order + found + 1,
                (size_t)(queue_count - found - 1) * sizeof(int));
        if (found <= queue_shuffle_pos) queue_shuffle_pos--;
        int at = queue_shuffle_pos + 1 + n;
        memmove(queue_shuffle_order + at + 1, queue_shuffle_order + at,
                (size_t)(queue_count - at - 1) * sizeof(int));
        queue_shuffle_order[at] = physical;
    }
}

static void queue_shuffle_insert(int physical, int added) {
    int old_count = queue_count - added;
    if (queue_shuffle_order && queue_shuffle_order_count == old_count) {
        int *grown = realloc(queue_shuffle_order, (size_t)queue_count * sizeof(int));
        if (grown) {
            queue_shuffle_order = grown;
            int at = queue_shuffle_pos + 1 + physical - queue_current - 1;
            if (at < 0) at = 0;
            if (at > old_count) at = old_count;
            playback_order_insert(queue_shuffle_order, old_count, physical, at, added);
            queue_shuffle_order_count = queue_count;
        } else {
            free(queue_shuffle_order);
            queue_shuffle_order = NULL;
            queue_shuffle_order_count = 0;
        }
    }
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
}

static void queue_shuffle_remove(int physical) {
    playback_order_note_removed(queue_shuffle_order, &queue_shuffle_order_count,
                                &queue_shuffle_pos, physical, queue_count);
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
}

static int compute_auto_advance_index(int index, int mode) {
    if (index < 0 || queue_count <= 0) return -1;
    if (queue_pending_count > 0 && index + 1 < queue_count) return index + 1;

    switch (mode) {
        case PLAY_MODE_REPEAT_ONE:
            return index;
        case PLAY_MODE_REPEAT_ALL:
            return (index + 1) % queue_count;
        case PLAY_MODE_SHUFFLE: {
            ensure_shuffle_order_current();
            if (!queue_shuffle_order) return -1;
            int peek_pos = queue_shuffle_pos + 1;
            if (peek_pos < queue_count) return queue_shuffle_order[peek_pos];
            if (!queue_shuffle_continuation) {
                queue_shuffle_continuation = malloc(sizeof(int) * (size_t)queue_count);
                if (!queue_shuffle_continuation) return -1;
                memcpy(queue_shuffle_continuation, queue_shuffle_order,
                       sizeof(int) * (size_t)queue_count);
                fisher_yates_shuffle(queue_shuffle_continuation, queue_count);
            }
            return queue_shuffle_continuation[0];
        }
        case PLAY_MODE_SHUFFLE_ONCE: {
            ensure_shuffle_order_current();
            if (!queue_shuffle_order) return -1;
            int peek_pos = queue_shuffle_pos + 1;
            return peek_pos < queue_count ? queue_shuffle_order[peek_pos] : -1;
        }
        case PLAY_MODE_SEQUENTIAL:
        default:
            return index + 1 < queue_count ? index + 1 : -1;
    }
}

void player_queue_model_clear_begin(void) {
    queue_revision++;
    queue_pending_count = 0;
    queue_next_insert_index = -1;
    free(queue_shuffle_order);
    queue_shuffle_order = NULL;
    queue_shuffle_order_count = 0;
    queue_shuffle_pos = -1;
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
}

void player_queue_model_clear_finish(void) {
    for (int i = 0; i < queue_count; i++) free(queue_paths[i]);
    free(queue_paths);
    queue_paths = NULL;
    queue_count = 0;
    queue_current = -1;
    if (queue_sd_dirfd >= 0) close(queue_sd_dirfd);
    queue_sd_dirfd = -1;
    free(queue_lazy_ranks);
    queue_lazy_ranks = NULL;
    if (queue_snapshot) metadata_db_snapshot_close(queue_snapshot);
    queue_snapshot = NULL;
    if (queue_file_index) file_browser_index_close(queue_file_index);
    queue_file_index = NULL;
}

void player_queue_model_install_take(player_queue_install_t owned) {
    assert(queue_paths == NULL && queue_count == 0 && queue_current == -1);
    assert(queue_lazy_ranks == NULL && queue_snapshot == NULL && queue_file_index == NULL);
    assert(queue_shuffle_order == NULL && queue_shuffle_continuation == NULL);
    assert(queue_sd_dirfd == -1);
    assert(owned.queue_sd_dirfd >= -1);
    queue_paths = owned.paths;
    queue_count = owned.count;
    queue_current = owned.current;
    queue_lazy_ranks = owned.lazy_ranks;
    queue_lazy_order_is_recency = owned.lazy_order_is_recency;
    queue_snapshot = owned.snapshot;
    queue_file_index = owned.file_index;
    queue_shuffle_order = owned.shuffle_order;
    queue_shuffle_order_count = owned.shuffle_order ? owned.count : 0;
    queue_shuffle_continuation = owned.shuffle_continuation;
    queue_shuffle_pos = owned.shuffle_pos;
    queue_pending_count = owned.pending_count;
    queue_next_insert_index = owned.next_insert_index;
    queue_sd_dirfd = owned.queue_sd_dirfd;
}

int player_queue_model_count(void) { return queue_count; }
int player_queue_model_current(void) { return queue_current; }
int player_queue_model_pending_count(void) { return queue_pending_count; }
uint64_t player_queue_model_revision(void) { return queue_revision; }

const char *player_queue_model_path_if_resolved(int physical_index) {
    if (physical_index < 0 || physical_index >= queue_count || !queue_paths || !queue_paths[physical_index]) return "";
    return queue_paths[physical_index];
}

const char *player_queue_model_path_at(int physical_index) {
    if (physical_index < 0 || physical_index >= queue_count) return "";
    if (queue_paths[physical_index]) return queue_paths[physical_index];
    if (!queue_lazy_ranks) return "";
    int rank = queue_lazy_ranks[physical_index];
    char resolved[PATH_MAX];
    bool got = queue_snapshot && metadata_db_snapshot_path_at(queue_snapshot, rank, resolved, sizeof(resolved));
    if (!got && queue_file_index)
        got = file_browser_index_playable_path_at(queue_file_index, (unsigned)rank,
                                                  resolved, sizeof(resolved));
    if (!got || !resolved[0]) return "";
    char *copy = strdup(resolved);
    if (!copy) return "";
    queue_paths[physical_index] = copy;
    return copy;
}

bool player_queue_model_has_lazy_order(void) { return queue_lazy_ranks != NULL; }

bool player_queue_model_is_stream_cache_path(const char *path) {
    return path && strncmp(path, QUEUE_STREAM_CACHE_DIR, strlen(QUEUE_STREAM_CACHE_DIR)) == 0;
}

bool player_queue_model_has_resolved_sd_path(const char *music_root) {
    if (!music_root || !music_root[0]) return false;
    size_t root_len = strlen(music_root);
    for (int i = 0; i < queue_count; i++) {
        const char *path = queue_paths ? queue_paths[i] : NULL;
        if (!path || strncmp(path, music_root, root_len) != 0) continue;
        if (path[root_len] == '/' || path[root_len] == '\0') return true;
    }
    return false;
}

int player_queue_model_dup_sd_dirfd(void) {
    return queue_sd_dirfd >= 0 ? dup(queue_sd_dirfd) : -1;
}
bool player_queue_model_has_sd_dirfd_pin(void) { return queue_sd_dirfd >= 0; }
void player_queue_model_remove_sd_checkpoint(const char *name) {
    if (queue_sd_dirfd >= 0) player_queue_storage_remove_checkpoint_at(queue_sd_dirfd, name);
}
void player_queue_model_adopt_sd_dirfd(int dirfd) {
    if (dirfd < 0) return;
    if (queue_sd_dirfd >= 0) close(dirfd);
    else queue_sd_dirfd = dirfd;
}

bool player_queue_model_display_snapshot(int mode, player_queue_display_snapshot_t *out) {
    if (!out) return false;
    *out = (player_queue_display_snapshot_t){
        .count = queue_count,
        .current = queue_current,
        .revision = queue_revision
    };
    if (!queue_count) return true;
    int *order = malloc((size_t)queue_count * sizeof(int));
    if (!order) return false;
    if (mode_uses_shuffle_bag(mode)) {
        ensure_shuffle_order_current();
        if (!queue_shuffle_order) {
            free(order);
            return false;
        }
        memcpy(order, queue_shuffle_order, (size_t)queue_count * sizeof(int));
        out->current = queue_shuffle_pos;
    } else {
        for (int i = 0; i < queue_count; i++) order[i] = i;
    }
    out->display_order = order;
    return true;
}

void player_queue_model_display_snapshot_free(player_queue_display_snapshot_t *snapshot) {
    if (!snapshot) return;
    free(snapshot->display_order);
    *snapshot = (player_queue_display_snapshot_t){0};
}

bool player_queue_model_try_add_many(const char *const *new_paths, int count,
                                     int mode, int *added_out) {
    (void)mode;
    if (added_out) *added_out = 0;
    if (!new_paths || count <= 0 || count > INT_MAX - queue_count ||
        count > INT_MAX - queue_pending_count) return false;
    char **copies = calloc((size_t)count, sizeof(*copies));
    if (!copies) return false;
    for (int i = 0; i < count; i++) {
        copies[i] = new_paths[i] ? strdup(new_paths[i]) : NULL;
        if (!copies[i]) {
            for (int j = 0; j < count; j++) free(copies[j]);
            free(copies);
            return false;
        }
    }
    int pos = (queue_next_insert_index >= 0 && queue_next_insert_index <= queue_count)
        ? queue_next_insert_index : queue_current + 1;
    char **grown = realloc(queue_paths, sizeof(*queue_paths) * (size_t)(queue_count + count));
    if (!grown) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return false;
    }
    queue_paths = grown;
    int *grown_ranks = queue_lazy_ranks
        ? realloc(queue_lazy_ranks, sizeof(*queue_lazy_ranks) * (size_t)(queue_count + count))
        : NULL;
    if (queue_lazy_ranks && !grown_ranks) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return false;
    }
    if (grown_ranks) queue_lazy_ranks = grown_ranks;
    memmove(&queue_paths[pos + count], &queue_paths[pos],
            sizeof(*queue_paths) * (size_t)(queue_count - pos));
    if (queue_lazy_ranks)
        memmove(&queue_lazy_ranks[pos + count], &queue_lazy_ranks[pos],
                sizeof(*queue_lazy_ranks) * (size_t)(queue_count - pos));
    for (int i = 0; i < count; i++) {
        queue_paths[pos + i] = copies[i];
        if (queue_lazy_ranks) queue_lazy_ranks[pos + i] = -1;
    }
    free(copies);
    queue_count += count;
    queue_pending_count += count;
    queue_next_insert_index = pos + count;
    queue_shuffle_insert(pos, count);
    if (added_out) *added_out = count;
    return true;
}

bool player_queue_model_remove_pending(int offset) {
    if (offset < 0 || offset >= queue_pending_count || queue_current < 0) return false;
    int pos = queue_current + 1 + offset;
    queue_shuffle_remove(pos);
    free(queue_paths[pos]);
    memmove(&queue_paths[pos], &queue_paths[pos + 1],
            sizeof(char *) * (size_t)(queue_count - pos - 1));
    if (queue_lazy_ranks)
        memmove(&queue_lazy_ranks[pos], &queue_lazy_ranks[pos + 1],
                sizeof(int) * (size_t)(queue_count - pos - 1));
    queue_count--;
    queue_pending_count--;
    queue_next_insert_index = queue_pending_count > 0 ? queue_current + 1 + queue_pending_count : -1;
    return true;
}

static bool materialize_display_order(int mode) {
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(mode, &snapshot)) return false;
    int *order = snapshot.display_order;
    int count = snapshot.count;
    int current = snapshot.current;
    if (!count) {
        free(order);
        return true;
    }
    if (queue_lazy_ranks) {
        int *mapped = malloc((size_t)count * sizeof(*mapped));
        char **mapped_paths = calloc((size_t)count, sizeof(*mapped_paths));
        if (!mapped || !mapped_paths) {
            free(mapped); free(mapped_paths); free(order);
            return false;
        }
        for (int i = 0; i < count; i++) {
            mapped[i] = queue_lazy_ranks[order[i]];
            mapped_paths[i] = queue_paths[order[i]];
        }
        free(order);
        free(queue_paths);
        free(queue_lazy_ranks);
        queue_paths = mapped_paths;
        queue_lazy_ranks = mapped;
        queue_current = current;
        if (queue_shuffle_order && queue_shuffle_order_count == count) {
            for (int i = 0; i < count; i++) queue_shuffle_order[i] = i;
            queue_shuffle_pos = current;
        }
        free(queue_shuffle_continuation);
        queue_shuffle_continuation = NULL;
        queue_next_insert_index = queue_pending_count ? current + 1 + queue_pending_count : -1;
        return true;
    }
    char **paths = calloc((size_t)count, sizeof(*paths));
    if (!paths) {
        free(order);
        return false;
    }
    for (int i = 0; i < count; i++) {
        const char *path = player_queue_model_path_at(order[i]);
        if (!path || !path[0]) {
            free(paths);
            free(order);
            return false;
        }
        paths[i] = queue_paths[order[i]];
    }
    free(queue_paths);
    queue_paths = paths;
    free(queue_lazy_ranks);
    queue_lazy_ranks = NULL;
    queue_current = current;
    if (queue_shuffle_order && queue_shuffle_order_count == count) {
        for (int i = 0; i < count; i++) queue_shuffle_order[i] = i;
        queue_shuffle_pos = current;
    }
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
    queue_next_insert_index = queue_pending_count ? current + 1 + queue_pending_count : -1;
    free(order);
    return true;
}

player_queue_edit_result_t player_queue_model_edit_displayed(
    uint64_t expected_revision, int from_display, int to_display, int mode) {
    if (expected_revision != queue_revision) return PLAYER_QUEUE_EDIT_STALE;
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(mode, &snapshot)) return PLAYER_QUEUE_EDIT_INVALID;
    int current = snapshot.current;
    int count = snapshot.count;
    free(snapshot.display_order);
    if (from_display <= current || from_display >= count ||
        (to_display >= 0 && (to_display <= current || to_display >= count)))
        return PLAYER_QUEUE_EDIT_INVALID;
    if (!materialize_display_order(mode)) return PLAYER_QUEUE_EDIT_INVALID;

    if (queue_lazy_ranks) {
        int moved_rank = queue_lazy_ranks[from_display];
        char *moved_path = queue_paths[from_display];
        if (to_display < 0) {
            queue_shuffle_remove(from_display);
            memmove(&queue_lazy_ranks[from_display], &queue_lazy_ranks[from_display + 1],
                    (size_t)(queue_count - from_display - 1) * sizeof(*queue_lazy_ranks));
            queue_count--;
            free(queue_paths[from_display]);
            memmove(&queue_paths[from_display], &queue_paths[from_display + 1],
                    (size_t)(queue_count - from_display) * sizeof(*queue_paths));
            char **shrunk_paths = realloc(queue_paths, (size_t)queue_count * sizeof(*queue_paths));
            if (shrunk_paths || queue_count == 0) queue_paths = shrunk_paths;
            int *shrunk_ranks = realloc(queue_lazy_ranks, (size_t)queue_count * sizeof(*queue_lazy_ranks));
            if (shrunk_ranks || queue_count == 0) queue_lazy_ranks = shrunk_ranks;
            if (from_display <= queue_current + queue_pending_count) queue_pending_count--;
        } else {
            if (from_display < to_display)
                memmove(&queue_lazy_ranks[from_display], &queue_lazy_ranks[from_display + 1],
                        (size_t)(to_display - from_display) * sizeof(*queue_lazy_ranks));
            else
                memmove(&queue_lazy_ranks[to_display + 1], &queue_lazy_ranks[to_display],
                        (size_t)(from_display - to_display) * sizeof(*queue_lazy_ranks));
            queue_lazy_ranks[to_display] = moved_rank;
            if (from_display < to_display)
                memmove(&queue_paths[from_display], &queue_paths[from_display + 1],
                        (size_t)(to_display - from_display) * sizeof(*queue_paths));
            else
                memmove(&queue_paths[to_display + 1], &queue_paths[to_display],
                        (size_t)(from_display - to_display) * sizeof(*queue_paths));
            queue_paths[to_display] = moved_path;
            int pending_end = queue_current + queue_pending_count;
            if (from_display <= pending_end && to_display > pending_end)
                queue_pending_count = to_display - queue_current;
            else if (from_display > pending_end && to_display <= pending_end)
                queue_pending_count++;
        }
        queue_next_insert_index = queue_pending_count ? queue_current + 1 + queue_pending_count : -1;
        return PLAYER_QUEUE_EDIT_OK;
    }

    char *path = queue_paths[from_display];
    int pending_end = queue_current + queue_pending_count;
    if (to_display < 0) {
        queue_shuffle_remove(from_display);
        memmove(queue_paths + from_display, queue_paths + from_display + 1,
                (size_t)(queue_count - from_display - 1) * sizeof(*queue_paths));
        free(path);
        queue_count--;
        if (from_display <= pending_end) queue_pending_count--;
    } else {
        if (from_display < to_display)
            memmove(queue_paths + from_display, queue_paths + from_display + 1,
                    (size_t)(to_display - from_display) * sizeof(*queue_paths));
        else
            memmove(queue_paths + to_display + 1, queue_paths + to_display,
                    (size_t)(from_display - to_display) * sizeof(*queue_paths));
        queue_paths[to_display] = path;
        if (from_display <= pending_end && to_display > pending_end)
            queue_pending_count = to_display - queue_current;
        else if (from_display > pending_end && to_display <= pending_end)
            queue_pending_count++;
    }
    queue_next_insert_index = queue_pending_count ? queue_current + 1 + queue_pending_count : -1;
    return PLAYER_QUEUE_EDIT_OK;
}

bool player_queue_model_select_displayed(uint64_t expected_revision,
                                         int display_index, int mode, int *physical_out) {
    if (expected_revision != queue_revision || display_index < 0 || display_index >= queue_count) return false;
    if (!materialize_display_order(mode)) return false;
    if (display_index > queue_current && display_index <= queue_current + queue_pending_count)
        queue_pending_count -= display_index - queue_current;
    else if (display_index != queue_current)
        queue_pending_count = 0;
    queue_next_insert_index = queue_pending_count ? display_index + 1 + queue_pending_count : -1;
    if (queue_shuffle_order) queue_shuffle_pos = display_index;
    if (physical_out) *physical_out = display_index;
    return true;
}

bool player_queue_model_clear_to_current(int mode) {
    if (queue_lazy_ranks) {
        const char *current_path = player_queue_model_path_at(queue_current);
        if (!current_path || !current_path[0]) return false;
        char *keep = strdup(current_path);
        if (!keep) return false;
        char **replacement = calloc(1, sizeof(*replacement));
        if (!replacement) {
            free(keep);
            return false;
        }
        replacement[0] = keep;
        for (int i = 0; i < queue_count; i++) free(queue_paths[i]);
        free(queue_paths);
        queue_paths = replacement;
        free(queue_lazy_ranks);
        queue_lazy_ranks = NULL;
        if (queue_snapshot) metadata_db_snapshot_close(queue_snapshot);
        queue_snapshot = NULL;
        if (queue_file_index) file_browser_index_close(queue_file_index);
        queue_file_index = NULL;
        queue_count = 1;
        queue_current = 0;
        free(queue_shuffle_order);
        queue_shuffle_order = NULL;
        queue_shuffle_order_count = 0;
        free(queue_shuffle_continuation);
        queue_shuffle_continuation = NULL;
        queue_pending_count = 0;
        queue_next_insert_index = -1;
        return true;
    }
    if (!materialize_display_order(mode)) return false;
    for (int i = 0; i < queue_count; i++) {
        if (i != queue_current) free(queue_paths[i]);
    }
    queue_paths[0] = queue_paths[queue_current];
    queue_count = 1;
    queue_current = 0;
    if (queue_shuffle_order) {
        queue_shuffle_order[0] = 0;
        queue_shuffle_order_count = 1;
        queue_shuffle_pos = 0;
    }
    queue_pending_count = 0;
    queue_next_insert_index = -1;
    return true;
}

bool player_queue_model_restart_displayed(int display_index, int mode, int *physical_out) {
    if (queue_count <= 0 || display_index < 0 || display_index >= queue_count) return false;
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(mode, &snapshot) || snapshot.count != queue_count) {
        free(snapshot.display_order);
        return false;
    }
    char **next_paths = NULL;
    int *next_ranks = NULL;
    if (!playback_order_resequence(snapshot.display_order, snapshot.count,
                                   queue_paths, queue_lazy_ranks,
                                   &next_paths, &next_ranks)) {
        free(snapshot.display_order);
        return false;
    }
    free(snapshot.display_order);
    free(queue_paths);
    free(queue_lazy_ranks);
    queue_paths = next_paths;
    queue_lazy_ranks = next_ranks;
    queue_current = display_index;
    queue_pending_count = 0;
    queue_next_insert_index = -1;
    free(queue_shuffle_order);
    queue_shuffle_order = NULL;
    queue_shuffle_order_count = 0;
    queue_shuffle_pos = -1;
    free(queue_shuffle_continuation);
    queue_shuffle_continuation = NULL;
    if (physical_out) *physical_out = display_index;
    return true;
}

bool player_queue_model_delete_current(int *next_physical_out) {
    if (queue_current < 0 || queue_current >= queue_count) return false;
    int deleted = queue_current;
    free(queue_paths[deleted]);
    for (int i = deleted; i < queue_count - 1; i++) queue_paths[i] = queue_paths[i + 1];
    if (queue_lazy_ranks) {
        for (int i = deleted; i < queue_count - 1; i++) queue_lazy_ranks[i] = queue_lazy_ranks[i + 1];
    }
    queue_count--;
    if (queue_count == 0) {
        /* The GUI still performs audio stop and synchronous plugin notification
         * before the legacy array/rank/current cleanup point. */
        if (next_physical_out) *next_physical_out = -1;
    } else if (next_physical_out) {
        *next_physical_out = deleted < queue_count ? deleted : queue_count - 1;
    }
    return true;
}

void player_queue_model_finish_empty_delete(void) {
    /* Match delete_song_confirm_cb's post-notification cleanup, including its
     * behavior if a stopped subscriber synchronously replaced the queue. */
    free(queue_paths);
    queue_paths = NULL;
    queue_current = -1;
    free(queue_lazy_ranks);
    queue_lazy_ranks = NULL;
}

bool player_queue_model_play_next(const char *path, int mode) {
    if (!path || !path[0]) return false;
    int old_count = queue_count;
    int old_insert = queue_next_insert_index;
    queue_next_insert_index = queue_current + 1;
    bool added = player_queue_model_try_add_many((const char *const *)&path, 1, mode, NULL);
    if (!added) {
        queue_next_insert_index = old_insert;
        return false;
    }
    if (queue_count == old_count) queue_next_insert_index = old_insert;
    else queue_next_insert_index = queue_current + 1 + queue_pending_count;
    return true;
}

int player_queue_model_predict_auto_advance(int mode, int current) {
    return compute_auto_advance_index(current, mode);
}

void player_queue_model_commit_auto_advance(int mode) {
    if (queue_pending_count > 0) {
        if (queue_shuffle_order && queue_shuffle_order_count == queue_count) queue_shuffle_pos++;
        queue_pending_count--;
        if (queue_pending_count == 0) queue_next_insert_index = -1;
        return;
    }
    if (!mode_uses_shuffle_bag(mode)) return;
    int peek_pos = queue_shuffle_pos + 1;
    if (peek_pos < queue_count) {
        queue_shuffle_pos = peek_pos;
        return;
    }
    if (mode == PLAY_MODE_SHUFFLE && queue_shuffle_continuation) {
        free(queue_shuffle_order);
        queue_shuffle_order = queue_shuffle_continuation;
        queue_shuffle_continuation = NULL;
        queue_shuffle_order_count = queue_count;
        queue_shuffle_pos = 0;
    }
}

int player_queue_model_manual_step(int mode, int current, int direction) {
    if (current < 0 || queue_count <= 0) return -1;
    if (direction < 0 && queue_pending_count > 0) {
        queue_pending_count = 0;
        queue_next_insert_index = -1;
    }
    if (direction > 0 && queue_pending_count > 0 && current + 1 < queue_count) {
        if (queue_shuffle_order && queue_shuffle_order_count == queue_count) queue_shuffle_pos++;
        queue_pending_count--;
        if (queue_pending_count == 0) queue_next_insert_index = -1;
        return current + 1;
    }
    if (mode_uses_shuffle_bag(mode)) {
        ensure_shuffle_order_current();
        if (!queue_shuffle_order) return -1;
        int new_pos = queue_shuffle_pos + direction;
        if (new_pos < 0) return -1;
        if (new_pos >= queue_count && mode == PLAY_MODE_SHUFFLE) {
            fisher_yates_shuffle(queue_shuffle_order, queue_count);
            new_pos = 0;
        }
        if (new_pos >= queue_count) return -1;
        queue_shuffle_pos = new_pos;
        return queue_shuffle_order[queue_shuffle_pos];
    }
    if (mode == PLAY_MODE_REPEAT_ALL)
        return (current + direction + queue_count) % queue_count;
    int next = current + direction;
    return next >= 0 && next < queue_count ? next : -1;
}

void player_queue_model_select_physical(int physical_index) {
    queue_current = physical_index;
}

const char *player_queue_model_note_next_armed(int mode, int current,
                                               int *next_physical_out,
                                               uint64_t *revision_out) {
    queue_revision++;
    if (revision_out) *revision_out = queue_revision;
    if (mode_uses_shuffle_bag(mode)) {
        ensure_shuffle_order_current();
        if (queue_shuffle_order) {
            for (int i = 0; i < queue_count; i++)
                if (queue_shuffle_order[i] == current) { queue_shuffle_pos = i; break; }
        }
    }
    int next_index = compute_auto_advance_index(current, mode);
    if (next_physical_out) *next_physical_out = next_index;
    return next_index < 0 ? "" : player_queue_model_path_at(next_index);
}

typedef struct {
    const char *const *failed_paths;
    int failed_count;
} failed_path_set_t;

static bool candidate_path_failed(int index, void *userdata) {
    failed_path_set_t *set = userdata;
    const char *path = player_queue_model_path_at(index);
    if (!path || !path[0]) return false;
    for (int i = 0; i < set->failed_count; i++)
        if (set->failed_paths[i] && set->failed_paths[i][0] && strcmp(path, set->failed_paths[i]) == 0)
            return true;
    return false;
}

static void commit_failure_advance(const failure_advance_plan_t *plan, int mode) {
    if (!plan || plan->target_index < 0) return;
    if (plan->queued_consumed > 0) {
        if (plan->queued_consumed >= queue_pending_count) {
            queue_pending_count = 0;
            queue_next_insert_index = -1;
        } else {
            queue_pending_count -= plan->queued_consumed;
        }
    }
    if (mode_uses_shuffle_bag(mode) && plan->shuffle_steps > 0) {
        if (plan->shuffle_wrapped && queue_shuffle_continuation) {
            free(queue_shuffle_order);
            queue_shuffle_order = queue_shuffle_continuation;
            queue_shuffle_continuation = NULL;
            queue_shuffle_order_count = queue_count;
            queue_shuffle_pos = (queue_shuffle_pos + plan->shuffle_steps) % queue_count;
        } else {
            queue_shuffle_pos += plan->shuffle_steps;
            if (queue_shuffle_pos >= queue_count) queue_shuffle_pos = queue_count - 1;
        }
    }
}

int player_queue_model_advance_past_failed(int mode,
                                           const char *const *failed_paths,
                                           int failed_count) {
    if (mode == PLAY_MODE_SHUFFLE) ensure_shuffle_order_current();
    if (mode == PLAY_MODE_SHUFFLE && queue_shuffle_order && !queue_shuffle_continuation) {
        int *continuation = malloc(sizeof(int) * (size_t)queue_count);
        if (continuation) {
            memcpy(continuation, queue_shuffle_order, sizeof(int) * (size_t)queue_count);
            fisher_yates_shuffle(continuation, queue_count);
            queue_shuffle_continuation = continuation;
        }
    }
    failed_path_set_t failed = { .failed_paths = failed_paths, .failed_count = failed_count };
    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        queue_current, queue_count, mode, queue_pending_count,
        queue_shuffle_order, queue_shuffle_pos, queue_shuffle_continuation,
        candidate_path_failed, &failed);
    if (plan.target_index < 0) return -1;
    commit_failure_advance(&plan, mode);
    return plan.target_index;
}

bool player_queue_model_checkpoint_snapshot(int mode, double position, queue_resume_t *out) {
    if (!out) return false;
    *out = (queue_resume_t){0};
    queue_resume_t snapshot = {
        .count = queue_count,
        .current = queue_current,
        .pending = queue_pending_count,
        .mode = mode,
        .shuffle_pos = queue_shuffle_pos,
        .position = position
    };
    snapshot.paths = calloc((size_t)snapshot.count, sizeof(char *));
    if (!snapshot.paths) return false;
    bool ok = true;
    for (int i = 0; i < snapshot.count; i++) {
        const char *path = player_queue_model_path_at(i);
        if (!path || !path[0] || remote_track_path_is_remote(path) ||
            player_queue_model_is_stream_cache_path(path)) {
            ok = false;
            break;
        }
        snapshot.paths[i] = strdup(path);
        if (!snapshot.paths[i]) { ok = false; break; }
    }
    if (ok && mode_uses_shuffle_bag(mode)) {
        ensure_shuffle_order_current();
        snapshot.shuffle_pos = queue_shuffle_pos;
        snapshot.order = malloc((size_t)snapshot.count * sizeof(int));
        ok = snapshot.order && queue_shuffle_order;
        if (ok) memcpy(snapshot.order, queue_shuffle_order,
                       (size_t)snapshot.count * sizeof(int));
        if (ok && mode == PLAY_MODE_SHUFFLE && queue_shuffle_continuation) {
            snapshot.continuation = malloc((size_t)snapshot.count * sizeof(int));
            ok = snapshot.continuation != NULL;
            if (ok) memcpy(snapshot.continuation, queue_shuffle_continuation,
                           (size_t)snapshot.count * sizeof(int));
        }
    }
    if (!ok) {
        queue_resume_free(&snapshot);
        return false;
    }
    *out = snapshot;
    return true;
}

bool player_queue_model_save_as(const char *name, int mode, bool has_sd) {
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(mode, &snapshot)) return false;
    return player_queue_storage_save_start((player_queue_storage_save_source_t){
        .name = name,
        .count = snapshot.count,
        .order = snapshot.display_order,
        .lazy_ranks = queue_lazy_ranks,
        .paths = queue_paths,
        .snapshot = queue_snapshot,
        .file_index = queue_file_index,
        .queue_sd_dirfd = queue_sd_dirfd,
        .has_sd = has_sd
    });
}
