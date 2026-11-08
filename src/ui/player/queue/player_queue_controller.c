#include "player_queue_resume.h"
#include "player_queue_model.h"
#include "../screen/gui_player.h"
#include "../screen/gui_player_internal.h"
#include "../../application/gui.h"
#include "../../library/browse/gui_library.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/metadata/db/metadata_db.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the current playlist came from. Deliberately NOT derived
 * from `playlist` itself: that's just a flat array of paths with no
 * memory of which screen/group built it. Each interactive play-launch
 * site (all_songs_row_click_cb, group_song_row_click_cb,
 * files_search_row_click_cb, on_file_browser_selected) calls the
 * matching set_player_source_*() helper right before on_file_selected();
 * the Subsonic-download and DLNA-cast play sites call
 * clear_player_source() instead, since a streamed/cast single track has
 * no on-device list to go back to. */
/* player_source_kind_t defined in gui.h */

player_source_kind_t player_source_kind = PLAYER_SOURCE_NONE;

int player_source_all_songs_index = -1; /* row index into all_songs_list -- the DB's own title-sorted order */
int player_source_recently_added_index = -1; /* row index into recently_added_list -- the DB's own first_seen-DESC order */

/* Own copy of the group's song entries at the moment playback started --
 * group_songs_entries/count/title_label themselves just describe
 * whichever group gui_library_get_group_songs_screen() CURRENTLY shows, which can change
 * (browsing to a different artist/album, or a library rescan) before the
 * playback continues. group_song_entry_t (gui.c further down) is
 * declared after this point in the file -- forward-declared here since
 * this struct only needs a pointer to it, not its layout. */
/* group_song_entry_t defined in gui_library.h */
char * player_source_group_title = NULL;
group_song_entry_t * player_source_group_entries = NULL;
int player_source_group_count = 0;
int player_source_group_pos = -1; /* row index within the group */

char player_source_file_browser_dir[PATH_MAX];
int player_source_file_browser_row = -1;

/* Queue ordering lives in player_queue_model; these semantic wrappers retain
 * the GUI-facing transition entry points and supply the current explicit mode. */
static bool play_mode_uses_shuffle_bag(play_mode_t mode) {
    return mode == PLAY_MODE_SHUFFLE || mode == PLAY_MODE_SHUFFLE_ONCE;
}

int compute_auto_advance_index(int index) {
    return player_queue_model_predict_auto_advance(current_settings.play_mode, index);
}

void commit_auto_advance(void) {
    player_queue_model_commit_auto_advance(current_settings.play_mode);
}

int compute_manual_step_index(int index, int direction) {
    return player_queue_model_manual_step(current_settings.play_mode, index, direction);
}

const char *play_mode_icon_asset(play_mode_t mode) {
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: return "playing_plane/loop.png";
        case PLAY_MODE_REPEAT_ONE: return "playing_plane/single.png";
        case PLAY_MODE_SHUFFLE: return "playing_plane/random.png";
        case PLAY_MODE_SHUFFLE_ONCE: return "playing_plane/random_s.png";
        default: return "playing_plane/order.png";
    }
}

/* Song long-press context menu's "Add to Queue" -- splices `path` into the
 * live playlist right after whatever was queued last (or right after the
 * currently-playing track, if the queue's currently empty), so repeated
 * Add to Queue taps play back in the order they were added. See
 * queued_pending_count's own comment for why this reuses playlist[]
 * directly instead of a separate list. No-op with a toast if nothing's
 * playing -- there's no "currently playing track" position to queue
 * after. */
void queue_add_song(const char * path) {
    if (path && path[0]) gui_player_queue_add_many(&path, 1);
}

static bool queue_budget_add(size_t * total, size_t bytes) {
    if (*total > SIZE_MAX - bytes) return false;
    *total += bytes;
    return true;
}

static bool queue_batch_fits_budget(const char * const *paths, int count,
                                    size_t staging_bytes, size_t memory_limit) {
    int queue_count = player_queue_model_count();
    int pending = player_queue_model_pending_count();
    if (!paths || count <= 0 || count > INT_MAX - queue_count ||
        count > INT_MAX - pending) return false;
    size_t total = staging_bytes;
    if ((size_t)count > SIZE_MAX / (3 * sizeof(char *) + sizeof(int)) ||
        !queue_budget_add(&total, (size_t)count * (3 * sizeof(char *) + sizeof(int)))) return false;
    for (int i = 0; i < count; i++) {
        if (!paths[i] || !queue_budget_add(&total, strlen(paths[i]) + 1)) return false;
    }
    return total <= memory_limit;
}

