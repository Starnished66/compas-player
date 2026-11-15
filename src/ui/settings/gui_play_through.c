#include "gui_play_through.h"
#include "gui_native_audio.h"
#include "../application/gui.h"
#include "../plugins/gui_plugin_library.h"
#include "../library/browse/gui_library_internal.h"
#include "../widgets/builders/screen_builders.h"
#include "../text/i18n/i18n.h"
#include "../../audio/engine/audio.h"
#include "../../library/browser/paths/library_paths.h"
#include "../../library/parsers/cue/cue_parser.h"
#include "../../core/persistence/settings.h"
#include "../../core/storage/storage_roots.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

static char ** candidates;
static int candidate_count, candidate_index;
static char ** batch;
static int batch_count;
static int stopped_polls;
static bool continuing;
static lv_timer_t * continuation_timer;
static lv_obj_t * play_through_screen;
static lv_obj_t * about_screen;

struct discovery {
    char * path, * artist, * album;
    bool albums, folders;
    metadata_db_album_sort_t sort;
    unsigned generation;
    char ** tracks;
    int count;
    atomic_bool done;
};
static unsigned discovery_generation;
static struct discovery * discovery_active, * discovery_queued;
static void continuation_tick(lv_timer_t * timer);

static void free_discovery(struct discovery * job) {
    if (!job) return;
    free(job->path);
    free(job->artist);
    free(job->album);
    gui_plugin_free_string_array(job->tracks, job->count);
    free(job);
}

static void clear_candidates(void) {
    ++discovery_generation;
    free_discovery(discovery_queued);
    discovery_queued = NULL;
    gui_plugin_free_string_array(candidates, candidate_count);
    candidates = NULL;
    candidate_count = candidate_index = 0;
    gui_plugin_free_string_array(batch, batch_count);
    batch = NULL;
    batch_count = stopped_polls = 0;
}

static int lexical_compare(const void * a, const void * b) {
    return strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

static bool audio_name(const char * name) {
    const char * dot = strrchr(name, '.');
    if (!dot) return false;
    static const char * const extensions[] = {
        "mp3", "flac", "wav", "aif", "aiff", "aifc", "dsf", "dff", "aac",
        "m4a", "m4b", "alac", "ape", "opus", "ogg", "wma"
    };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (!strcasecmp(dot + 1, extensions[i])) return true;
    return false;
}

/* Folder order deliberately follows the original plugin's lexical order. */
static char ** directory_paths(const char * directory, bool folders, int * count) {
    *count = 0;
    DIR * dir = opendir(directory);
    if (!dir) return NULL;
    char ** paths = NULL;
    struct dirent * entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!folders && !audio_name(entry->d_name)) continue;
        char * path = malloc(strlen(directory) + strlen(entry->d_name) + 2);
        if (!path) goto failed;
        sprintf(path, "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(path, &st) || (folders ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
            free(path);
            continue;
        }
        char ** grown = realloc(paths, ((size_t)*count + 1) * sizeof(*paths));
        if (!grown) { free(path); goto failed; }
        paths = grown;
        paths[(*count)++] = path;
    }
    closedir(dir);
    if (*count > 1) qsort(paths, *count, sizeof(*paths), lexical_compare);
    return paths;
failed:
    closedir(dir);
    gui_plugin_free_string_array(paths, *count);
    *count = 0;
    return NULL;
}

/* The queue's final file for a track: a cue reference counts only as its sheet's last track. */
static bool track_file(const char * path, char * file, size_t size) {
    if (!cue_reference_is(path)) return snprintf(file, size, "%s", path) < (int)size;
    cue_sheet_t sheet;
    unsigned ordinal;
    if (!cue_reference_resolve(path, &sheet, &ordinal)) return false;
    bool last = ordinal == (unsigned)sheet.track_count;
    cue_sheet_free(&sheet);
    return last && cue_reference_parse(path, file, size, &ordinal);
}

static bool cut_last_component(char * path) {
    char * slash = strrchr(path, '/');
    if (!slash || slash == path) return false;
    *slash = '\0';
    return true;
}

static bool path_below(const char * path, const char * root) {
    size_t length = strlen(root);
    return !strncmp(path, root, length) && path[length] == '/';
}

#define FOLDER_DESCENT_MAX 16 /* guards against symlink loops */

