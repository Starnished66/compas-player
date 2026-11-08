#include "player_queue_model.h"
#include "player_queue_storage.h"
#include "../screen/gui_player.h"
#include "../screen/gui_player_internal.h"
#include "../../application/gui.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/browser/paths/path_cache.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../network/remote/control/remote_control.h"
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

const char *playlist_path_at(int index) {
    const char *path = player_queue_model_path_at(index);
    return path ? path : "";
}

bool gui_player_queue_restart_displayed(int selected) {
    if (!gui_player_local_playback_allowed()) return false;
    int physical = -1;
    return player_queue_model_restart_displayed(selected, current_settings.play_mode, &physical);
}

int gui_player_get_playlist_index(void) { return player_queue_model_current(); }

bool gui_player_has_active_track(void) {
    int current = player_queue_model_current();
    return current >= 0 && current < player_queue_model_count();
}

const char * gui_player_get_current_track_path(void) {
    int current = player_queue_model_current();
    if (current < 0 || current >= player_queue_model_count()) return "";
    return player_queue_model_path_at(current);
}

const char * gui_player_get_track_path_at(int index) {
    return playlist_path_at(index);
}

int gui_player_get_queued_count(void) { return player_queue_model_pending_count(); }

void gui_player_queue_add(const char * path) {
    queue_add_song(path);
}

uint64_t gui_player_queue_revision(void) { return player_queue_model_revision(); }

bool gui_player_queue_snapshot(int **order, int *count, int *current, uint64_t *revision) {
    if (!order || !count || !current || !revision) return false;
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(current_settings.play_mode, &snapshot)) return false;
    *order = snapshot.display_order;
    *count = snapshot.count;
    *current = snapshot.current;
    *revision = snapshot.revision;
    return true;
}

/* Publish the displayed Up Next order only when the queue revision changes.
 * The remote listener owns its own copy, so it never reads playlist state
 * from its worker thread. This also seeds the endpoint the first time
 * Remote Control is enabled after boot/restoration. */
void gui_player_sync_remote_queue(void) {
    static uint64_t published_revision = 0;
    if (player_queue_model_revision() == published_revision) return;
    int * order = NULL, count = 0, current = -1;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    if (revision == published_revision) { free(order); return; }
    int upcoming = current >= 0 && current < count ? count - current - 1 : 0;
    const char ** paths = upcoming ? malloc(sizeof(*paths) * (size_t) upcoming) : NULL;
    if (upcoming && !paths) { free(order); return; }
    for (int i = 0; i < upcoming; i++) paths[i] = playlist_path_at(order[current + 1 + i]);
    remote_control_sync_queue(paths, upcoming, revision);
    free(paths);
    free(order);
    published_revision = revision;
}

/* Maps a Remote Control Up Next offset (current track excluded) to its
 * displayed queue index, or -1 when the revision or offset is stale. */
static int remote_queue_displayed_index(int offset, uint64_t revision) {
    if (revision != player_queue_model_revision() || offset < 0) return -1;
    int * order = NULL, count = 0, current = -1;
    uint64_t actual = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &actual)) return -1;
    free(order);
    if (actual != revision || current < 0 || offset >= count - current - 1) return -1;
    return current + 1 + offset;
}

bool gui_player_remote_queue_remove(int offset, uint64_t revision) {
    int from = remote_queue_displayed_index(offset, revision);
    return from >= 0 && gui_player_queue_edit(revision, from, -1);
}

bool gui_player_remote_queue_move(int from_offset, int to_offset, uint64_t revision) {
    int from = remote_queue_displayed_index(from_offset, revision);
    int to = remote_queue_displayed_index(to_offset, revision);
    return from >= 0 && to >= 0 && (from == to || gui_player_queue_edit(revision, from, to));
}

bool gui_player_remote_queue_play(int offset, uint64_t revision) {
    int index = remote_queue_displayed_index(offset, revision);
    return index >= 0 && gui_player_queue_select(revision, index);
}

bool gui_player_remote_queue_clear(uint64_t revision) {
    if (revision != player_queue_model_revision()) return false;
    gui_player_queue_clear_all();
    return revision != player_queue_model_revision();
}

bool gui_player_queue_edit(uint64_t revision, int from, int to) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    player_queue_edit_result_t result = player_queue_model_edit_displayed(
        revision, from, to, current_settings.play_mode);
    if (result != PLAYER_QUEUE_EDIT_OK) return false;
    arm_next_track_for_audio(player_queue_model_current());
    playback_intent_bump();
    return true;
}

bool gui_player_queue_select(uint64_t revision, int index) {
    if (!gui_player_local_playback_allowed()) return false;
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int physical = -1;
    if (!player_queue_model_select_displayed(revision, index, current_settings.play_mode, &physical))
        return false;
    play_track_at(physical);
    return true;
}

/* Clear surrounding queue entries while keeping the playing track active. */
void gui_player_queue_clear_all(void) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) return;
    if (!player_queue_model_clear_to_current(current_settings.play_mode)) return;
    arm_next_track_for_audio(player_queue_model_current());
    playback_intent_bump();
}

void gui_player_queue_play_next(const char *path) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) {
        queue_add_song(path);
        return;
    }
    if (!player_queue_model_play_next(path, current_settings.play_mode)) return;
    int current = player_queue_model_current();
    player_set_song_count(current, player_queue_model_count());
    arm_next_track_for_audio(current);
    char msg[128];
    snprintf(msg, sizeof(msg), TR_N("Added %d song to queue", "Added %d songs to queue", 1), 1);
    show_info_toast(msg);
    playback_intent_bump();
}

bool gui_player_queue_save_as(const char *name) {
    if (player_queue_storage_save_active()) return false;
    bool has_sd = player_queue_model_has_resolved_sd_path(MUSIC_ROOT_DIR);
    return player_queue_model_save_as(name, current_settings.play_mode, has_sd);
}

bool gui_player_queue_save_as_poll(bool *done, bool *ok) {
    player_queue_storage_save_completion_t completion;
    bool worker_done = false;
    if (!player_queue_storage_save_poll(&completion, &worker_done)) {
        if (done) *done = true;
        if (ok) *ok = false;
        return false;
    }
    if (!worker_done) { if (done) *done = false; return true; }
    struct stat job_dir, current_dir;
    bool same_destination = fstat(completion.output_dirfd, &job_dir) == 0 &&
                            stat(PLAYLISTS_DIR, &current_dir) == 0 &&
                            job_dir.st_dev == current_dir.st_dev && job_dir.st_ino == current_dir.st_ino;
    bool cache_bound = false;
    if (same_destination) {
        int parent = openat(completion.output_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) { cache_bound = path_cache_bind_directory_fd(parent); close(parent); }
    }
    bool result = completion.worker_ok && same_destination && cache_bound && metadata_db_storage_current();
    if (result) { char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", PLAYLISTS_DIR, completion.leaf); metadata_db_playlist_insert_one(path); }
    if (done) *done = true;
    if (ok) *ok = result;
    player_queue_storage_save_release_completed();
    return true;
}