bool gui_player_queue_try_add_many_bounded(const char * const *paths, int count,
                                           size_t staging_bytes, size_t memory_limit) {
    if (!queue_batch_fits_budget(paths, count, staging_bytes, memory_limit)) return false;
    return gui_player_queue_try_add_many(paths, count);
}

static void player_source_set_resume(int kind, const char * name) {
    current_settings.last_source_kind = kind;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s", name);
}

bool gui_player_queue_try_add_many(const char * const *paths, int count) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int old_count = player_queue_model_count();
    int old_pending = player_queue_model_pending_count();
    if (count <= 0 || count > INT_MAX - old_count || count > INT_MAX - old_pending) return false;

    if (!gui_player_has_active_track()) {
        char **copies = calloc((size_t)count, sizeof(*copies));
        if (!copies) return false;
        for (int i = 0; i < count; i++) {
            copies[i] = paths[i] ? strdup(paths[i]) : NULL;
            if (!copies[i]) {
                for (int j = 0; j < count; j++) free(copies[j]);
                free(copies);
                return false;
            }
        }
        free_playlist();
        clear_player_source();
        player_queue_install_t install = player_queue_install_new(copies, count);
        install.current = 0;
        install.pending_count = count - 1;
        install.next_insert_index = count > 1 ? count : -1;
        player_queue_model_install_take(install);
        request_track_metadata(0, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s",
                 player_queue_model_path_at(0));
        current_settings.last_position = 0;
        player_source_set_resume(0, "");
        settings_save_async(&current_settings);
        show_info_toast(TR("Queue ready. Press Play to start."));
        playback_intent_bump();
        return true;
    }
    int added = 0;
    if (!player_queue_model_try_add_many(paths, count, current_settings.play_mode, &added)) return false;
    int current = player_queue_model_current();
    player_set_song_count(current, player_queue_model_count());
    arm_next_track_for_audio(current);
    char msg[128];
    snprintf(msg, sizeof(msg), TR_N("Added %d song to queue", "Added %d songs to queue", added), added);
    show_info_toast(msg);
    playback_intent_bump();
    return true;
}

void gui_player_queue_add_many(const char * const * paths, int count) {
    (void) gui_player_queue_try_add_many(paths, count);
}

void on_file_selected(char ** new_playlist, int count, int selected_index) {
    on_file_selected_at(new_playlist, count, selected_index, 0.0);
}

/* on_file_selected() starting at a track-relative resume offset. A brand new
 * playback context (a fresh song tapped from any list) invalidates whatever
 * was queued against the OLD playlist[] -- those array positions no longer
 * mean anything once the array itself has been replaced. Matches every other
 * music app: starting something new clears "Up Next". */
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds) {
    if (!gui_player_local_playback_allowed()) {
        for (int i = 0; i < count; i++) free(new_playlist[i]);
        free(new_playlist);
        return;
    }
    free_playlist();
    player_queue_model_install_take(player_queue_install_new(new_playlist, count));
    queue_pin_sd_dir_if_needed();
    play_track_at_from(selected_index, start_seconds);
}

/* set_player_source_group_songs() is defined further down, right after
 * group_songs_entries/count/title_label -- it needs those already in
 * scope. These three don't. */
void clear_player_source(void) {
    player_source_kind = PLAYER_SOURCE_NONE;
    player_source_all_songs_index = -1;
    player_source_recently_added_index = -1;
    free(player_source_group_title);
    player_source_group_title = NULL;
    free_group_song_entries(player_source_group_entries, player_source_group_count);
    player_source_group_entries = NULL;
    player_source_group_count = 0;
    player_source_group_pos = -1;
    player_source_file_browser_row = -1;
}

void set_player_source_all_songs(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_ALL_SONGS;
    player_source_all_songs_index = display_index;
    player_source_set_resume(1, "");
}

void set_player_source_recently_added(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_RECENTLY_ADDED;
    player_source_recently_added_index = display_index;
    /* No dedicated resume kind for this source -- falls back to "unknown",
     * same as Favorites/Most Played/user playlists (see group_song_row_
     * click_cb's own last_source_kind assignment); only All Songs and Album
     * get a real boot-resume slot (settings.h's own last_source_kind
     * comment). */
    player_source_set_resume(0, "");
}

void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_GROUP_SONGS;
    copy_group_song_entries(&player_source_group_entries, entries, count);
    player_source_group_count = count;
    player_source_group_pos = selected_index;
    if (title) player_source_group_title = strdup(title);
    player_source_set_resume(2, title ? title : "");
}

