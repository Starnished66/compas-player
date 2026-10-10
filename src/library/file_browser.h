#include <stdatomic.h>
#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stddef.h>

/* Called when the user taps a playable file. `playlist` holds `count`
 * heap-allocated absolute paths -- every playable file in the same
 * directory as the tapped one, sorted the same way they're listed on
 * screen -- and `selected_index` says which one was tapped. The callee
 * takes ownership: free each entry and then the array itself. */
typedef void (*file_browser_select_cb_t)(char ** playlist, int count, int selected_index);

/* Called when the user taps a .cue sheet -- unlike a playable file or .m3u
 * playlist above, a cue sheet doesn't get played directly (it describes
 * track boundaries within one ALREADY-listed audio file, not a file of its
 * own to hand to a decoder); the callee is expected to parse it (cue_parser.h)
 * and open its own track-list screen. cue_path is the tapped file's full
 * path, valid only for the duration of the callback (same convention as
 * file_browser_select_cb_t's own playlist array -- copy it if needed past
 * this call). */
typedef void (*file_browser_cue_select_cb_t)(const char * cue_path);

typedef struct file_browser file_browser_t;
typedef struct file_browser_index file_browser_index_t;

/* Called when the user taps a playable file, with a retained index of the
 * folder: the callee owns index and closes it. selected_playable is the
 * tapped file's position among the folder's playable files. */
typedef void (*file_browser_index_select_cb_t)(file_browser_index_t *index,
                                               unsigned playable_count,
                                               unsigned selected_playable);
/* Called when the user taps a file that is not media (only listed with
 * on_open_other). path is valid only during the call. */
typedef void (*file_browser_open_cb_t)(const char * path);
/* Called when selection mode starts or ends or the selection changes. */
typedef void (*file_browser_selection_cb_t)(file_browser_t * browser);

typedef struct {
    file_browser_select_cb_t on_select;             /* playable files and .m3u playlists */
    file_browser_cue_select_cb_t on_cue_select;     /* NULL hides .cue sheets */
    file_browser_index_select_cb_t on_index_select; /* NULL: playable taps go to on_select */
    file_browser_open_cb_t on_open_other;           /* non-NULL lists every file */
    file_browser_selection_cb_t on_selection;       /* non-NULL: long press selects */
} file_browser_options_t;

/* Builds a file browser (current-path label + scrollable list) as a child of
 * `parent`, starting at `root_dir`. The user can descend into
 * subdirectories and back up, but never above `root_dir`. The browser is
 * freed with parent. Several browsers can be alive at once. */
file_browser_t * file_browser_create(lv_obj_t * parent, const char * root_dir,
                                     const file_browser_options_t * options);

/* Resets every browser back to its root directory and re-scans it from
 * disk, discarding whatever subdirectory the user was browsing. For
 * refreshing after the underlying storage changes out from under the UI
 * (SD card removed/reinserted) rather than in response to user navigation. */
void file_browser_reset_to_root(void);

/* Rescans the current folder (after files changed), keeping the position.
 * Ends selection mode. */
void file_browser_refresh(file_browser_t * browser);
/* Refreshes every browser, after files changed on the card. */
void file_browser_refresh_all(void);
const char * file_browser_current_dir(const file_browser_t * browser);
/* Shows dir, which must be an existing folder at or under the browser's
 * root. False leaves the browser where it was. */
bool file_browser_open_dir(file_browser_t * browser, const char * dir);
void file_browser_set_show_hidden(file_browser_t * browser, bool show);
bool file_browser_shows_hidden(const file_browser_t * browser);

/* Selection mode (browsers created with on_selection): taps toggle rows
 * instead of opening them. Leaving the folder or refreshing ends it. */
bool file_browser_is_selecting(const file_browser_t * browser);
void file_browser_set_selecting(file_browser_t * browser, bool selecting);
void file_browser_select_all(file_browser_t * browser);
unsigned file_browser_selected_count(const file_browser_t * browser);
/* Absolute paths of the selected entries in list order. The caller frees
 * each path and the array. False when nothing is selected or on error. */
bool file_browser_selected_paths(const file_browser_t * browser, char *** out_paths, int * out_count);

/* In-screen back for the browser on `screen`: ends selection mode, else
 * steps up one folder. False when there is no browser on screen or it is
 * already at its root (the caller pops the screen). */
bool file_browser_go_up_for_screen(lv_obj_t * screen);