/* The audio of the first folder at or below dir, in lexical depth-first order. */
static char ** first_audio_in(const char * dir, int depth, int * count) {
    char ** tracks = directory_paths(dir, false, count);
    if (*count > 0 || depth <= 0) return tracks;
    int folders_count;
    char ** folders = directory_paths(dir, true, &folders_count);
    for (int i = 0; i < folders_count && *count == 0; ++i)
        tracks = first_audio_in(folders[i], depth - 1, count);
    gui_plugin_free_string_array(folders, folders_count);
    return tracks;
}

/* Audio of the first sibling after dir (inside parent) that has any. */
static char ** later_sibling_tracks(const char * dir, const char * parent, int * count) {
    *count = 0;
    int folders_count;
    char ** folders = directory_paths(parent, true, &folders_count);
    char ** tracks = NULL;
    bool found = false;
    for (int i = 0; i < folders_count && *count == 0; ++i) {
        if (found) tracks = first_audio_in(folders[i], FOLDER_DESCENT_MAX, count);
        else if (!strcmp(folders[i], dir)) found = true;
    }
    gui_plugin_free_string_array(folders, folders_count);
    return tracks;
}

/* When path is its folder's last track, the next folder's tracks: the following sibling,
 * else the parent's following sibling and so on, never above root. Outside root only
 * the first level is tried. */
static char ** next_folder_tracks(const char * path, const char * root, int * count) {
    *count = 0;
    char file[1024];
    char * dir = strdup(path);
    char * parent = strdup(path);
    char ** tracks = NULL;
    if (!dir || !parent || !track_file(path, file, sizeof(file)) ||
        !cut_last_component(dir) || !strcmp(dir, root)) goto done;
    int tracks_count;
    char ** own = directory_paths(dir, false, &tracks_count);
    bool last = tracks_count > 0 && !strcmp(own[tracks_count - 1], file);
    gui_plugin_free_string_array(own, tracks_count);
    if (!last) goto done;
    bool inside = path_below(dir, root);
    for (;;) {
        strcpy(parent, dir);
        if (!cut_last_component(parent)) break;
        tracks = later_sibling_tracks(dir, parent, count);
        if (*count > 0 || !inside || !path_below(parent, root)) break;
        strcpy(dir, parent);
    }
done:
    free(parent);
    free(dir);
    return tracks;
}

static bool sequential(void) {
    return !strcmp(gui_plugin_get_play_mode(), "sequential");
}

static void * discover_tracks(void * arg) {
    struct discovery * job = arg;
    if (job->albums && *job->artist && *job->album) {
        int count;
        char ** tracks = gui_plugin_get_album_tracks(job->artist, job->album, &count);
        bool last = count > 0 && !strcmp(tracks[count - 1], job->path);
        gui_plugin_free_string_array(tracks, count);
        if (last) job->tracks = gui_plugin_get_next_album_tracks_sorted(job->artist, job->album, job->sort, &job->count);
    }
    if (!job->tracks && job->folders) {
        storage_root_t roots[STORAGE_ROOTS_MAX];
        size_t count = storage_roots_list(roots, STORAGE_ROOTS_MAX), index;
        const char * root = storage_roots_find_path(roots, count, job->path, &index)
            ? roots[index].path : MUSIC_ROOT_DIR;
        job->tracks = next_folder_tracks(job->path, root, &job->count);
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

/* Only the UI publishes results; a single worker bounds concurrent scans. */
static void poll_discovery(void) {
    if (discovery_active && atomic_load_explicit(&discovery_active->done, memory_order_acquire)) {
        struct discovery * job = discovery_active;
        const char * path = gui_plugin_get_current_track_path();
        if (job->generation == discovery_generation && path && !strcmp(path, job->path) &&
            job->albums == current_settings.native_play_through_albums &&
            job->folders == current_settings.native_play_through_folders && sequential()) {
            candidates = job->tracks;
            candidate_count = job->count;
            job->tracks = NULL;
            job->count = 0;
        }
        free_discovery(job);
        discovery_active = NULL;
    }
    if (!discovery_active && discovery_queued) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, discover_tracks, discovery_queued) == 0) {
            discovery_active = discovery_queued;
            discovery_queued = NULL;
            pthread_detach(thread);
        }
    }
}