void set_player_source_file_browser(const char * dir, int row) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_FILE_BROWSER;
    snprintf(player_source_file_browser_dir, sizeof(player_source_file_browser_dir), "%s", dir);
    player_source_file_browser_row = row;
    player_source_set_resume(0, "");
}

/* Wraps on_file_selected() as file_browser_create()'s on_select, rather than
 * passing on_file_selected directly, so the source snapshot above only
 * ever gets set for an actual folder-browse tap -- on_file_selected()
 * itself is shared by every play-launch path (All Songs, group songs,
 * Subsonic downloads, DLNA casts, ...) and has no way to tell which of
 * them is calling it. file_browser_get_last_selected_dir()/_row() are
 * only valid synchronously within file_browser.c's own select_cb() call,
 * which is exactly where this reads them. */
void on_file_browser_selected(char ** new_playlist, int count, int selected_index) {
    if (!gui_player_local_playback_allowed()) {
        for (int i = 0; i < count; i++) free(new_playlist[i]);
        free(new_playlist);
        return;
    }
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    on_file_selected(new_playlist, count, selected_index);
}

/* Installs a directory index as the queue and plays one playable ordinal.
 * Takes ownership of index. source_dir must stay valid through the call.
 * False when nothing changed (bad arguments or no memory). */
static bool play_file_browser_index(file_browser_index_t *index, unsigned playable_count,
                                    unsigned selected_playable, const char *source_dir, int source_row) {
    if (!gui_player_local_playback_allowed()) {
        if (index) file_browser_index_close(index);
        return false;
    }
    if (!index || !playable_count || selected_playable >= playable_count || playable_count > (unsigned) INT_MAX) {
        if (index) file_browser_index_close(index);
        return false;
    }
    char ** slots = calloc(playable_count, sizeof(*slots));
    int * order = malloc((size_t) playable_count * sizeof(*order));
    if (!slots || !order) {
        free(slots); free(order); file_browser_index_close(index); return false;
    }
    int pinned_queue_dirfd = open_queue_dir_from_file_index(index);
    for (unsigned i = 0; i < playable_count; i++) order[i] = (int) i;
    free_playlist();
    player_queue_install_t install = player_queue_install_new(slots, (int)playable_count);
    install.lazy_ranks = order;
    install.file_index = index;
    install.queue_sd_dirfd = pinned_queue_dirfd;
    player_queue_model_install_take(install);
    set_player_source_file_browser(source_dir, source_row);
    play_track_at((int) selected_playable);
    return true;
}

