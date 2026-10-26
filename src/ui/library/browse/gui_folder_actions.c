#include "gui_folder_actions.h"

#include "../../application/gui.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../player/screen/gui_player.h"
#include "../playlists/gui_playlist_target.h"
#include "../../text/i18n/i18n.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/playlists/playlist_files.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define FILE_FOLDER_QUEUE_BUDGET (512u * 1024u)
#define FILE_FOLDER_QUEUE_MAX_SONGS 4096u

typedef struct {
    file_browser_index_t * index;
    char path[PATH_MAX];
    unsigned count;
    atomic_int * progress;
    atomic_bool * cancel;
} folder_playlist_provider_t;

typedef struct {
    pthread_t thread;
    atomic_bool done;
    gui_folder_action_t action;
    char source_dir[PATH_MAX];
    char destination[PATH_MAX];
    char playlist_name[201];
    char created_path[PATH_MAX];
    bool ok;
    bool oversized;
    bool playback_changed;
    file_browser_index_t * index;
    unsigned playable_count;
    const char ** paths;
    size_t staging_bytes;
    atomic_bool cancel;
    atomic_int progress;
    gui_busy_handle_t busy_handle;
    lv_obj_t * return_screen;
    uint64_t playback_intent_epoch;
    uint64_t playlist_target_generation;
    bool picker_storage_invalidated;
    uint64_t picker_storage_invalidation_generation;
} file_folder_job_t;

static lv_obj_t * file_folder_menu_popup;
static lv_obj_t * file_folder_menu_backdrop;
static lv_obj_t * file_folder_menu_page_row;
static char file_folder_menu_path[PATH_MAX];
static file_folder_job_t * file_folder_job;
static lv_timer_t * file_folder_job_timer;
static gui_folder_storage_invalidated_cb_t storage_invalidated_cb;
static gui_folder_open_picker_cb_t open_picker_cb;
static gui_folder_page_jump_cb_t page_jump_cb;
static bool file_folder_job_storage_invalidated(void);
static void file_folder_add_playlist_cb(lv_event_t * e);
static bool file_folder_job_start(gui_folder_action_t action, const char * source_dir,
                                  const char * destination, const char * playlist_name,
                                  uint64_t target_generation, lv_obj_t * return_screen);
static void file_folder_job_complete(void);
static void file_folder_job_poll(lv_timer_t * timer);
static bool file_folder_start_menu_job(gui_folder_action_t action, const char *source_dir);

static bool folder_playlist_path_at(void * context, int item, const char ** path) {
    folder_playlist_provider_t * provider = context;
    if (!provider || item < 0 || (provider->cancel && atomic_load(provider->cancel)) ||
        !file_browser_index_playable_path_at(provider->index, (unsigned)item,
                                             provider->path, sizeof(provider->path))) return false;
    *path = provider->path;
    if (provider->progress && provider->count)
        atomic_store(provider->progress, 20 + (int)(((uint64_t)(unsigned)item + 1) * 70u / provider->count));
    return true;
}

static bool folder_playlist_write(const char * source_dir, const char * name, const char * existing_path,
                                  char * out_path, size_t out_size, atomic_bool * cancel,
                                  atomic_int * progress) {
    file_browser_index_t * index = NULL;
    unsigned listed_count = 0;
    if (!source_dir || !source_dir[0] ||
        !file_browser_index_open_cancelable(source_dir, &index, &listed_count, cancel)) {
        if (index) file_browser_index_close(index);
        return false;
    }
    unsigned count = file_browser_index_playable_count(index);
    if (!count || count > INT_MAX) { file_browser_index_close(index); return false; }
    if (progress) atomic_store(progress, 20);
    folder_playlist_provider_t provider = {
        .index = index, .count = count, .progress = progress, .cancel = cancel
    };
    bool ok;
    if (existing_path) {
        ok = playlist_files_append_stream(existing_path, (int)count, folder_playlist_path_at, &provider);
        if (ok && out_path && strlen(existing_path) < out_size)
            snprintf(out_path, out_size, "%s", existing_path);
        else if (ok && out_path) ok = false;
    } else {
        ok = playlist_files_write_new_stream(PLAYLISTS_DIR, name, (int)count,
                                             folder_playlist_path_at, &provider,
                                             out_path, out_size);
    }
    if (progress && ok) atomic_store(progress, 95);
    file_browser_index_close(index);
    return ok;
}

