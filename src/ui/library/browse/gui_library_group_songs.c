/* The Songs page shared by albums, artists, genres and playlists: state, entries, play actions, probes, search and in-place refresh. */
#include "../../text/i18n/i18n.h"
#include "gui_album_header.h"
#include "gui_artist_data.h"
#include "gui_artist_page.h"
#include "gui_page_durations.h"
#include "../library_selection_snapshot.h"
#include "../../assets/assets.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../core/ui/display/ui_wake.h"
#include "../../../library/playlists/playlist_files.h"
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui_group_songs_internal.h"

static void refresh_group_songs_search_now_playing(void);

bool group_songs_source_is_album = false;
bool group_songs_loading;
group_songs_refresh_source_t group_songs_refresh_source;
static char group_songs_refresh_album[128];
char group_songs_refresh_album_artist[128];
static char group_songs_refresh_artist_filter[128];
char group_songs_refresh_artist_name[128];
metadata_db_group_kind_t group_songs_refresh_artist_kind;
char group_songs_refresh_m3u_path[PATH_MAX];
char group_songs_refresh_genre[METADATA_DB_TEXT_MAX] = {0};
/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
lv_obj_t * group_songs_edit_btn;
group_song_entry_t * group_songs_entries; /* owned -- see set_group_songs_entries() */
int group_songs_count;
int group_songs_page_start;
lv_obj_t * group_songs_selection_checks[GROUP_SONGS_PAGE_SIZE];
lv_obj_t * group_songs_delete_btn;
lv_obj_t * group_songs_select_all_row;
lv_obj_t * group_songs_select_all_checkbox;
lv_obj_t * group_songs_select_all_label;
int group_songs_populated_page_start = -1;
bool group_songs_anchor_pending;
static bool group_songs_rebuild_on_load;
int group_songs_anchor_index;
int32_t group_songs_anchor_offset;
/* One bounded job at a time. The worker owns path copies and never sees
 * LVGL objects or group_songs_entries. Page generations invalidate late
 * results without waiting for the current decoder to finish. */
track_probe_job_t * group_probe_job;
uint64_t group_probe_generation = 1;
uint64_t group_probe_job_generation;
static uint64_t group_probe_requested_generation;
static int group_probe_page_start;
bool group_probe_storage_available = true;
static bool group_probe_retry_pending;
static uint32_t group_probe_retry_tick;
/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
lv_obj_t * group_songs_now_playing_bar;
lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
lv_obj_t * group_songs_thumbnail_images[GROUP_SONGS_PAGE_SIZE];
bool group_songs_music_submenu;
/* Non-NULL only when the currently shown group is a user-created .m3u
 * playlist (set by show_m3u_playlist() via show_group_songs_editable()),
 * enabling the edit/remove-song UI. NULL for Artists/Albums/Favorites/
 * Most Played, which do not back onto a rewritable file. Borrowed pointer
 * from playlists_m3u_paths[]; valid for the lifetime of the screen.
 * group_songs_edit_mode is reset to false on every fresh entry. */
const char * group_songs_edit_m3u_path = NULL;
static char group_songs_owned_m3u_path[PATH_MAX];
struct stat group_songs_file_stat;
bool group_songs_file_stat_valid;
bool group_songs_edit_mode = false;

/* Index of now_playing_path in group_songs_entries, remembered so a now-playing update does not
 * strcmp every entry again. The two setters below are the only writers of the entry array and
 * paths (the probe path only rewrites titles), and each bumps the generation. */
static uint64_t group_songs_entries_generation;
static struct {
    bool valid;
    uint64_t generation;
    int count;
    int index; /* -1: not in the list */
    char path[sizeof(now_playing_path)];
} group_songs_now_playing_cache;