void on_file_browser_index_selected(file_browser_index_t *index, unsigned playable_count,
                                    unsigned selected_playable) {
    (void) play_file_browser_index(index, playable_count, selected_playable,
                                   file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
}

/* Remote Control folder play: same queue a Files tap builds, the track's
 * directory index in browser order. */
bool gui_player_play_folder_track(const char * track_path) {
    file_browser_index_t * index = NULL;
    unsigned playable = 0, selected = 0;
    if (!file_browser_open_lazy_directory(track_path, &index, &playable, &selected)) return false;
    /* The index owns its directory string for as long as the queue holds it. */
    return play_file_browser_index(index, playable, selected, file_browser_index_directory(index), -1);
}

bool gui_player_play_folder_index(file_browser_index_t *index, unsigned playable_count,
                                  int play_mode) {
    if (!gui_player_local_playback_allowed()) {
        if (index) file_browser_index_close(index);
        return false;
    }
    if (play_mode != PLAY_MODE_SEQUENTIAL && play_mode != PLAY_MODE_SHUFFLE &&
        play_mode != PLAY_MODE_SHUFFLE_ONCE) {
        if (index) file_browser_index_close(index);
        return false;
    }
    unsigned initial = 0;
    if (play_mode_uses_shuffle_bag((play_mode_t) play_mode) && playable_count > 1)
        initial = (unsigned)player_queue_model_random_index((int)playable_count);
    gui_player_set_play_mode(play_mode);
    return play_file_browser_index(index, playable_count, initial,
                                   index ? file_browser_index_directory(index) : "", -1);
}

/* Queue every song of the library in database order (All Songs) or by
 * recency (Recently Added). Recently Added is pinned to the SD queue
 * directory, All Songs follows its snapshot. */
static void play_lazy_songs(int selected_index, bool recency) {
    if (!gui_player_local_playback_allowed()) return;
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;
    if (count <= 0 || selected_index < 0 || selected_index >= count) return;

    char ** new_pl = calloc((size_t) count, sizeof(char *));
    int * new_order = malloc(sizeof(int) * (size_t) count);
    if (!new_pl || !new_order) {
        free(new_pl);
        free(new_order);
        return;
    }

    metadata_db_snapshot_t * snapshot = metadata_db_snapshot_open(recency);
    if (!snapshot || metadata_db_snapshot_count(snapshot) != count) {
        if (snapshot) metadata_db_snapshot_close(snapshot);
        free(new_pl); free(new_order); return;
    }
    for (int i = 0; i < count; i++) new_order[i] = i;
    free_playlist();
    int queue_dirfd = recency ? open_sd_queue_dir() : open_queue_dir_from_snapshot(snapshot);
    player_queue_install_t install = player_queue_install_new(new_pl, count);
    install.lazy_ranks = new_order;
    install.lazy_order_is_recency = recency;
    install.snapshot = snapshot;
    install.queue_sd_dirfd = queue_dirfd;
    player_queue_model_install_take(install);
    play_track_at(selected_index);
}

void on_file_selected_lazy_all_songs(int selected_index) {
    play_lazy_songs(selected_index, false);
}

void on_file_selected_lazy_recently_added(int selected_index) {
    play_lazy_songs(selected_index, true);
}

/* Replace the queue with a collection: forget the old source and queue, and
 * start from the requested play mode. */
static void begin_collection_playback(int play_mode) {
    clear_player_source();
    player_source_set_resume(0, "");
    free_playlist();
    gui_player_set_play_mode(play_mode);
}

bool gui_player_play_lazy_collection(metadata_db_snapshot_t *snapshot,
                                     int *title_ranks, int count, int play_mode) {
    if (!gui_player_local_playback_allowed()) return false;
    if (!snapshot || !title_ranks || count <= 0 || count > INT_MAX ||
        play_mode < PLAY_MODE_SEQUENTIAL || play_mode > PLAY_MODE_SHUFFLE_ONCE)
        return false;
    int snapshot_count = metadata_db_snapshot_count(snapshot);
    if (snapshot_count <= 0) return false;
    if ((size_t)snapshot_count > SIZE_MAX - 7u) return false;
    size_t seen_size = ((size_t)snapshot_count + 7u) / 8u;
    uint8_t *seen_ranks = calloc(seen_size, 1);
    if (!seen_ranks) return false;
    for (int i = 0; i < count; i++) {
        int rank = title_ranks[i];
        if (rank < 0 || rank >= snapshot_count) { free(seen_ranks); return false; }
        size_t byte = (size_t)rank >> 3;
        uint8_t mask = (uint8_t)(1u << ((unsigned)rank & 7u));
        if (seen_ranks[byte] & mask) { free(seen_ranks); return false; }
        seen_ranks[byte] |= mask;
    }
    free(seen_ranks);

    char **new_playlist = calloc((size_t)count, sizeof(*new_playlist));
    int *new_shuffle_order = NULL;
    if (!new_playlist) return false;
    if (play_mode == PLAY_MODE_SHUFFLE || play_mode == PLAY_MODE_SHUFFLE_ONCE) {
        int first = 0;
        if (!player_queue_model_make_shuffle_order(count, &new_shuffle_order, &first)) {
            free(new_playlist); return false;
        }
    }

    int start = new_shuffle_order ? new_shuffle_order[0] : 0;
    begin_collection_playback(play_mode);
    int queue_dirfd = open_queue_dir_from_snapshot(snapshot);
    player_queue_install_t install = player_queue_install_new(new_playlist, count);
    install.lazy_ranks = title_ranks;
    install.snapshot = snapshot;
    install.shuffle_order = new_shuffle_order;
    install.shuffle_pos = new_shuffle_order ? 0 : -1;
    install.queue_sd_dirfd = queue_dirfd;
    player_queue_model_install_take(install);
    play_track_at(start);
    return true;
}

bool gui_player_play_collection_paths(char **paths, int count, int play_mode) {
    if (!gui_player_local_playback_allowed()) return false;
    if (!paths || count <= 0 || count > INT_MAX ||
        play_mode < PLAY_MODE_SEQUENTIAL || play_mode > PLAY_MODE_SHUFFLE_ONCE)
        return false;
    for (int i = 0; i < count; i++) if (!paths[i] || !paths[i][0]) return false;
    begin_collection_playback(play_mode);
    player_queue_model_install_take(player_queue_install_new(paths, count));
    queue_pin_sd_dir_if_needed();
    int start = 0;
    if (play_mode_uses_shuffle_bag((play_mode_t)play_mode) && count > 1)
        start = player_queue_model_random_index(count);
    play_track_at(start);
    return true;
}