static void hide_file_folder_menu(void) {
    if (file_folder_menu_popup) lv_obj_add_flag(file_folder_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (file_folder_menu_backdrop) lv_obj_add_flag(file_folder_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void invalidate_file_folder_menu(void) {
    hide_file_folder_menu();
    file_folder_menu_path[0] = '\0';
}

static void file_folder_job_free(file_folder_job_t * job) {
    if (!job) return;
    for (unsigned i = 0; i < job->playable_count && job->paths; i++) free((void *)job->paths[i]);
    free(job->paths);
    if (job->index) file_browser_index_close(job->index);
    free(job);
}

static void * file_folder_job_worker(void * argument) {
    file_folder_job_t * job = argument;
    if (job->action == GUI_FOLDER_ACTION_PLAYLIST_APPEND) {
        job->ok = folder_playlist_write(job->source_dir, NULL, job->destination, NULL, 0,
                                        &job->cancel, &job->progress);
        atomic_store_explicit(&job->done, true, memory_order_release);
        return NULL;
    }
    if (job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE) {
        job->ok = folder_playlist_write(job->source_dir, job->playlist_name, NULL,
                                        job->created_path, sizeof(job->created_path),
                                        &job->cancel, &job->progress);
        atomic_store_explicit(&job->done, true, memory_order_release);
        return NULL;
    }
    unsigned listed_count = 0;
    if (!file_browser_index_open_cancelable(job->source_dir, &job->index, &listed_count,
                                            &job->cancel)) goto done;
    atomic_store(&job->progress, 10);
    job->playable_count = file_browser_index_playable_count(job->index);
    if (!job->playable_count || job->playable_count > INT_MAX) goto done;

    if (job->action == GUI_FOLDER_ACTION_QUEUE) {
        if (job->playable_count > FILE_FOLDER_QUEUE_MAX_SONGS ||
            (size_t)job->playable_count > FILE_FOLDER_QUEUE_BUDGET /
                (3 * sizeof(char *) + sizeof(int))) {
            job->oversized = true;
            goto done;
        }
        size_t path_bytes = 0;
        for (unsigned i = 0; i < job->playable_count; i++) {
            if (atomic_load(&job->cancel)) goto done;
            char path[PATH_MAX];
            if (!file_browser_index_playable_path_at(job->index, i, path, sizeof(path))) goto done;
            size_t len = strlen(path) + 1;
            if (len > FILE_FOLDER_QUEUE_BUDGET / 2 || path_bytes > FILE_FOLDER_QUEUE_BUDGET / 2 - len) {
                job->oversized = true;
                goto done;
            }
            path_bytes += len;
        }
        /* Staging path pointers + queue copies/growth + lazy-order growth. */
        size_t pointers = (size_t)job->playable_count * (4 * sizeof(char *) + sizeof(int));
        if (path_bytes * 2 + pointers > FILE_FOLDER_QUEUE_BUDGET) { job->oversized = true; goto done; }
        job->staging_bytes = path_bytes + (size_t)job->playable_count * sizeof(char *);
        job->paths = calloc(job->playable_count, sizeof(*job->paths));
        if (!job->paths) goto done;
        for (unsigned i = 0; i < job->playable_count; i++) {
            if (atomic_load(&job->cancel)) goto done;
            char path[PATH_MAX];
            if (!file_browser_index_playable_path_at(job->index, i, path, sizeof(path)) ||
                !(job->paths[i] = strdup(path))) goto done;
            atomic_store(&job->progress,
                         10 + (int)(((uint64_t)(i + 1) * 80u) / job->playable_count));
        }
        job->ok = true;
    } else {
        atomic_store(&job->progress, 95);
        job->ok = true; /* Keep the retained index for the UI thread to install. */
    }
done:
    if (job->action == GUI_FOLDER_ACTION_QUEUE || !job->ok) {
        if (job->index) { file_browser_index_close(job->index); job->index = NULL; }
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static void file_folder_play_sequential_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_file_folder_menu();
    (void)file_folder_start_menu_job(GUI_FOLDER_ACTION_PLAY_SEQUENTIAL, file_folder_menu_path);
}

static void file_folder_play_shuffle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_file_folder_menu();
    (void)file_folder_start_menu_job(GUI_FOLDER_ACTION_PLAY_SHUFFLE, file_folder_menu_path);
}

static void file_folder_add_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_file_folder_menu();
    (void)file_folder_start_menu_job(GUI_FOLDER_ACTION_QUEUE, file_folder_menu_path);
}

static void file_folder_page_jump_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_file_folder_menu();
    if (page_jump_cb) page_jump_cb(file_folder_menu_path);
}

static void file_folder_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_file_folder_menu();
}