static int group_songs_now_playing_index(void) {
    if (!now_playing_path[0]) return -1;
    if (!group_songs_now_playing_cache.valid ||
        group_songs_now_playing_cache.generation != group_songs_entries_generation ||
        group_songs_now_playing_cache.count != group_songs_count ||
        strcmp(group_songs_now_playing_cache.path, now_playing_path) != 0) {
        int match = -1;
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) { match = i; break; }
        }
        group_songs_now_playing_cache.valid = true;
        group_songs_now_playing_cache.generation = group_songs_entries_generation;
        group_songs_now_playing_cache.count = group_songs_count;
        group_songs_now_playing_cache.index = match;
        snprintf(group_songs_now_playing_cache.path, sizeof(group_songs_now_playing_cache.path), "%s",
                 now_playing_path);
    }
    return group_songs_now_playing_cache.index;
}

/* Frees the previous group_songs_entries (if any) and replaces it with an
 * owned copy of entries[0..count), for callers that keep their own array
 * (the public show_group_songs_editable() path). Callers that hand over a
 * freshly built array use set_group_songs_entries_owned() instead. Either way
 * group_songs_entries is NULL or a malloc'd array this screen owns. */
static void set_group_songs_entries(const group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries_generation++;
    group_songs_entries = NULL;
    group_songs_count = copy_group_song_entries(&group_songs_entries, entries, count) ? count : 0;
}

/* Sibling to set_group_songs_entries() that takes ownership of an already
 * allocated entries[] array rather than copying it, avoiding duplicate
 * allocations for large song lists. The caller transfers ownership and
 * must not free `entries` or its elements afterward. */
void set_group_songs_entries_owned(group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries_generation++;
    group_songs_entries = entries;
    group_songs_count = count;
}

void cancel_group_song_probes(void) {
    ++group_probe_generation;
    group_probe_retry_pending = false;
    gui_page_durations_reset();
    if (group_probe_job) track_probe_job_cancel(group_probe_job);
}

static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          group_songs_source_title ? group_songs_source_title : "", pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album && group_songs_source_title ? group_songs_source_title : "");
}

/* One past the last entry of the page shown, and how many entries it holds. */
int group_songs_page_end(void) {
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    return page_end > group_songs_count ? group_songs_count : page_end;
}

int group_songs_visible_count(void) {
    int visible_count = group_songs_count - group_songs_page_start;
    return visible_count > GROUP_SONGS_PAGE_SIZE ? GROUP_SONGS_PAGE_SIZE : visible_count;
}

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already triggered the context menu on a group song row. */
bool group_song_row_long_press_fired = false;

static void group_play_at(int pos) {
    if (library_rescan_active) return;
    if (pos < 0 || pos >= group_songs_count) return;
    gui_player_seed_selection_song(NULL, NULL);
    if (group_songs_edit_m3u_path && group_songs_playlist_stale()) return;
    /* Allocate and copy playlist paths using calloc to handle potential
     * allocation failures cleanly. */
    char ** playlist_copy = calloc((size_t) group_songs_count, sizeof(char *));
    if (!playlist_copy) return;
    bool ok = true;
    for (int i = 0; i < group_songs_count; i++) {
        playlist_copy[i] = strdup(group_songs_entries[i].path);
        if (!playlist_copy[i]) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (int i = 0; i < group_songs_count; i++) free(playlist_copy[i]);
        free(playlist_copy);
        return;
    }
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1,
                           group_songs_entries[pos].song_id,
                           group_songs_entries[pos].path);
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) { group_song_row_long_press_fired = false; return; }
    group_play_at((int) (intptr_t) lv_event_get_user_data(e));
}

int playlist_start_source;

/* Starting a collection sequentially should keep an active repeat mode. */
play_mode_t collection_play_mode(play_mode_t requested_mode) {
    if (requested_mode == PLAY_MODE_SHUFFLE || requested_mode == PLAY_MODE_SHUFFLE_ONCE)
        return requested_mode;
    if (requested_mode == PLAY_MODE_SEQUENTIAL &&
        (current_settings.play_mode == PLAY_MODE_REPEAT_ALL ||
         current_settings.play_mode == PLAY_MODE_REPEAT_ONE)) {
        return (play_mode_t) current_settings.play_mode;
    }
    return requested_mode;
}

