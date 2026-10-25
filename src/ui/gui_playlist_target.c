#include "gui_playlist_target.h"

#include <stdio.h>
#include <string.h>

static char add_to_playlist_target_path[GUI_PLAYLIST_TARGET_SONG_PATH_CAPACITY] = "";
static char add_to_playlist_target_dir[PATH_MAX];
static uint64_t add_to_playlist_target_generation;

void gui_playlist_target_copy(gui_playlist_target_snapshot_t *out) {
    if (!out) return;
    out->generation = add_to_playlist_target_generation;
    memcpy(out->song_path, add_to_playlist_target_path, sizeof(out->song_path));
    memcpy(out->folder_path, add_to_playlist_target_dir, sizeof(out->folder_path));
    out->kind = out->folder_path[0] ? GUI_PLAYLIST_TARGET_FOLDER
        : (out->song_path[0] ? GUI_PLAYLIST_TARGET_SONG : GUI_PLAYLIST_TARGET_NONE);
}

uint64_t gui_playlist_target_begin_song(const char *path) {
    add_to_playlist_target_dir[0] = '\0';
    snprintf(add_to_playlist_target_path, sizeof(add_to_playlist_target_path), "%s", path);
    add_to_playlist_target_generation++;
    return add_to_playlist_target_generation;
}

uint64_t gui_playlist_target_begin_folder(const char *path) {
    snprintf(add_to_playlist_target_dir, sizeof(add_to_playlist_target_dir), "%s", path);
    add_to_playlist_target_path[0] = '\0';
    add_to_playlist_target_generation++;
    return add_to_playlist_target_generation;
}

uint64_t gui_playlist_target_begin_standalone(void) {
    add_to_playlist_target_path[0] = '\0';
    add_to_playlist_target_dir[0] = '\0';
    add_to_playlist_target_generation++;
    return add_to_playlist_target_generation;
}

void gui_playlist_target_clear_folder(void) {
    add_to_playlist_target_dir[0] = '\0';
    add_to_playlist_target_generation++;
}
