#include "player_queue_resume.h"
#include "../screen/gui_player.h"
#include "../screen/gui_player_internal.h"
#include "../../application/gui.h"
#include "../../shell/gui_navigation.h"
#include "player_queue_model.h"
#include "player_queue_storage.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../../hardware/device/headphone_status.h"
#include "../../../hardware/power/battery.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/playlists/queue_resume.h"
#include "../../../library/remote/remote_track.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

static bool resume_playlist_needs_lazy_order = false;
static file_browser_index_t *resume_file_index = NULL;
#ifdef HOST_BUILD
#define QUEUE_RESUME_PATH "./.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "./music/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "./music/.compas"
#else
#define QUEUE_RESUME_PATH "/usr/data/.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "/data/mnt/sd_0/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "/data/mnt/sd_0/.compas"
#endif

/* A lazy All-Songs queue has no paths until playlist_path_at() resolves them.
 * Persisting one above this bound would materialize and strdup the whole
 * library, which is unbounded work for a periodic checkpoint on the target.
 * The settings last_track/last_source_kind resume path remains available. */
#define LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES 32768

static atomic_bool sd_card_absent_immediate = false;

void gui_player_notify_sd_unmounted_immediate(void) {
    atomic_store(&sd_card_absent_immediate, true);
}

/* Call on every poll that currently observes the SD card mounted, regardless
 * of whether a full confirmed mount/unmount transition fires -- a transient
 * one-poll blip also calls gui_player_notify_sd_unmounted_immediate() above,
 * and this is what un-sticks the resulting flag for a card that was never
 * actually removed. */
void gui_player_notify_sd_mounted(void) {
    atomic_store(&sd_card_absent_immediate, false);
}

static bool is_sd_card_path(const char * path) {
    if (!path || !path[0]) return false;
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0) return false;
    return path[root_len] == '/' || path[root_len] == '\0';
}

static queue_resume_t restored_queue;
static bool have_restored_queue;

static atomic_bool checkpoint_urgent_pending;

static bool queue_checkpoint_is_large_lazy(void) {
    return player_queue_model_has_lazy_order() &&
           player_queue_model_count() > LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES;
}