void playlist_start_selected(bool shuffle) {
    if (library_rescan_active) return;
    int count = playlist_start_source == PLAY_ALL_SOURCE_GROUP ? group_songs_count
                                                               : (int) metadata_db_get_song_count();
    if (count <= 0) { show_info_toast(TR("Playlist is empty")); return; }
    int index = 0;
    if (shuffle) {
        unsigned int value = (unsigned int) time(NULL) ^ lv_tick_get();
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, &value, sizeof(value)); (void) n; close(fd); }
        index = (int) (value % (unsigned int) count);
    }
    gui_player_set_play_mode(collection_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL));
    if (playlist_start_source == PLAY_ALL_SOURCE_RECENTLY_ADDED) {
        set_player_source_recently_added(index);
        on_file_selected_lazy_recently_added(index);
    } else if (playlist_start_source == PLAY_ALL_SOURCE_ALL_SONGS) {
        set_player_source_all_songs(index);
        on_file_selected_lazy_all_songs(index);
    } else group_play_at(index);
}
/* The Play and Shuffle buttons of the album, artist-songs and playlist pages and of the song lists: start
 * the whole collection in order, or from a random song in shuffle mode. */
void group_play_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlist_start_source = PLAY_ALL_SOURCE_GROUP;
    playlist_start_selected(false);
}
void group_shuffle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlist_start_source = PLAY_ALL_SOURCE_GROUP;
    playlist_start_selected(true);
}

/* True while the group-songs screen shows an album (not a playlist, genre,
 * artist's songs or favourites): the only screen that gets the album header
 * and numbered rows. */
bool group_songs_album_layout(void) {
    return group_songs_source_is_album && group_songs_music_submenu && !group_songs_edit_m3u_path;
}

void group_song_row_long_press_cb(lv_event_t * e) {
    if (library_rescan_active) return;
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
}

static int group_probe_batch_index[GROUP_SONGS_PAGE_SIZE]; /* probe job index -> page index */
static uint64_t group_probe_lookup_generation;

/* Shows "4:32 · FLAC" on page row `index` (a probe result or a cache hit). */
static void apply_group_probe_row(int index, unsigned int seconds, audio_codec_t codec) {
    int pos = group_probe_page_start + index;
    if (index < 0 || index >= GROUP_SONGS_PAGE_SIZE || pos >= group_songs_count) return;
    char detail[64];
    gui_song_format_detail(detail, sizeof(detail), seconds, library_codec_name(codec));
    const char * old = group_songs_entries[pos].title;
    const char * newline = strchr(old, '\n');
    size_t title_len = newline ? (size_t)(newline - old) : strlen(old);
    char * title = malloc(title_len + strlen(detail) + 2);
    if (!title) return;
    memcpy(title, old, title_len);
    title[title_len] = '\n';
    strcpy(title + title_len + 1, detail);
    free(group_songs_entries[pos].title);
    group_songs_entries[pos].title = title;
    lv_obj_t * row = group_songs_visible_rows[index];
    if (row && group_songs_album_layout()) {
        gui_album_header_row_set_duration(row, seconds);
    } else if (row) {
        group_song_row_show_detail(row, detail);
    }
}

static void apply_cached_group_row(int index, const duration_info_t * info, void * user) {
    (void) user;
    int pos = group_probe_page_start + index;
    if (group_songs_album_layout()) gui_album_header_note_cached(pos, info);
    apply_group_probe_row(index, (unsigned int)(info->duration_ms / 1000.0 + 0.5), (audio_codec_t) info->codec);
}

static void group_probe_schedule_retry(void) {
    group_probe_retry_pending = true;
    group_probe_retry_tick = lv_tick_get();
}

