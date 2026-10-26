#pragma once

#include <stdint.h>
#include <limits.h>

#define GUI_PLAYLIST_TARGET_SONG_PATH_CAPACITY 600

typedef enum {
    GUI_PLAYLIST_TARGET_NONE,
    GUI_PLAYLIST_TARGET_SONG,
    GUI_PLAYLIST_TARGET_FOLDER
} gui_playlist_target_kind_t;

typedef struct {
    gui_playlist_target_kind_t kind;
    uint64_t generation;
    char song_path[GUI_PLAYLIST_TARGET_SONG_PATH_CAPACITY];
    char folder_path[PATH_MAX];
} gui_playlist_target_snapshot_t;

/* UI-thread-only model for the current Add to Playlist target. Snapshots are
 * copied values; background folder jobs must use their captured request. */
void gui_playlist_target_copy(gui_playlist_target_snapshot_t *out);
uint64_t gui_playlist_target_begin_song(const char *path);
uint64_t gui_playlist_target_begin_folder(const char *path);
uint64_t gui_playlist_target_begin_standalone(void);
/* Clear the folder and advance once, preserving the song path. Callers keep
 * request-validity decisions at their original point before UI side effects. */
void gui_playlist_target_clear_folder(void);