void gui_folder_actions_build_menu(void) {
    const menu_popup_row_t rows[] = {
        { TR("Play All (Sequential)"), file_folder_play_sequential_cb, false, false, NULL },
        { TR("Play All (Shuffle)"), file_folder_play_shuffle_cb, false, false, NULL },
        { TR("Add to Queue"), file_folder_add_queue_cb, false, false, NULL },
        { TR("Add to Playlist"), file_folder_add_playlist_cb, false, false, NULL },
        { TR("Go to page"), file_folder_page_jump_cb, false, false, NULL },
        { TR("Cancel"), file_folder_menu_backdrop_cb, false, true, NULL },
    };
    file_folder_menu_popup = build_menu_popup(rows, (int)(sizeof(rows) / sizeof(rows[0])),
                                              file_folder_menu_backdrop_cb,
                                              &file_folder_menu_backdrop);
    file_folder_menu_page_row = file_folder_menu_popup ? lv_obj_get_child(file_folder_menu_popup, 4) : NULL;
}

static void file_folder_add_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_file_folder_menu();
    if (open_picker_cb) open_picker_cb(file_folder_menu_path);
}

static bool file_folder_job_storage_invalidated(void) {
    return storage_invalidated_cb && storage_invalidated_cb();
}

static void file_folder_job_complete(void) {
    file_folder_job_t * job = file_folder_job;
    if (!job) return;
    if (file_folder_job_timer) { lv_timer_delete(file_folder_job_timer); file_folder_job_timer = NULL; }
    pthread_join(job->thread, NULL);
    bool stale = atomic_load_explicit(&job->cancel, memory_order_acquire) ||
                 file_folder_job_storage_invalidated();
    bool ok = job->ok && !stale;
    bool playlist_action = job->action == GUI_FOLDER_ACTION_PLAYLIST_APPEND ||
                           job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE;
    bool play_folder_action = job->action == GUI_FOLDER_ACTION_PLAY_SEQUENTIAL ||
                              job->action == GUI_FOLDER_ACTION_PLAY_SHUFFLE;
    gui_playlist_target_snapshot_t target;
    gui_playlist_target_copy(&target);
    bool same_picker_request = !stale && playlist_action &&
        job->playlist_target_generation == target.generation &&
        strcmp(target.folder_path, job->source_dir) == 0;
    bool stale_storage_picker_request = stale && playlist_action &&
        job->picker_storage_invalidated &&
        job->picker_storage_invalidation_generation == target.generation &&
        !target.folder_path[0] && !target.song_path[0];
    bool dismiss_picker = (ok && same_picker_request) || stale_storage_picker_request;
    /* Covered pickers can be removed, but an active picker must use nav_pop
     * after busy dismissal; deleting the active LVGL screen is unsafe. */
    bool dismiss_active_picker = false;
    if (dismiss_picker) {
        int depth = gui_navigation_get_depth();
        for (int i = 0; i < depth; i++) {
            if (gui_navigation_get_screen_at(i) != job->return_screen) continue;
            if (i == depth - 1 && lv_screen_active() == job->return_screen) {
                dismiss_active_picker = true;
            } else if (i < depth - 1) {
                nav_remove_stack_slot(i);
            }
            break;
        }
    }
    if (play_folder_action && ok) {
        bool context_current = gui_player_playback_intent_epoch() == job->playback_intent_epoch &&
            gui_navigation_is_top(gui_busy_get_screen()) &&
            gui_navigation_contains(job->return_screen);
        if (context_current) {
            /* Load Files synchronously before pushing Player so a pending
             * busy slide cannot later reload Files over it. */
            gui_busy_hide_immediate(job->busy_handle);
            if (lv_screen_active() != job->return_screen) {
                ok = false;
                job->playback_changed = true;
            }
        } else {
            gui_busy_hide(job->busy_handle);
            ok = false;
            job->playback_changed = true;
        }
        if (ok) {
            file_browser_index_t * index = job->index;
            job->index = NULL;
            ok = gui_player_play_folder_index(index, job->playable_count,
                job->action == GUI_FOLDER_ACTION_PLAY_SHUFFLE
                    ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
        }
    } else {
        gui_busy_hide(job->busy_handle);
    }
    if (dismiss_picker && dismiss_active_picker &&
        gui_navigation_is_top(job->return_screen) &&
        lv_screen_active() == job->return_screen) {
        nav_pop();
    }
    if (ok && job->action == GUI_FOLDER_ACTION_QUEUE) {
        ok = gui_player_queue_try_add_many_bounded(job->paths, (int)job->playable_count,
                                                   job->staging_bytes,
                                                   FILE_FOLDER_QUEUE_BUDGET);
    }
    if (ok && job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE)
        metadata_db_playlist_insert_one(job->created_path);

    if (playlist_action) {
        if (!stale)
            show_error_toast(ok ? (job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE
                                      ? TR("Playlist created") : TR("Added folder to playlist"))
                                : TR("Failed to add folder to playlist"));
        if (ok && same_picker_request)
            gui_playlist_target_clear_folder();
    } else if (!ok && !stale) {
        show_error_toast(job->oversized ? TR("Folder is too large to add to queue")
            : (job->playback_changed ? TR("Playback changed before folder was ready")
            : (job->action == GUI_FOLDER_ACTION_QUEUE ? TR("Cannot add folder within queue memory limit")
                                                      : TR("Cannot play this folder"))));
    }
    file_folder_job = NULL;
    file_folder_job_free(job);
}

static void file_folder_job_poll(lv_timer_t * timer) {
    (void)timer;
    if (!file_folder_job) return;
    if (file_folder_job_storage_invalidated()) {
        gui_folder_actions_invalidate_storage_targets();
        gui_folder_actions_cancel();
    }
    gui_busy_set_progress(file_folder_job->busy_handle, atomic_load(&file_folder_job->progress));
    if (atomic_load_explicit(&file_folder_job->done, memory_order_acquire) &&
        !gui_navigation_transition_in_progress()) file_folder_job_complete();
}

static bool file_folder_job_start(gui_folder_action_t action, const char * source_dir,
                                  const char * destination, const char * playlist_name,
                                  uint64_t target_generation, lv_obj_t * return_screen) {
    if (file_folder_job) { show_info_toast(TR("Folder action is still running")); return false; }
    file_folder_job_t * job = calloc(1, sizeof(*job));
    if (!job) { show_error_toast(TR("Cannot start folder action")); return false; }
    job->action = action;
    snprintf(job->source_dir, sizeof(job->source_dir), "%s", source_dir ? source_dir : "");
    snprintf(job->destination, sizeof(job->destination), "%s", destination ? destination : "");
    snprintf(job->playlist_name, sizeof(job->playlist_name), "%s", playlist_name ? playlist_name : "");
    atomic_init(&job->done, false);
    atomic_init(&job->cancel, false);
    atomic_init(&job->progress, 0);
    job->playback_intent_epoch = gui_player_playback_intent_epoch();
    job->playlist_target_generation = target_generation;
    job->return_screen = return_screen;
    if (!job->source_dir[0]) {
        file_folder_job_free(job);
        show_error_toast(TR("Cannot start folder action"));
        return false;
    }
    file_folder_job = job;
    const char * busy_title = action == GUI_FOLDER_ACTION_QUEUE ? TR("Adding folder to queue")
        : (action == GUI_FOLDER_ACTION_PLAYLIST_APPEND || action == GUI_FOLDER_ACTION_PLAYLIST_CREATE)
            ? TR("Adding folder to playlist") : TR("Preparing folder playback");
    job->busy_handle = gui_busy_show(busy_title, TR("Scanning playable files"));
    gui_busy_set_progress(job->busy_handle, 0);
    file_folder_job_timer = lv_timer_create(file_folder_job_poll, 100, NULL);
    if (!file_folder_job_timer) {
        file_folder_job = NULL;
        gui_busy_hide(job->busy_handle);
        file_folder_job_free(job);
        show_error_toast(TR("Cannot start folder action"));
        return false;
    }
    if (pthread_create(&job->thread, NULL, file_folder_job_worker, job) != 0) {
        lv_timer_delete(file_folder_job_timer);
        file_folder_job_timer = NULL;
        file_folder_job = NULL;
        gui_busy_hide(job->busy_handle);
        file_folder_job_free(job);
        show_error_toast(TR("Cannot start folder action"));
        return false;
    }
    return true;
}

static bool file_folder_start_menu_job(gui_folder_action_t action, const char *source_dir) {
    gui_playlist_target_snapshot_t target;
    gui_playlist_target_copy(&target);
    return file_folder_job_start(action, source_dir, NULL, NULL,
                                 target.generation, lv_screen_active());
}

bool gui_folder_actions_start_playlist_job(bool create, const char *source_dir,
                                           const char *destination, const char *playlist_name,
                                           uint64_t target_generation, lv_obj_t *return_screen) {
    return file_folder_job_start(create ? GUI_FOLDER_ACTION_PLAYLIST_CREATE
                                        : GUI_FOLDER_ACTION_PLAYLIST_APPEND,
                                 source_dir, destination, playlist_name,
                                 target_generation, return_screen);
}

void gui_folder_actions_init(gui_folder_storage_invalidated_cb_t storage_invalidated,
                             gui_folder_open_picker_cb_t open_picker,
                             gui_folder_page_jump_cb_t page_jump) {
    storage_invalidated_cb = storage_invalidated;
    open_picker_cb = open_picker;
    page_jump_cb = page_jump;
}

void gui_folder_actions_open_menu(const char *path, bool show_page_jump) {
    snprintf(file_folder_menu_path, sizeof(file_folder_menu_path), "%s", path ? path : "");
    if (!file_folder_menu_popup || !file_folder_menu_backdrop) return;
    if (file_folder_menu_page_row) {
        if (show_page_jump) lv_obj_remove_flag(file_folder_menu_page_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(file_folder_menu_page_row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(file_folder_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(file_folder_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(file_folder_menu_backdrop);
    lv_obj_move_foreground(file_folder_menu_popup);
}

void gui_folder_actions_menu_screen_unloaded_cb(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOADED) invalidate_file_folder_menu();
}

void gui_folder_actions_invalidate_storage_targets(void) {
    invalidate_file_folder_menu();
    gui_playlist_target_snapshot_t target;
    gui_playlist_target_copy(&target);
    if (!target.folder_path[0]) return;
    uint64_t invalidated_generation = target.generation + 1;
    if (file_folder_job &&
        (file_folder_job->action == GUI_FOLDER_ACTION_PLAYLIST_APPEND ||
         file_folder_job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE) &&
        file_folder_job->playlist_target_generation == target.generation &&
        strcmp(target.folder_path, file_folder_job->source_dir) == 0) {
        file_folder_job->picker_storage_invalidated = true;
        file_folder_job->picker_storage_invalidation_generation = invalidated_generation;
    }
    gui_playlist_target_clear_folder();
}

void gui_folder_actions_cancel(void) {
    if (file_folder_job)
        atomic_store_explicit(&file_folder_job->cancel, true, memory_order_release);
}

bool gui_folder_actions_busy(void) { return file_folder_job != NULL; }

bool gui_folder_actions_menu_visible(void) {
    return (file_folder_menu_popup && !lv_obj_has_flag(file_folder_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (file_folder_menu_backdrop && !lv_obj_has_flag(file_folder_menu_backdrop, LV_OBJ_FLAG_HIDDEN));
}

void gui_folder_actions_stop_work(void) {
    if (file_folder_job_timer) { lv_timer_delete(file_folder_job_timer); file_folder_job_timer = NULL; }
    if (file_folder_job) {
        file_folder_job_t * job = file_folder_job;
        atomic_store_explicit(&job->cancel, true, memory_order_release);
        pthread_join(job->thread, NULL);
        gui_busy_hide(job->busy_handle);
        if (job->ok && job->action == GUI_FOLDER_ACTION_PLAYLIST_CREATE)
            metadata_db_playlist_insert_one(job->created_path);
        file_folder_job = NULL;
        file_folder_job_free(job);
    }
}

void gui_folder_actions_teardown(void) {
    if (file_folder_menu_popup) { lv_obj_delete(file_folder_menu_popup); file_folder_menu_popup = NULL; }
    file_folder_menu_page_row = NULL;
    if (file_folder_menu_backdrop) { lv_obj_delete(file_folder_menu_backdrop); file_folder_menu_backdrop = NULL; }
    file_folder_menu_path[0] = '\0';
}