void gui_library_poll_track_probes(void) {
    bool artist_visible = artist_albums_screen && lv_screen_active() == artist_albums_screen &&
                          group_probe_storage_available && backlight_screen_is_on();
    gui_artist_data_poll(artist_visible);
    if (artist_albums_list && gui_artist_page_poll(artist_albums_list, artist_visible) &&
        !compact_list_rederive_cached(artist_albums_list, artist_album_rederive_row, NULL))
        compact_list_refetch(artist_albums_list);
    bool visible = group_songs_screen && lv_screen_active() == group_songs_screen &&
                   group_songs_music_submenu && group_probe_storage_available && backlight_screen_is_on();
    if (!visible && group_probe_job && group_probe_job_generation == group_probe_generation)
        cancel_group_song_probes();
    /* The album header's whole-album probe waits for the page's own probe. */
    if (gui_album_header_poll(visible && group_songs_album_layout(),
                              visible && group_songs_album_layout() && !group_probe_job &&
                              group_probe_requested_generation == group_probe_generation))
        refresh_group_songs_now_playing_indicator(); /* wrapped chips moved the rows */

    if (group_probe_job) {
        bool current = visible && group_probe_job_generation == group_probe_generation;
        /* Take the completion snapshot before draining results, so a final
         * result published just after next() returns false is not lost. */
        bool done = track_probe_job_done(group_probe_job);
        bool drained = false;
        if (current) {
            /* Limit label work per tick; never rebuild the list or reset
             * its scroll position when a probe completes. */
            for (int budget = 0; budget < 4; ++budget) {
                size_t index;
                audio_current_format_info_t info;
                if (!track_probe_job_next(group_probe_job, &index, &info)) {
                    drained = true;
                    break;
                }
                if (budget == 3) ui_wake_notify(); /* budget spent with results left: come back next pass */
                if (index >= GROUP_SONGS_PAGE_SIZE) continue;
                int page_index = group_probe_batch_index[index];
                int pos = group_probe_page_start + page_index;
                if (pos >= group_songs_count) continue;
                if (!info.valid || !(info.duration_seconds > 0.0 &&
                    info.duration_seconds <= UINT_MAX - 1.0)) {
                    /* Remembered only when the format itself was rejected. */
                    if (!info.valid && track_probe_job_confirmed_failure(group_probe_job, index)) {
                        gui_page_durations_note_failed(page_index);
                        if (group_songs_album_layout()) {
                            duration_info_t failed = { .bit_depth = DURATION_PROBE_REVISION,
                                                       .flags = DURATION_CACHE_FLAG_FAILED };
                            gui_album_header_note_cached(pos, &failed);
                        }
                    }
                    continue;
                }
                gui_page_durations_note_probe(page_index, &info);
                if (group_songs_album_layout()) gui_album_header_note_probe(pos, &info);
                apply_group_probe_row(page_index, (unsigned int)(info.duration_seconds + 0.5), info.codec);
            }
        }
        if (done && (!current || drained)) {
            track_probe_job_destroy(group_probe_job);
            group_probe_job = NULL;
        }
    }

    if (!visible || group_probe_job || group_probe_requested_generation == group_probe_generation)
        return;
    if (group_probe_retry_pending && lv_tick_elaps(group_probe_retry_tick) < 1000) return;
    int count = group_songs_visible_count();
    if (count <= 0) return;
    const char * paths[GROUP_SONGS_PAGE_SIZE];
    /* 1. Ask the duration cache about the whole page (on a worker). */
    if (group_probe_lookup_generation != group_probe_generation) {
        for (int i = 0; i < count; ++i) paths[i] = group_songs_entries[group_songs_page_start + i].path;
        if (!gui_page_durations_begin(paths, count)) {
            group_probe_schedule_retry();
            return;
        }
        group_probe_retry_pending = false;
        group_probe_lookup_generation = group_probe_generation;
        group_probe_page_start = group_songs_page_start;
    }
    if (!gui_page_durations_ready()) return;
    /* 2. Cached rows are filled now; only the misses are probed. */
    int miss[GROUP_SONGS_PAGE_SIZE];
    int n = gui_page_durations_plan(miss, GROUP_SONGS_PAGE_SIZE, apply_cached_group_row, NULL);
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            paths[i] = group_songs_entries[group_probe_page_start + miss[i]].path;
            group_probe_batch_index[i] = miss[i];
        }
        group_probe_job = track_probe_job_start(paths, (size_t)n);
        if (!group_probe_job) {
            group_probe_schedule_retry();
            return;
        }
    }
    group_probe_retry_pending = false;
    group_probe_requested_generation = group_probe_generation;
    group_probe_job_generation = group_probe_generation;
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. The visible-row
 * table lets the marker follow LVGL's final flex layout, including runtime
 * font/touch sizing and any action or paging rows before the song. */