/* One-shot lookup that doesn't touch (or require) any browser UI state:
 * scans path's containing directory and builds the same kind of playlist
 * tapping it in the browser would have, for resuming a track on launch
 * before the user has ever opened the browser screen. On success, the
 * caller owns *out_playlist the same way as the select callback above.
 * Returns false if the directory can't be read or path isn't among its
 * playable files (e.g. it was deleted/moved since). */
bool file_browser_build_playlist_for_path(const char * path, char *** out_playlist, int * out_count, int * out_selected_index);

/* Bounded-memory library walk. Each playable file is delivered immediately
 * to cb; the path is valid only for the duration of the callback. Returning
 * false from cb stops the walk. out_count receives the number of songs
 * delivered. Memory use is bounded by the recursion stack + one PATH_MAX
 * buffer per active directory, independent of library size. */
typedef bool (*file_browser_song_visit_cb_t)(const char * path, void * user);

/* Database-oriented variant which excludes one immediate child directory of
 * root, case-insensitively. Descendants with the same name elsewhere are
 * not skipped. A directory containing database.ignore excludes itself and its
 * subtree from the results, database.unignore re-includes one, and a directory
 * containing both or neither inherits from its parent.
 * False means the scan is void; a non-zero *out_skipped means it was
 * incomplete. */
bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress,
                                                     int * out_skipped);

/* Snapshot of the directory + on-screen row a file/playlist was last
 * tapped from -- for the player's "List" option to reopen the folder a
 * track was played from. The getters are only meaningful right after a tap
 * (same convention as e.g. lv_event_get_user_data() being valid only
 * within its own callback), so gui.c must read them synchronously from its
 * own select_cb before returning. Both return defaults before any tap. */
const char * file_browser_get_last_selected_dir(void);
int file_browser_get_last_selected_row(void);

/* True when name has an extension the decoders accept (the Files filter). */
bool file_browser_is_playable_name(const char * name);

/* Parses a M3U/M3U8 playlist file: one entry path per non-blank,
 * non-comment line, resolved relative to the playlist's own directory
 * (standard M3U convention) unless already absolute. Entries that aren't
 * recognized playable audio files are skipped rather than passed through
 * to the decoder to fail on. Same caller-owned-array convention as the
 * select callback above. Returns false if the file can't be opened or has
 * no playable entries. Exposed (not file_browser.c-local) for gui.c's
 * Playlists screen (Music submenu) to open a user-created .m3u without
 * going through the interactive browser UI. */
bool file_browser_build_playlist_from_m3u(const char * m3u_path, char *** out_playlist, int * out_count);

/* Directory index. Writable folders use unlinked disk-backed runs; read-only
 * folders use a bounded 4096-entry in-memory index and fail explicitly when
 * that bound is exceeded. The index pins its source directory FD. Handles
 * may be duplicated for the queue worker; each owner closes its own handle. */
typedef enum {
    FILE_BROWSER_SORT_NAME = 0,
    FILE_BROWSER_SORT_NEWEST = 1,
} file_browser_sort_mode_t;
/* Sort setting is snapshotted by each scan/index operation. Changing it
 * rebuilds the visible directory while preserving its path. */
void file_browser_set_sort_mode(file_browser_sort_mode_t mode);
file_browser_sort_mode_t file_browser_get_sort_mode(void);
bool file_browser_index_open(const char * directory, file_browser_index_t ** out, unsigned * out_count);
bool file_browser_index_retain(const file_browser_index_t * source, file_browser_index_t ** out);
bool file_browser_index_path_at(const file_browser_index_t * index, unsigned ordinal,
                                char * out_path, size_t out_size);
unsigned file_browser_index_playable_count(const file_browser_index_t * index);
bool file_browser_index_playable_path_at(const file_browser_index_t * index, unsigned ordinal,
                                         char * out_path, size_t out_size);
int file_browser_index_dup_directory_fd(const file_browser_index_t * index);
const char * file_browser_index_directory(const file_browser_index_t * index);
void file_browser_index_close(file_browser_index_t * index);
bool file_browser_index_entry_name(const file_browser_index_t * index, unsigned ordinal,
                                   char * name, size_t name_size, bool * is_dir);
/* Opens the track's parent directory index and reports its playable ordinal.
 * The caller owns *out_index. No per-file path array is allocated. */
bool file_browser_open_lazy_directory(const char * track_path, file_browser_index_t ** out_index,
                                      unsigned * out_playable, unsigned * out_selected);

#endif /* FILE_BROWSER_H */