static int open_queue_dir(const char * path) {
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

int open_sd_queue_dir(void) {
    /* Pin the root mount before creating/opening the child.  All later worker
     * operations use this descriptor, so a card replacement cannot redirect
     * rename/unlink to the new card. */
    int rootfd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

int open_queue_dir_from_snapshot(const metadata_db_snapshot_t *snapshot) {
    int dbfd = metadata_db_snapshot_dup_directory_fd(snapshot);
    if (dbfd < 0) return -1;
    int rootfd = openat(dbfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(dbfd);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

int open_queue_dir_from_file_index(const file_browser_index_t *index) {
    const char *dir = file_browser_index_directory(index);
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (!dir || strncmp(dir, MUSIC_ROOT_DIR, root_len) != 0 ||
        (dir[root_len] != '/' && dir[root_len] != '\0')) return -1;
    int fd = file_browser_index_dup_directory_fd(index);
    if (fd < 0) return -1;
    const char *suffix = dir + root_len;
    unsigned levels = 0;
    for (const char *p = suffix; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        levels++;
        while (*p && *p != '/') p++;
    }
    for (unsigned i = 0; i < levels; i++) {
        int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) { close(fd); return -1; }
        close(fd); fd = parent;
    }
    (void) mkdirat(fd, COMPAS_STORAGE_DIR, 0755);
    int queuefd = openat(fd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(fd);
    return queuefd;
}

static int open_checkpoint_target_dir(const char * path) {
    char dir[PATH_MAX];
    const char * slash = strrchr(path, '/');
    if (!slash) return open_queue_dir(".");
    size_t n = (size_t) (slash - path);
    if (n == 0) { dir[0] = '/'; dir[1] = 0; }
    else if (n < sizeof(dir)) { memcpy(dir, path, n); dir[n] = 0; }
    else return -1;
    return open_queue_dir(dir);
}

void queue_pin_sd_dir_if_needed(void) {
    if (player_queue_model_has_sd_dirfd_pin()) return;
    int count = player_queue_model_count();
    for (int i = 0; i < count; i++) {
        const char *path = player_queue_model_path_if_resolved(i);
        if (path && is_sd_card_path(path)) {
            player_queue_model_adopt_sd_dirfd(open_sd_queue_dir());
            return;
        }
    }
}

/* Deduplication state belongs to the UI thread. The worker reports failure
 * separately; checkpoint revision lanes need no cross-thread atomics. */
static uint64_t checkpoint_revision_internal;
static double checkpoint_position_internal = -1;
static uint64_t checkpoint_revision_sd;
static double checkpoint_position_sd = -1;
/* Hash of the non-path file layout at the last full write (0 = none), so an
 * unchanged queue only rewrites the file header with the new position. */
static uint64_t checkpoint_layout_internal;
static uint64_t checkpoint_layout_sd;

bool gui_player_queue_write_busy(void) {
    return player_queue_storage_checkpoint_busy();
}

/* UI code may poll this and present one persistent status indicator; the flag
 * is deliberately level-triggered so a failed read-only card does not create
 * a toast on every periodic checkpoint attempt. */
bool gui_player_queue_checkpoint_failed(void) {
    return player_queue_storage_checkpoint_failed();
}

void gui_player_queue_checkpoint(void) {
    if (queue_checkpoint_is_large_lazy()) {
        /* Set this before observing the worker: if it is just finishing, the
         * worker will remove anything it wrote; the reap path below covers the
         * race where it has already published completion. */
        player_queue_storage_checkpoint_defer_unlink();
        atomic_store(&checkpoint_urgent_pending, false);
        if (!player_queue_storage_checkpoint_reap_if_done(NULL, NULL)) return;
        int internal_dirfd = open_checkpoint_target_dir(QUEUE_RESUME_PATH);
        if (internal_dirfd >= 0) { player_queue_storage_remove_checkpoint_at(internal_dirfd, "queue.bin"); close(internal_dirfd); }
        player_queue_model_remove_sd_checkpoint("queue.bin");
        checkpoint_revision_internal = 0;
        checkpoint_position_internal = -1;
        checkpoint_layout_internal = 0;
        checkpoint_layout_sd = 0;
        checkpoint_revision_sd = 0;
        checkpoint_position_sd = -1;
        player_queue_storage_checkpoint_clear_deferred_unlink();
        return;
    }

    bool completed, failed;
    if (!player_queue_storage_checkpoint_reap_if_done(&completed, &failed)) return;
    if (completed && failed) {
        checkpoint_revision_internal = 0;
        checkpoint_revision_sd = 0;
    }
    if (player_queue_storage_checkpoint_take_header_fallback()) {
        checkpoint_revision_internal = 0;
        checkpoint_revision_sd = 0;
    }
    player_queue_storage_checkpoint_clear_deferred_unlink();
    if (!gui_player_has_active_track()) return;
    double position = deferred_resume_pending ? deferred_resume_position : audio_get_resume_position_seconds();

    bool is_sd_persistable = true;
    int queue_count = player_queue_model_count();
    for (int i = 0; i < queue_count; i++) {
        const char *path = player_queue_model_path_at(i);
        if (!path || !path[0] || !is_sd_card_path(path) ||
            player_queue_model_is_stream_cache_path(path)) {
            is_sd_persistable = false;
            break;
        }
    }

    bool sd_mounted = sd_card_root_is_mounted() && !atomic_load(&sd_card_absent_immediate);

    const char * target_path = QUEUE_RESUME_PATH;
    const char * target_name = "queue.bin";
    int target_dirfd = open_checkpoint_target_dir(target_path);
    uint64_t * last_rev = &checkpoint_revision_internal;
    double * last_pos = &checkpoint_position_internal;
    uint64_t * last_layout = &checkpoint_layout_internal;

    if (is_sd_persistable && sd_mounted) {
        target_name = "queue.bin";
        target_path = SD_QUEUE_RESUME_PATH;
        if (target_dirfd >= 0) close(target_dirfd);
        /* A restored queue.bin fills playlist[] without going through
         * on_file_selected(), which is the only place that used to pin this
         * directory. Leaving it at -1 made every boot checkpoint fail before
         * any write, and the toast blamed a read-only card. */
        if (!player_queue_model_has_sd_dirfd_pin())
            player_queue_model_adopt_sd_dirfd(open_sd_queue_dir());
        target_dirfd = player_queue_model_dup_sd_dirfd();
        last_rev = &checkpoint_revision_sd;
        last_pos = &checkpoint_position_sd;
        last_layout = &checkpoint_layout_sd;
    }

    uint64_t revision = player_queue_model_revision();
    if (revision == *last_rev && fabs(position - *last_pos) < 1.0) { if (target_dirfd >= 0) close(target_dirfd); return; }
    if (target_dirfd < 0) { *last_rev = 0; player_queue_storage_checkpoint_mark_failed(); return; }

    queue_resume_header_t header;
    uint64_t layout = 0;
    bool have_header = player_queue_model_checkpoint_header(current_settings.play_mode, position, &header, &layout);
    if (have_header && revision == *last_rev && layout == *last_layout) {
        /* Same queue as the file on disk: no path copy, just the new position. */
        *last_pos = position;
        if (!player_queue_storage_checkpoint_start_header(header, target_dirfd, target_name)) *last_rev = 0;
        return;
    }

    queue_resume_t s = {0};
    if (!player_queue_model_checkpoint_snapshot(current_settings.play_mode, position, &s)) {
        close(target_dirfd); return;
    }
    *last_rev = revision;
    *last_pos = position;
    *last_layout = have_header ? layout : 0;

    if (!player_queue_storage_checkpoint_start(s, target_dirfd, target_name)) *last_rev = 0;
}

/* Like gui_player_queue_checkpoint(), but guarantees the request is not silently
 * dropped if a previous checkpoint's background worker is still writing: it marks
 * an urgent-pending flag instead, which gui_player_queue_poll_urgent() below
 * retries on the very next UI poll tick once the in-flight worker finishes,
 * rather than waiting for the next scheduled 30-second/1-second periodic tick. */
void gui_player_queue_checkpoint_urgent(void) {
    if (player_queue_storage_checkpoint_busy()) {
        atomic_store(&checkpoint_urgent_pending, true);
        return;
    }
    gui_player_queue_checkpoint();
}

/* Called every UI poll tick (gui_queue_poll()) to retry a checkpoint request that
 * gui_player_queue_checkpoint_urgent() deferred because a worker was already
 * in flight. No-op unless a retry is actually pending. */
void gui_player_queue_poll_urgent(void) {
    if (!atomic_load(&checkpoint_urgent_pending)) return;
    if (player_queue_storage_checkpoint_busy()) return; /* still busy */
    atomic_store(&checkpoint_urgent_pending, false);
    gui_player_queue_checkpoint();
}

void gui_player_queue_flush(void) {
    player_queue_storage_checkpoint_join();
    gui_player_queue_checkpoint();
    player_queue_storage_checkpoint_join();
}

void gui_player_handle_sd_unmount(void) {
    atomic_store(&sd_card_absent_immediate, true);
    checkpoint_revision_sd = 0;
    checkpoint_position_sd = -1;

    /* Deliberately do NOT join the queue-storage worker here, even if one is
     * still running: this runs on the UI thread (via poll_sd_card_hotplug()'s
     * 500ms timer), and the worker may be blocked inside a write()/fsync()
     * to the very card that's being pulled out from under it -- joining
     * unconditionally here would risk freezing the whole UI on a stalled/
     * hung SD write. This is safe to skip: the worker only ever touches its
     * own deep-copied queue_resume_t snapshot (built synchronously before
     * pthread_create()), never the live playlist[] this function is
     * about to free, so there's no actual data race to guard against by
     * waiting for it here. It will finish (or fail harmlessly, retried on
     * the next checkpoint) on its own; the service's nonblocking worker
     * status is rechecked by the next call to gui_player_queue_checkpoint(). */

    /* Lazy song queues belong to the SD database even when no path has been
     * resolved yet. Never resolve every entry of a 300K queue on the UI
     * thread just to decide whether it must be cleared. */
    bool has_sd_playlist = player_queue_model_has_lazy_order() ||
        player_queue_model_has_resolved_sd_path(MUSIC_ROOT_DIR);
    if (!has_sd_playlist) return;

    const char *cur_path = gui_player_get_current_track_path();
    if (is_sd_card_path(cur_path)) {
        audio_stop();
        plugin_manager_notify_stopped();
    }
    free_playlist();
    clear_player_source();
    player_show_empty_queue();
    if (lv_screen_active() == gui_player_get_screen()) nav_pop();
}

bool build_sd_card_resume_playlist(char *** out_playlist, int * out_count, int * out_index, double * out_position) {
    resume_playlist_needs_lazy_order = false;
    queue_resume_free(&restored_queue);
    have_restored_queue = false;

    if (!queue_resume_read(SD_QUEUE_RESUME_PATH, &restored_queue)) return false;

    if (restored_queue.count <= 0 || restored_queue.current < 0 || restored_queue.current >= restored_queue.count) {
        queue_resume_free(&restored_queue);
        return false;
    }

    bool all_sd = true;
    for (int i = 0; i < restored_queue.count; i++) {
        const char * path = restored_queue.paths[i];
        if (!path || !path[0] || !is_sd_card_path(path) ||
            player_queue_model_is_stream_cache_path(path)) {
            all_sd = false;
            break;
        }
    }
    if (!all_sd) {
        queue_resume_free(&restored_queue);
        return false;
    }

    *out_playlist = restored_queue.paths;
    *out_count = restored_queue.count;
    *out_index = restored_queue.current;
    if (out_position) *out_position = restored_queue.position;
    restored_queue.paths = NULL;
    have_restored_queue = true;
    return true;
}

int gui_player_boot_resume_mode(void) {
    if (!current_settings.car_mode_enabled) return current_settings.resume_mode;
    if (current_settings.car_mode_autoresume_enabled &&
        battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED &&
        get_headphone_state() != HEADPHONE_STATE_NONE) return 1;
    /* Keep the saved position ready for Play without starting the output. */
    return 2;
}

bool gui_player_restore_sd_queue(bool is_boot) {
    atomic_store(&sd_card_absent_immediate, false);

    if (is_boot) {
        /* This can be called a second time shortly after boot -- once from
         * gui_init() directly, and again from poll_sd_card_hotplug()'s
         * boot-time-mount-race recheck (which covers the case where the
         * card wasn't mounted yet when gui_init() ran) -- both with
         * is_boot=true. Block on ANY already-loaded track, not just an
         * SD-based one: an SD-based track means the first call (or the
         * pre-existing car-mode/resume_mode fallback) already handled it,
         * so re-running here would free_playlist() and restart playback
         * moments after it had already started correctly -- but a NON-SD
         * track is just as much a reason to skip (e.g. the user started a
         * Subsonic stream while the card was out, reinserted it -- the
         * confirmed-reinsertion path already correctly preserved that
         * session, is_boot=true must not then clobber it on the very next
         * poll just because it doesn't recognize the path as "already SD").
         * Only actually restore here if nothing is loaded at all -- that's
         * the genuine late-mount case this second call exists for. */
        if (gui_player_has_active_track()) return false;
    } else {
        /* Preserve any loaded non-SD session, not just an actively-playing
         * one -- a paused (or deferred-pending) stream is just as much a
         * session the user cares about and doesn't want silently replaced
         * by a newly-inserted card's queue. */
        if (gui_player_has_active_track()) {
            const char * cur = gui_player_get_current_track_path();
            if (!is_sd_card_path(cur)) return false;
        }
    }

    char ** resume_playlist = NULL;
    int resume_count = 0, resume_index = 0;
    double position = 0.0;
    if (!build_sd_card_resume_playlist(&resume_playlist, &resume_count, &resume_index, &position)) return false;

    /* install_saved_resume_playlist() only swaps the queue bookkeeping
     * (free_playlist() + replace playlist[]/player_queue_model_count()) -- it never
     * touches the audio subsystem. The guards above block the common cases
     * where something is still active, but the is_boot=false path
     * deliberately still allows through an active track that's already
     * SD-based (a genuinely unusual leftover, since gui_player_handle_sd_unmount()
     * normally clears that on confirmed unmount) -- if that's somehow still
     * playing here, stop it first so playback and the newly-installed queue
     * never disagree about what's current. */
    if (gui_player_has_active_track() && audio_is_playing()) audio_stop();

    if (!install_saved_resume_playlist(resume_playlist, resume_count)) return false;

    int resume_mode = is_boot ? gui_player_boot_resume_mode() : current_settings.resume_mode;
    if (resume_mode == 1) {
        play_track_at_from(resume_index, position);
    } else if (resume_mode == 2) {
        prepare_deferred_resume(resume_index, position);
    } else {
        /* Resume disabled: load and display the queue/track, but don't
         * resume playback position -- deferred_resume_pending still needs
         * to be set (position 0, not the checkpoint's) so a later Play
         * actually starts this track from the top, matching the existing
         * "queue loaded, not playing" pattern used elsewhere in this file.
         * Without it, player_queue_model_current() is set but nothing is "current" in the
         * audio subsystem yet, so toggle_play_pause() would be a no-op. */
        player_queue_model_select_physical(resume_index);
        request_track_metadata(resume_index, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
    }
    return true;
}
bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index) {
    resume_playlist_needs_lazy_order = false;
    if (resume_file_index) {
        file_browser_index_close(resume_file_index);
        resume_file_index = NULL;
    }
    queue_resume_free(&restored_queue);
    have_restored_queue = false;
    if (queue_resume_read(QUEUE_RESUME_PATH, &restored_queue)) {
        bool local = true;
        for (int i = 0; i < restored_queue.count; i++) {
            const char * path = restored_queue.paths[i];
            if (remote_track_path_is_remote(path) || player_queue_model_is_stream_cache_path(path))
                local = false;
        }
        if (local && strcmp(restored_queue.paths[restored_queue.current], current_settings.last_track) == 0) {
            *out_playlist = restored_queue.paths;
            *out_count = restored_queue.count;
            *out_index = restored_queue.current;
            restored_queue.paths = NULL;
            current_settings.last_position = restored_queue.position;
            have_restored_queue = true;
            return true;
        }
        queue_resume_free(&restored_queue);
    }
    song_row_t current;
    bool indexed = metadata_db_get_song_by_path(current_settings.last_track, &current);

    if (indexed && current_settings.last_source_kind == 2 && current_settings.last_source_name[0] &&
        strcasecmp(current.tags.album, current_settings.last_source_name) == 0) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, NULL, current.tags.album_artist, current.tags.album, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            int count = (int) count64;
            group_song_entry_t * entries = calloc((size_t) count, sizeof(*entries));
            char ** paths = calloc((size_t) count, sizeof(*paths));
            song_row_t rows[64];
            int loaded = 0, selected = -1;
            while (entries && paths && loaded < count) {
                int want = count - loaded;
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, NULL, current.tags.album_artist,
                                                               current.tags.album, NULL, loaded, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    entries[loaded + i].path = strdup(rows[i].path);
                    entries[loaded + i].title = strdup(title);
                    paths[loaded + i] = strdup(rows[i].path);
                    if (strcmp(rows[i].path, current_settings.last_track) == 0) selected = loaded + i;
                    if (!entries[loaded + i].path || !entries[loaded + i].title || !paths[loaded + i]) {
                        got = 0;
                        break;
                    }
                }
                if (got <= 0) break;
                loaded += got;
                if (got < want) break;
            }
            if (loaded == count && selected >= 0) {
                set_player_source_group_songs_direct(entries, count, current.tags.album, selected);
                free_group_song_entries(entries, count);
                *out_playlist = paths;
                *out_count = count;
                *out_index = selected;
                return true;
            }
            if (entries) free_group_song_entries(entries, loaded);
            if (paths) {
                for (int i = 0; i < loaded; i++) free(paths[i]);
                free(paths);
            }
        }
    } else if (indexed && current_settings.last_source_kind == 1) {
        int64_t count64 = metadata_db_get_song_count();
        int64_t selected64 = metadata_db_get_song_title_offset(current_settings.last_track);
        if (count64 > 0 && count64 <= INT_MAX && selected64 >= 0 && selected64 < count64) {
            *out_playlist = calloc((size_t) count64, sizeof(char *));
            if (!*out_playlist) return false;
            *out_count = (int) count64;
            *out_index = (int) selected64;
            resume_playlist_needs_lazy_order = true;
            set_player_source_all_songs(*out_index);
            return true;
        }
    }

    /* A Files folder, including one too large to checkpoint, reopens its
     * directory index. Do not fall through to a full path allocation. */
    file_browser_index_t *folder = NULL;
    unsigned playable = 0, selected = 0;
    if (!current_settings.last_track[0] ||
        !file_browser_open_lazy_directory(current_settings.last_track, &folder, &playable, &selected) ||
        playable == 0 || playable > (unsigned) INT_MAX) {
        if (folder) file_browser_index_close(folder);
        return false;
    }
    char **slots = calloc(playable, sizeof(*slots));
    if (!slots) {
        file_browser_index_close(folder);
        return false;
    }
    *out_playlist = slots;
    *out_count = (int) playable;
    *out_index = (int) selected;
    resume_file_index = folder;
    return true;
}

bool install_saved_resume_playlist(char ** resume_playlist, int resume_count) {
    file_browser_index_t *file_index = resume_file_index;
    resume_file_index = NULL;
    int * new_order = NULL;
    if (resume_playlist_needs_lazy_order || file_index) {
        new_order = malloc(sizeof(int) * (size_t) resume_count);
        if (!new_order) {
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            if (file_index) file_browser_index_close(file_index);
            return false;
        }
        for (int i = 0; i < resume_count; i++) new_order[i] = i;
    }
    metadata_db_snapshot_t * resume_snapshot = NULL;
    if (resume_playlist_needs_lazy_order && !file_index) {
        resume_snapshot = metadata_db_snapshot_open(false);
        if (!resume_snapshot || metadata_db_snapshot_count(resume_snapshot) != resume_count) {
            if (resume_snapshot) metadata_db_snapshot_close(resume_snapshot);
            free(new_order);
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            return false;
        }
    }
    player_queue_install_t owned = player_queue_install_new(resume_playlist, resume_count);
    owned.lazy_ranks = new_order;
    owned.snapshot = resume_snapshot;
    owned.file_index = file_index;
    if (have_restored_queue) {
        owned.pending_count = restored_queue.pending;
        owned.next_insert_index = owned.pending_count
            ? restored_queue.current + 1 + owned.pending_count : -1;
        owned.shuffle_order = restored_queue.order;
        owned.shuffle_continuation = restored_queue.continuation;
        owned.shuffle_pos = restored_queue.shuffle_pos;
        restored_queue.order = NULL;
        restored_queue.continuation = NULL;
        current_settings.play_mode = restored_queue.mode;
        queue_resume_free(&restored_queue);
        have_restored_queue = false;
    }
    free_playlist();
    owned.queue_sd_dirfd = file_index ? open_queue_dir_from_file_index(file_index)
        : (resume_snapshot ? open_queue_dir_from_snapshot(resume_snapshot) : -1);
    player_queue_model_install_take(owned);
    return true;
}

void prepare_deferred_resume(int index, double start_seconds) {
    player_queue_model_select_physical(index);
    set_play_button_state(false);
    deferred_resume_pending = true;
    deferred_resume_position = start_seconds;
    request_track_metadata(index, false, true, false);

    nav_push(gui_player_get_screen());
}