void refresh_group_songs_now_playing_indicator(void) {
    group_songs_place_now_playing_bar(false);
}

/* `laid_out`: the list's layout is already current (a build chunk just ended with it). */
void group_songs_place_now_playing_bar(bool laid_out) {
    /* Search results can include songs outside the underlying list's page. */
    refresh_group_songs_search_now_playing();
    if (!group_songs_now_playing_bar) return;

    int match = group_songs_now_playing_index();

    int page_end = group_songs_page_end();
    if (match < group_songs_page_start || match >= page_end) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t * row = group_songs_visible_rows[match - group_songs_page_start];
    if (!row) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Runtime font/touch sizing can make the rendered row taller than its
     * compile-time minimum. Follow the real laid-out object rather than
     * reconstructing its bounds from fixed height and gap constants. */
    if (!laid_out) lv_obj_update_layout(group_songs_list);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, lv_obj_get_y(row));
    lv_obj_set_height(group_songs_now_playing_bar, lv_obj_get_height(row));
}

static void group_songs_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        gui_album_header_forget();
        gui_album_header_release_cover();
        library_thumbnails_forget_list(group_songs_list);
        group_songs_reorder_detach();
        group_songs_clear_selection();
        group_songs_edit_mode = false;
        group_songs_edit_btn = NULL;
        group_songs_delete_btn = NULL;
        group_songs_title_label = NULL;
        free(group_songs_source_title);
        group_songs_source_title = NULL;
        group_songs_list = NULL;
        group_songs_build_reset();
        group_songs_now_playing_bar = NULL;
        group_songs_populated_page_start = -1;
        group_songs_anchor_pending = false;
        group_songs_rebuild_on_load = false;
        group_songs_forget_row_objects();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        group_songs_build_pause(); /* resumed by SCREEN_LOADED */
        group_songs_reorder_detach();
        if (group_songs_edit_mode) {
            group_songs_capture_scroll_anchor(NULL, 0);
            group_songs_clear_selection();
            memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
            group_songs_edit_mode = false;
            group_songs_rebuild_on_load = true;
        }
    } else if (code == LV_EVENT_SCREEN_LOADED) {
        if (group_songs_rebuild_on_load) {
            group_songs_rebuild_on_load = false;
            populate_group_songs_rows();
        }
        group_songs_build_resume(); /* paused while another screen was shown */
        if (!group_songs_album_layout()) ui_wake_notify(); /* the page's duration lookup starts now (see populate_group_songs_rows) */
    }
}

/* entries[0..count) is copied into group_songs_entries (see set_group_songs_
 * entries()) -- the caller's own array/buffer can be freed immediately after
 * this call returns, unlike the old group_t-based API where the group_t's
 * .indices had to stay valid for as long as this screen kept showing it. */
