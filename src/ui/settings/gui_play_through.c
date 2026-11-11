#include "gui_play_through.h"
#include "gui_native_audio.h"
#include "../application/gui.h"
#include "../plugins/gui_plugin_library.h"
#include "../widgets/builders/screen_builders.h"
#include "../text/i18n/i18n.h"
#include "../../audio/engine/audio.h"
#include "../../core/persistence/settings.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char ** candidates;
static int candidate_count, candidate_index;
static char ** batch;
static int batch_count;
static int stopped_polls;
static bool continuing;
static lv_timer_t * continuation_timer;
static lv_obj_t * play_through_screen;
static lv_obj_t * about_screen;

static void clear_candidates(void) {
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

static char ** next_folder_tracks(const char * path, int * count) {
    *count = 0;
    char * directory = strdup(path);
    if (!directory) return NULL;
    char * slash = strrchr(directory, '/');
    if (!slash || slash == directory) { free(directory); return NULL; }
    *slash = '\0';
    int tracks_count;
    char ** tracks = directory_paths(directory, false, &tracks_count);
    bool last = tracks_count > 0 && !strcmp(tracks[tracks_count - 1], path);
    gui_plugin_free_string_array(tracks, tracks_count);
    if (!last) { free(directory); return NULL; }
    char * parent = strdup(directory);
    if (!parent) { free(directory); return NULL; }
    slash = strrchr(parent, '/');
    if (!slash || slash == parent) { free(parent); free(directory); return NULL; }
    *slash = '\0';
    int folders_count;
    char ** folders = directory_paths(parent, true, &folders_count);
    bool found = false;
    tracks = NULL;
    for (int i = 0; i < folders_count; ++i) {
        if (found) {
            tracks = directory_paths(folders[i], false, count);
            if (*count > 0) break;
            free(tracks);
        } else if (!strcmp(folders[i], directory)) found = true;
    }
    gui_plugin_free_string_array(folders, folders_count);
    free(parent);
    free(directory);
    return tracks;
}

static bool sequential(void) {
    return !strcmp(gui_plugin_get_play_mode(), "sequential");
}

void gui_play_through_track_started(const char * artist, const char * album) {
    const char * path = gui_plugin_get_current_track_path();
    if (candidates && path) {
        for (int i = 0; i < batch_count; ++i)
            if (!strcmp(batch[i], path)) return;
    }
    clear_candidates();
    if (!sequential() || !path || strstr(path, "://")) return;
    if (current_settings.native_play_through_albums && artist && *artist && album && *album) {
        int count;
        char ** tracks = gui_plugin_get_album_tracks(artist, album, &count);
        bool last = count > 0 && !strcmp(tracks[count - 1], path);
        gui_plugin_free_string_array(tracks, count);
        if (last) candidates = gui_plugin_get_next_album_tracks(artist, album, &candidate_count);
    }
    if (!candidates && current_settings.native_play_through_folders)
        candidates = next_folder_tracks(path, &candidate_count);
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
    if (direction == 1 && candidates && sequential()) continue_now();
}

static void continuation_tick(lv_timer_t * timer) {
    (void)timer;
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
        add_wrapped_section_header(list, TR("Albums continue in the artist's alphabetical album order."));
        add_wrapped_section_header(list, TR("Folders continue in lexical (alphabetical, case-insensitive) sibling-folder order."));
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