void gui_play_through_track_started(const char * artist, const char * album) {
    const char * path = gui_plugin_get_current_track_path();
    if (candidates && path) {
        for (int i = 0; i < batch_count; ++i)
            if (!strcmp(batch[i], path)) return;
    }
    clear_candidates();
    if (!sequential() || !path || strstr(path, "://") ||
        (!current_settings.native_play_through_albums && !current_settings.native_play_through_folders)) return;
    struct discovery * job = calloc(1, sizeof(*job));
    if (!job) return;
    job->path = strdup(path);
    job->artist = strdup(artist ? artist : "");
    job->album = strdup(album ? album : "");
    if (!job->path || !job->artist || !job->album) { free_discovery(job); return; }
    job->sort = requested_group_album_sort(METADATA_DB_GROUP_ARTIST);
    job->albums = current_settings.native_play_through_albums;
    job->folders = current_settings.native_play_through_folders;
    job->generation = discovery_generation;
    atomic_init(&job->done, false);
    discovery_queued = job;
    if (!continuation_timer) continuation_timer = lv_timer_create(continuation_tick, 1000, NULL);
    poll_discovery();
}

void gui_play_through_stopped(void) {
    if (!continuing) clear_candidates();
}

void gui_play_through_resumed(void) {
    stopped_polls = 0;
}

static void continue_now(void) {
    int count = candidate_count - candidate_index;
    if (count > 500) count = 500;
    if (count <= 0) return;
    gui_plugin_free_string_array(batch, batch_count);
    batch = calloc((size_t)count, sizeof(*batch));
    batch_count = 0;
    if (!batch) return;
    for (int i = 0; i < count; ++i) {
        batch[i] = strdup(candidates[candidate_index + i]);
        if (!batch[i]) { gui_plugin_free_string_array(batch, i); batch = NULL; return; }
    }
    batch_count = count;
    candidate_index += count;
    if (candidate_index == candidate_count) {
        gui_plugin_free_string_array(candidates, candidate_count);
        candidates = NULL;
        candidate_count = candidate_index = 0;
    }
    stopped_polls = 0;
    continuing = true;
    gui_plugin_play_paths((const char * const *)batch, count, 0, true);
    continuing = false;
    show_info_toast(TR("Continuing playback"));
}

void gui_play_through_queue_exhausted(int direction) {
    poll_discovery();
    if (direction == 1 && candidates && sequential()) continue_now();
}

static void continuation_tick(lv_timer_t * timer) {
    (void)timer;
    poll_discovery();
    if (!candidates || !sequential() || audio_is_playing() || audio_is_paused()) {
        stopped_polls = 0;
        return;
    }
    if (++stopped_polls >= 2) continue_now();
}

static void rearm(void) {
    char title[256], artist[256], album[256];
    double duration;
    clear_candidates();
    if (gui_plugin_get_now_playing(title, sizeof(title), artist, sizeof(artist), album, sizeof(album), &duration))
        gui_play_through_track_started(artist, album);
}

static void play_through_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool * setting = lv_event_get_user_data(e);
    *setting = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    rearm();
}

static void about_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!about_screen) {
        lv_obj_t * list;
        about_screen = build_subsonic_list_screen(TR("How it works"), NULL, &list);
        add_wrapped_section_header(list, TR("Albums continue in the artist's album order, then with the next artist."));
        add_wrapped_section_header(list, TR("Folders continue alphabetically, then with the next folder one level up."));
        add_wrapped_section_header(list, TR("Album mode has priority when both options are enabled."));
        add_wrapped_section_header(list, TR("Shuffle and repeat modes are left unchanged."));
    }
    nav_push(about_screen);
}

static void play_through_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!play_through_screen) {
        lv_obj_t * list;
        play_through_screen = build_subsonic_list_screen(TR("Play through"), NULL, &list);
        pill_list_item_t items[] = {
            { TR("Play through folders"), PILL_ACCESSORY_TOGGLE, current_settings.native_play_through_folders,
              NULL, play_through_toggle_cb, &current_settings.native_play_through_folders },
            { TR("Play through albums"), PILL_ACCESSORY_TOGGLE, current_settings.native_play_through_albums,
              NULL, play_through_toggle_cb, &current_settings.native_play_through_albums },
            { TR("How it works"), PILL_ACCESSORY_CHEVRON, false, about_cb },
        };
        add_pill_list_rows(list, items, 3);
    }
    nav_push(play_through_screen);
}

void gui_native_audio_add_play_through_rows(lv_obj_t * list) {
    add_pill_chevron_row(list, TR("Play through"), play_through_row_cb);
    if (!continuation_timer) continuation_timer = lv_timer_create(continuation_tick, 1000, NULL);
}

void gui_play_through_deinit(void) {
    /* Playback continuation survives UI reloads; only widgets belong to this teardown. */
    if (play_through_screen) lv_obj_delete(play_through_screen);
    if (about_screen) lv_obj_delete(about_screen);
    play_through_screen = about_screen = NULL;
}