static void show_group_songs_editable_impl(const char * name, const group_song_entry_t * entries, int count,
                                           const char * editable_m3u_path, bool music_submenu, bool take_ownership) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) {
        if (take_ownership) free_group_song_entries((group_song_entry_t *) entries, count);
        show_error_toast(TR("Out of memory"));
        return;
    }
    album_load_bump_generation();
    group_songs_source_is_album = group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM;
    group_songs_music_submenu = music_submenu;
    if (editable_m3u_path) snprintf(group_songs_owned_m3u_path, sizeof(group_songs_owned_m3u_path), "%s", editable_m3u_path);
    group_songs_edit_m3u_path = editable_m3u_path ? group_songs_owned_m3u_path : NULL;
    group_songs_file_stat_valid = editable_m3u_path && stat(editable_m3u_path, &group_songs_file_stat) == 0;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    if (take_ownership) set_group_songs_entries_owned((group_song_entry_t *) entries, count);
    else set_group_songs_entries(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path, bool music_submenu) {
    show_group_songs_editable_impl(name, entries, count, editable_m3u_path, music_submenu, false);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    /* The title arrives already translated (show_favorites()/show_most_played()). */
    group_songs_refresh_source = name && strcmp(name, TR("Favorites")) == 0 ? GROUP_SONGS_REFRESH_FAVORITES
                                : name && strcmp(name, TR("Most Played")) == 0 ? GROUP_SONGS_REFRESH_MOST_PLAYED
                                : GROUP_SONGS_REFRESH_NONE;
    show_group_songs_editable(name, entries, count, NULL, false);
}

void show_music_group_songs_impl(const char * name, const char * album_artist, const char * artist_filter,
                                        const group_song_entry_t * entries, int count, bool take_ownership) {
    group_songs_refresh_source = GROUP_SONGS_REFRESH_ALBUM;
    snprintf(group_songs_refresh_album, sizeof(group_songs_refresh_album), "%s", name ? name : "");
    snprintf(group_songs_refresh_album_artist, sizeof(group_songs_refresh_album_artist), "%s",
             album_artist ? album_artist : "");
    snprintf(group_songs_refresh_artist_filter, sizeof(group_songs_refresh_artist_filter), "%s",
             artist_filter ? artist_filter : "");
    show_group_songs_editable_impl(name, entries, count, NULL, true, take_ownership);
}

/* Ownership-transferring sibling of show_group_songs() -- see set_group_
 * songs_entries_owned()'s own comment for why this exists. Never editable
 * (no m3u path makes sense for a flattened, non-playlist view), same as
 * plain show_group_songs()'s own default. Caller must not free `entries`
 * after this call -- ownership has moved to group_songs_entries. */
void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count,
                                            bool push_screen) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) {
        free_group_song_entries(entries, count);
        show_error_toast(TR("Out of memory"));
        return;
    }
    album_load_bump_generation();
    group_songs_source_is_album = false;
    group_songs_music_submenu = true;
    group_songs_edit_m3u_path = NULL;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    set_group_songs_entries_owned(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    if (push_screen) nav_push(group_songs_screen);
}

lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    group_songs_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    /* Keep dynamic album/group names on one line. The title remains bounded
     * by the existing Edit/search action reservations below. */
    row_label_enable_marquee(group_songs_title_label);
    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, TR("Edit"));
    lv_obj_add_style(group_songs_edit_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(group_songs_edit_btn, BOARD_SCALE_PX(100));
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_DELETE, NULL);

    group_songs_delete_btn = build_top_right_icon_button(scr, asset_path("playlist_actions/trash.png"),
                                                          group_songs_delete_selected_cb);
    align_screen_header_action(group_songs_delete_btn, BOARD_SCALE_PX(20));
    lv_obj_t * delete_image = lv_obj_get_child(group_songs_delete_btn, 0);
    if (delete_image) {
        lv_obj_set_size(delete_image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(delete_image, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_style(delete_image, gui_theme_accent_style(), 0);
    }
    lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);

    group_songs_list = build_song_list_body(scr);
    lv_obj_set_scrollbar_mode(group_songs_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(group_songs_list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(group_songs_list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(group_songs_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_style(group_songs_list, gui_theme_accent_style(), LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(group_songs_list, group_songs_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(group_songs_list, group_songs_wake_cb, LV_EVENT_PRESSED, NULL);

    library_thumbnails_bind_list(scr, group_songs_list, LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM);

    finalize_screen_navigation(scr);
    return scr;
}

library_thumbnail_context_t library_thumbnail_genre_context(void) {
    int view = genre_browse_view();
    if (view == GENRE_VIEW_ALBUMS)
        return (library_thumbnail_context_t) {
            .key_kind = THUMBNAIL_KEY_ALBUM,
            .artist_group_kind = METADATA_DB_GROUP_ARTIST
        };
    return (library_thumbnail_context_t) {
        .key_kind = THUMBNAIL_KEY_ARTIST,
        .artist_group_kind = view == GENRE_VIEW_ALBUM_ARTISTS
            ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST
    };
}

bool library_thumbnail_policy(library_thumbnail_policy_t policy) {
    /* The warmer thread reads LAZY too: library_rescan_active is atomic. */
    bool rescanning = library_rescan_active;
    if (policy == LIBRARY_THUMB_POLICY_LAZY) return rescanning;
    return rescanning || gui_library_sd_format_is_active();
}

void library_thumbnail_show_busy(library_thumbnail_cover_scope_t scope) {
    const char *title = scope == LIBRARY_THUMB_COVER_ALL
        ? TR("Refreshing\nall covers...") : TR("Reloading\ncover...");
    library_rescan_token = gui_busy_show(title, TR("Preparing cover refresh..."));
    gui_busy_set_progress(library_rescan_token, 0);
}

/* Called after list deletion: unregistering is pointer identity only, so it never
 * touches a freed LVGL object. */
void forget_deleted_library_lists(void) {
    lv_obj_t *dead[] = { all_songs_list, artists_list, albums_list, album_artist_list,
                         recently_added_list, genres_list };
    for (size_t i = 0; i < sizeof(dead) / sizeof(dead[0]); ++i)
        if (dead[i]) library_thumbnails_forget_list(dead[i]);
    all_songs_list = artists_list = albums_list = album_artist_list = recently_added_list = NULL;
    all_songs_bar = (songs_bar_t) { 0 };
    recent_bar = (songs_bar_t) { 0 };
}

const char * group_songs_search_name_of(int index) {
    if (index >= 0 && index < group_songs_count && group_songs_entries[index].title)
        return group_songs_entries[index].title;
    return "";
}

void group_songs_search_row_click_cb(int display_index) {
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    group_play_at(real_index);
}

void group_songs_search_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    if (real_index >= 0 && real_index < group_songs_count && group_songs_entries[real_index].path)
        open_song_context_menu(group_songs_entries[real_index].path);
}

void update_group_songs_search_button(bool editing) {
    /* Album views have no Edit action; give their title the space up to
     * Search. Editable playlists reserve whichever action remains visible:
     * Edit in normal mode, trash while editing. */
    lv_obj_t * title_action = group_songs_edit_m3u_path
        ? (group_songs_edit_mode ? group_songs_delete_btn : group_songs_edit_btn)
        : NULL;
    library_search_update_header(SEARCH_BINDING_GROUP_SONGS, group_songs_title_label,
                                 title_action, editing);
}

static void refresh_group_songs_search_now_playing(void) {
    if (!group_songs_search_list) return;
    int target = -1;
    if (library_search_is_active(SEARCH_BINDING_GROUP_SONGS)) {
        int match = group_songs_now_playing_index();
        if (match >= 0) target = library_search_find_filtered_row(SEARCH_BINDING_GROUP_SONGS, match);
    }
    compact_list_set_now_playing(group_songs_search_list, target);
}

/* Frees paths[0..count) and the array; returns their entries, NULL when there are none or it ran out of memory. */
group_song_entry_t * group_song_entries_take_paths(char ** paths, int count) {
    group_song_entry_t * entries = count > 0 ? build_group_song_entries_from_paths(paths, count) : NULL;
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    return entries;
}

/* The page shows the entries of paths[0..count) (consumed). */
static void group_songs_set_from_paths(char ** paths, int count) {
    group_song_entry_t * entries = group_song_entries_take_paths(paths, count);
    set_group_songs_entries_owned(entries, entries ? count : 0);
}

static void refresh_group_songs_paths_in_place(void) {
    group_song_entry_t * entries = calloc((size_t)(group_songs_count > 0 ? group_songs_count : 1),
                                          sizeof(*entries));
    if (!entries) {
        set_group_songs_entries_owned(NULL, 0);
        populate_group_songs_rows();
        return;
    }
    int count = 0;
    for (int i = 0; i < group_songs_count; i++) {
        song_row_t song;
        if (!metadata_db_get_song_by_path(group_songs_entries[i].path, &song)) continue;
        char title[384];
        if (group_songs_music_submenu) format_music_submenu_identity(&song, title, sizeof(title));
        else format_song_identity(&song, title, sizeof(title));
        entries[count].path = strdup(song.path);
        entries[count].title = strdup(title);
        entries[count].song_id = song.id;
        entries[count].artwork_key = song_album_thumbnail_key(&song);
        entries[count].disc_number = group_songs_entries[i].disc_number;
        entries[count].show_disc_header = group_songs_entries[i].show_disc_header;
        if (!entries[count].path || !entries[count].title) {
            free_group_song_entries(entries, count + 1);
            set_group_songs_entries_owned(NULL, 0);
            populate_group_songs_rows();
            return;
        }
        count++;
    }
    set_group_songs_entries_owned(entries, count);
    populate_group_songs_rows();
}

void group_songs_replace_refreshed_entries(group_song_entry_t * entries, int count) {
    group_songs_page_start = 0;
    set_group_songs_entries_owned(entries, count);
    populate_group_songs_rows();
}

void refresh_group_songs_from_database(void) {
    /* An album page is read again on the album worker, so a large album does not freeze the UI;
     * the rows are replaced when it lands. */
    if (group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM &&
        album_refresh_in_place_start(group_songs_refresh_album, group_songs_refresh_album_artist,
                                     group_songs_refresh_artist_filter))
        return;
    group_songs_page_start = 0;
    switch (group_songs_refresh_source) {
        case GROUP_SONGS_REFRESH_FAVORITES: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_favorite_songs(&paths, &count);
            group_songs_set_from_paths(paths, count);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_MOST_PLAYED: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            group_songs_set_from_paths(paths, count);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_M3U: {
            char ** paths = NULL;
            int count = 0;
            bool loaded = group_songs_refresh_m3u_path[0] &&
                          playlist_files_read(group_songs_refresh_m3u_path, &paths, &count);
            group_songs_set_from_paths(paths, count);
            group_songs_file_stat_valid = loaded &&
                stat(group_songs_refresh_m3u_path, &group_songs_file_stat) == 0;
            if (!group_songs_reset_selection()) {
                group_songs_edit_mode = false;
                group_songs_clear_selection();
            }
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ALBUM: {
            /* Only reached when the worker could not be started. */
            int count = group_songs_refresh_artist_filter[0] ? 0
                : album_group_song_count(group_songs_refresh_album, group_songs_refresh_album_artist);
            int loaded_count = 0;
            group_song_entry_t * entries = load_album_entries_filtered(group_songs_refresh_album,
                group_songs_refresh_album_artist, count, group_songs_refresh_artist_filter, &loaded_count, NULL);
            set_group_songs_entries_owned(entries, entries ? loaded_count : 0);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ARTIST:
            if (!artist_albums_show_all_songs_internal(false, group_songs_refresh_artist_name,
                                                       group_songs_refresh_artist_kind)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_GENRE:
            if (!show_genre_songs_internal(false, group_songs_refresh_genre)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_NONE:
        default:
            refresh_group_songs_paths_in_place();
            break;
    }
}
