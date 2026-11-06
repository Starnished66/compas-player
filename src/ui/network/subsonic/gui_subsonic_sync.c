#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_covers.h"
#include "gui_subsonic_album.h"
#include "gui_subsonic_menu.h"
#include "../../library/library_search.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/remote/subsonic/subsonic_covers.h"
#include "../../../network/wifi/wifi_status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* What is known about each cached list of the connected server. */
subsonic_cache_entry_t subsonic_entries[SUBSONIC_LIST_ALL_KINDS];

/* The PING waiting for the busy worker to be collected. */
subsonic_next_job_t subsonic_next;

/* What each list shows next: its cache read, a parked result, a refresh due.
 * The two songs kinds follow the one album or playlist the Songs screen holds. */
static subsonic_list_state_t subsonic_lists[SUBSONIC_LIST_ALL_KINDS];

subsonic_list_kind_t subsonic_songs_kind = SUBSONIC_LIST_ALBUM_SONGS;
char subsonic_songs_id[64];
bool subsonic_songs_prune_due; /* a songs file was added: trim the old ones when the worker is idle */

/* The Cover Flow row asked for the albums and opens the carousel once they are in. */
bool subsonic_cover_flow_pending;

static int subsonic_spinning_list = -1; /* the list whose refresh button is spinning, or -1 */
#define SUBSONIC_LIST_STALE_SECONDS (15 * 60)

static void subsonic_reset_lists(void) {
    subsonic_cover_flow_pending = false;
    for (int kind = 0; kind < SUBSONIC_LIST_ALL_KINDS; kind++) subsonic_list_reset(&subsonic_lists[kind]);
}

/* The album or playlist id behind a songs kind, NULL for the server-wide lists. */
static const char * subsonic_list_id(subsonic_list_kind_t kind) {
    return subsonic_list_is_songs(kind) ? subsonic_songs_id : NULL;
}

/* Drops every browse cache (new connection). Lists are emptied first because
 * they borrow label text from the caches. */
void subsonic_clear_browse_caches(void) {
    subsonic_views_clear();
    subsonic_refresh_cancel(); /* it belongs to the server being left */
    subsonic_reset_lists();
    subsonic_next_clear(&subsonic_next);
    memset(subsonic_entries, 0, sizeof(subsonic_entries));
}

/* What a songs file is judged by: for an album the library stamp its list was
 * fetched under, for a playlist the song count and changed time from the
 * playlists list. Zero (nothing comparable) when unknown. */
void subsonic_songs_stamp(subsonic_list_kind_t kind, const char * id, subsonic_library_stamp_t * stamp) {
    memset(stamp, 0, sizeof(*stamp));
    if (kind == SUBSONIC_LIST_ALBUM_SONGS) {
        *stamp = subsonic_entries[SUBSONIC_LIST_ALBUMS].stamp;
        return;
    }
    for (int i = 0; i < subsonic_playlists_count; i++) {
        const subsonic_playlist_t * playlist = &subsonic_playlists_cache[i];
        if (strcmp(playlist->id, id) != 0) continue;
        subsonic_playlist_stamp(playlist, stamp);
        return;
    }
}

static void subsonic_set_refresh_spinner(int kind, bool on) {
    if (kind >= 0 && subsonic_refresh_buttons[kind]) set_header_refresh_action_busy(subsonic_refresh_buttons[kind], on);
}

static bool subsonic_start_refresh(subsonic_refresh_mode_t mode, subsonic_list_kind_t kind, bool force) {
    subsonic_refresh_job_t job = { .mode = mode, .kind = kind, .server = subsonic_server_from_settings(),
                                   .force = force, .entry = subsonic_entries[kind] };
    if (subsonic_list_is_songs(kind)) {
        snprintf(job.id, sizeof(job.id), "%s", subsonic_songs_id);
        subsonic_songs_stamp(kind, subsonic_songs_id, &job.save_stamp);
    }
    if (!subsonic_refresh_start(&job)) return false;
    if (mode == SUBSONIC_REFRESH_FETCH && !subsonic_list_is_songs(kind)) { /* a songs fetch runs unseen */
        subsonic_set_refresh_spinner(subsonic_spinning_list, false); /* one button spins at a time */
        subsonic_spinning_list = (int) kind;
        subsonic_set_refresh_spinner((int) kind, true);
    }
    return true;
}

/* Starts the PING now, or queues it behind the busy worker. Without Wi-Fi
 * it would only fail after a wait, so the offline notice shows at once. */
void subsonic_ping_or_queue(void) {
    int level;
    if (!wifi_get_status(&level)) {
        subsonic_next_clear(&subsonic_next);
        show_info_toast(TR("Offline - showing saved library"));
        return;
    }
    if (subsonic_start_refresh(SUBSONIC_REFRESH_PING, SUBSONIC_LIST_ARTISTS, false)) return;
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_active_server_key(key);
    if (subsonic_refresh_active()) subsonic_next_store(&subsonic_next, key);
}

/* Runs the queued PING once the previous worker has been collected. */
static void subsonic_start_next(void) {
    if (subsonic_refresh_active()) return;
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_active_server_key(key);
    if (subsonic_next_take(&subsonic_next, key)) (void) subsonic_start_refresh(SUBSONIC_REFRESH_PING, SUBSONIC_LIST_ARTISTS, false);
}

/* Automatic start: waits while a foreground request runs, gives up without
 * Wi-Fi, otherwise starts now or remembers it for the poll when the worker is busy. */
static void subsonic_fetch_or_want(subsonic_list_kind_t kind, bool force) {
    bool * wants = &subsonic_lists[kind].wants_fetch;
    int level;
    if (subsonic_browse_active || subsonic_connect_active) *wants = true;
    else if (!wifi_get_status(&level)) *wants = false;
    else *wants = !subsonic_start_refresh(SUBSONIC_REFRESH_FETCH, kind, force);
}

/* A songs file is as good as the stamp it was saved under; a list ages. */
static bool subsonic_list_stale(subsonic_list_kind_t kind) {
    if (subsonic_list_is_songs(kind)) {
        subsonic_library_stamp_t now;
        subsonic_songs_stamp(kind, subsonic_songs_id, &now);
        if (kind == SUBSONIC_LIST_PLAYLIST_SONGS) return !subsonic_playlist_stamp_unchanged(&subsonic_entries[kind].stamp, &now);
        return !subsonic_stamp_unchanged(&subsonic_entries[kind].stamp, &now);
    }
    long long age = (long long) time(NULL) - subsonic_entries[kind].synced_at;
    return age < 0 || age > SUBSONIC_LIST_STALE_SECONDS;
}

static void subsonic_refresh_if_stale(subsonic_list_kind_t kind) {
    if (subsonic_list_stale(kind)) subsonic_fetch_or_want(kind, false);
}

static bool subsonic_install_list(subsonic_list_kind_t kind, void * items, int count, bool keep_scroll) {
    switch (kind) {
        case SUBSONIC_LIST_ARTISTS: return subsonic_install_artists(items, count, keep_scroll);
        case SUBSONIC_LIST_ALBUMS: return subsonic_install_all_albums(items, count, keep_scroll);
        case SUBSONIC_LIST_PLAYLISTS: return subsonic_install_playlists(items, count, keep_scroll);
        default: return subsonic_install_songs(items, count, keep_scroll);
    }
}

/* A search keeps row numbers of the list it was run on: leave that list alone. */
static bool subsonic_list_search_active(subsonic_list_kind_t kind) {
    if (kind == SUBSONIC_LIST_ARTISTS) return library_search_is_active(SEARCH_BINDING_SUBSONIC_ARTISTS);
    if (kind == SUBSONIC_LIST_ALBUMS) return library_search_is_active(SEARCH_BINDING_SUBSONIC_ALBUMS);
    return false;
}

/* The one place a LOAD, FETCH or parked list goes on screen. A search keeps
 * the row numbers of the list it ran on, so a new list waits for it to end. */
static void subsonic_install_result(subsonic_list_kind_t kind, void * items, int count,
                                    const subsonic_cache_entry_t * entry) {
    subsonic_list_state_t * st = &subsonic_lists[kind];
    if (subsonic_list_search_active(kind)) {
        subsonic_list_park(st, items, count, entry);
        return;
    }
    subsonic_list_supersede(st);
    if (!subsonic_install_list(kind, items, count, true)) {
        show_error_toast(TR("Not enough memory to load from server"));
        return;
    }
    subsonic_entries[kind] = *entry;
}

/* Loads a server-wide list in the foreground (busy screen with Cancel), then shows it. */
static void subsonic_browse_list(subsonic_list_kind_t kind) {
    static const subsonic_browse_kind_t browse_kinds[] = { SUBSONIC_BROWSE_ARTISTS, SUBSONIC_BROWSE_ALL_ALBUMS,
                                                           SUBSONIC_BROWSE_PLAYLISTS };
    start_subsonic_browse(browse_kinds[kind], NULL,
                          kind == SUBSONIC_LIST_ARTISTS ? TR("Artists") :
                          kind == SUBSONIC_LIST_ALBUMS ? TR("Albums") : TR("Playlists"));
}

/* A saved list that cannot be read, opened with nothing on screen: leave the
 * empty list and load it as on a first open, instead of a blank list with a
 * spinner. False (the caller refreshes in the background) when the list is not
 * the open screen or the network is not there. */
static bool subsonic_reload_unreadable(subsonic_list_kind_t kind) {
    lv_obj_t * screens[SUBSONIC_LIST_KINDS] = { subsonic_artists_screen, subsonic_albums_screen,
                                                 subsonic_playlists_screen };
    int level;
    if (subsonic_list_is_songs(kind) || subsonic_list_loaded(kind) || subsonic_foreground_busy()) return false;
    if (lv_screen_active() != screens[kind] || !wifi_get_status(&level)) return false;
    nav_pop_immediate(); /* an animated pop would unload the busy screen and cancel the load */
    subsonic_browse_list(kind);
    return true;
}

static void subsonic_apply_loaded(subsonic_list_kind_t kind, const subsonic_load_result_t * result) {
    if (!result->ok) { /* an unreadable cache is as good as none */
        if (!subsonic_reload_unreadable(kind)) subsonic_fetch_or_want(kind, true);
        return;
    }
    subsonic_install_result(kind, result->items, result->count, &result->entry);
    subsonic_refresh_if_stale(kind);
}

/* The server dropped an album or playlist: the list it was in is out of date.
 * A list never opened has nothing to correct. */
void subsonic_refresh_parent_list(bool album) {
    subsonic_list_kind_t list = album ? SUBSONIC_LIST_ALBUMS : SUBSONIC_LIST_PLAYLISTS;
    if (subsonic_list_loaded(list)) subsonic_fetch_or_want(list, true);
}

static void subsonic_apply_fetched(const subsonic_refresh_job_t * job) {
    if (job->not_found) {
        subsonic_refresh_parent_list(job->kind == SUBSONIC_LIST_ALBUM_SONGS);
        return;
    }
    if (!job->ok) {
        if (job->force) show_error_toast(TR("Failed to load from server"));
        return;
    }
    if (subsonic_list_is_songs(job->kind) && (job->kind != subsonic_songs_kind || strcmp(job->id, subsonic_songs_id) != 0)) {
        free(job->items); /* the screen moved on: a refresh queued for the new one stays queued */
        return;
    }
    subsonic_lists[job->kind].wants_fetch = false; /* this refresh did what was due */
    if (job->changed) subsonic_install_result(job->kind, job->items, job->count, &job->entry);
    else subsonic_entries[job->kind] = job->entry;
}

/* Per list: show a parked result once its search is over, run a refresh that was due. */
static void subsonic_poll_list(subsonic_list_kind_t kind) {
    subsonic_list_state_t * st = &subsonic_lists[kind];
    subsonic_load_result_t result;
    if (subsonic_list_load_take(st, &result)) subsonic_apply_loaded(kind, &result);
    if (st->wants_load) {
        subsonic_server_t server = subsonic_server_from_settings();
        if (!subsonic_list_load_open(st, &server, kind, subsonic_list_id(kind))) subsonic_fetch_or_want(kind, true);
    }
    void * items;
    int count;
    subsonic_cache_entry_t entry;
    if (!subsonic_list_search_active(kind) && subsonic_list_unpark(st, &items, &count, &entry))
        subsonic_install_result(kind, items, count, &entry);
    if (subsonic_list_fetch_due(st, !subsonic_refresh_active())) subsonic_fetch_or_want(kind, false);
}

/* Trims old songs and cover files once the worker has nothing else to do. */
static void subsonic_prune_if_due(void) {
    if (subsonic_covers_prune_due()) subsonic_songs_prune_due = true;
    if (!subsonic_songs_prune_due || subsonic_browse_active || subsonic_connect_active) return;
    if (subsonic_start_refresh(SUBSONIC_REFRESH_PRUNE, SUBSONIC_LIST_ARTISTS, false)) subsonic_songs_prune_due = false;
}

static void subsonic_cover_flow_poll(void);

void poll_subsonic_refresh(void) {
    gui_subsonic_album_poll();
    if (gui_navigation_transition_in_progress()) return;
    subsonic_refresh_job_t job;
    if (subsonic_refresh_take(&job)) {
        if (job.mode == SUBSONIC_REFRESH_FETCH) subsonic_apply_fetched(&job);
        else if (job.mode == SUBSONIC_REFRESH_PING && !job.ok) show_info_toast(TR("Offline - showing saved library"));
    }
    subsonic_start_next();
    for (int kind = 0; kind < SUBSONIC_LIST_ALL_KINDS; kind++) subsonic_poll_list((subsonic_list_kind_t) kind);
    subsonic_prune_if_due();
    subsonic_cover_flow_poll();
    if (!subsonic_refresh_active() && subsonic_spinning_list >= 0) {
        subsonic_set_refresh_spinner(subsonic_spinning_list, false);
        subsonic_spinning_list = -1;
    }
}

static bool subsonic_load_cached_list(subsonic_list_kind_t kind) {
    subsonic_server_t server = subsonic_server_from_settings();
    return subsonic_cache_list_exists(&server, kind) &&
           subsonic_list_load_open(&subsonic_lists[kind], &server, kind, NULL);
}

/* A cached list opens at once, empty for the moment the card read takes. A
 * read already under way for this list is reused. */
static bool subsonic_open_from_cache(subsonic_list_kind_t kind) {
    if (!subsonic_load_cached_list(kind)) return false;
    subsonic_show_list(kind);
    return true;
}

static void subsonic_open_list(subsonic_list_kind_t kind) {
    if (subsonic_foreground_busy()) return;
    if (kind == SUBSONIC_LIST_ARTISTS) subsonic_restore_artists_view();
    if (subsonic_list_loaded(kind)) {
        subsonic_show_list(kind);
        subsonic_refresh_if_stale(kind);
    } else if (!subsonic_open_from_cache(kind)) {
        subsonic_browse_list(kind);
    }
}

/* A cached album or playlist opens at once, empty for the moment the card read
 * takes, and the server is asked in the background only when the file looks stale. */
static bool subsonic_open_songs_from_cache(subsonic_list_kind_t kind, const char * id, const char * title) {
    subsonic_server_t server = subsonic_server_from_settings();
    if (!subsonic_cache_songs_exists(&server, kind, id)) return false;
    subsonic_clear_songs();
    subsonic_set_songs_context(kind, id, title);
    if (!subsonic_list_load_open(&subsonic_lists[kind], &server, kind, id)) return false;
    nav_push(subsonic_songs_screen);
    return true;
}

void subsonic_open_songs(subsonic_list_kind_t kind, const char * id, const char * title) {
    if (subsonic_foreground_busy() || subsonic_open_songs_from_cache(kind, id, title)) return;
    start_subsonic_browse(kind == SUBSONIC_LIST_ALBUM_SONGS ? SUBSONIC_BROWSE_ALBUM_SONGS : SUBSONIC_BROWSE_PLAYLIST_SONGS,
                          id, title);
}

void subsonic_menu_artists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) subsonic_open_list(SUBSONIC_LIST_ARTISTS);
}

void subsonic_menu_playlists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) subsonic_open_list(SUBSONIC_LIST_PLAYLISTS);
}

void gui_subsonic_open_all_albums(void) { subsonic_open_list(SUBSONIC_LIST_ALBUMS); }
void subsonic_cover_flow_open_album(const char * id, const char * title) {
    subsonic_open_songs(SUBSONIC_LIST_ALBUM_SONGS, id, title);
}

/* Opens at once when the albums are in memory; otherwise loads them (cache first, else the
 * server) and the poll opens it when they arrive, without showing the Albums screen. */
void subsonic_menu_cover_flow_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_list_loaded(SUBSONIC_LIST_ALBUMS)) {
        subsonic_open_cover_flow();
        return;
    }
    if (subsonic_foreground_busy()) return;
    bool cached = subsonic_load_cached_list(SUBSONIC_LIST_ALBUMS);
    if (!cached) subsonic_browse_list(SUBSONIC_LIST_ALBUMS);
    subsonic_cover_flow_pending = cached || subsonic_browse_active;
}

/* Once the albums request is over, opens the carousel if the user is still on the menu. */
static void subsonic_cover_flow_poll(void) {
    const subsonic_list_state_t * st = &subsonic_lists[SUBSONIC_LIST_ALBUMS];
    if (!subsonic_cover_flow_pending || subsonic_browse_active) return;
    if (lv_screen_active() == gui_busy_get_screen()) return; /* the busy screen is still leaving */
    if (!subsonic_list_loaded(SUBSONIC_LIST_ALBUMS) && (st->load || st->wants_load || st->wants_fetch || subsonic_refresh_active()))
        return; /* a cache read or the background fetch behind it is still coming */
    subsonic_cover_flow_pending = false;
    if (subsonic_list_loaded(SUBSONIC_LIST_ALBUMS) && lv_screen_active() == gui_subsonic_menu_screen()) subsonic_open_cover_flow();
}

void subsonic_refresh_button_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    for (int kind = 0; kind < SUBSONIC_LIST_KINDS; kind++) {
        if (lv_event_get_current_target(e) != subsonic_refresh_buttons[kind] || !gui_subsonic_wifi_connected_guard()) continue;
        if (subsonic_connect_active || subsonic_browse_active ||
            !subsonic_start_refresh(SUBSONIC_REFRESH_FETCH, (subsonic_list_kind_t) kind, true))
            show_info_toast(TR("Previous request still finishing"));
    }
}


void gui_subsonic_sync_reset(void) {
    subsonic_refresh_shutdown();
    subsonic_reset_lists();
    subsonic_spinning_list = -1;
    memset(subsonic_entries, 0, sizeof(subsonic_entries));
    subsonic_next_clear(&subsonic_next);
}

void gui_subsonic_sync_stop_network(void) {
    subsonic_refresh_cancel(); /* its result is dropped when the poll collects it */
    gui_subsonic_covers_reset();
    /* Only network work stops: cache loads and parked results need no network. */
    for (int kind = 0; kind < SUBSONIC_LIST_ALL_KINDS; kind++) subsonic_lists[kind].wants_fetch = false;
    subsonic_next_clear(&subsonic_next);
}

void gui_subsonic_sync_shutdown(void) {
    subsonic_reset_lists();
    subsonic_refresh_shutdown();
    subsonic_next_clear(&subsonic_next);
    subsonic_set_refresh_spinner(subsonic_spinning_list, false);
    subsonic_spinning_list = -1;
}

static bool subsonic_loads_active(void) {
    for (int kind = 0; kind < SUBSONIC_LIST_ALL_KINDS; kind++)
        if (subsonic_lists[kind].load) return true;
    return false;
}

bool gui_subsonic_sync_busy(void) {
    return subsonic_refresh_active() || subsonic_loads_active();
}

void subsonic_sync_songs_context(subsonic_list_kind_t kind, const char * id) {
    for (int k = SUBSONIC_LIST_ALBUM_SONGS; k < SUBSONIC_LIST_ALL_KINDS; k++) {
        subsonic_list_reset(&subsonic_lists[k]);
        memset(&subsonic_entries[k], 0, sizeof(subsonic_entries[k]));
    }
    subsonic_songs_kind = kind;
    snprintf(subsonic_songs_id, sizeof(subsonic_songs_id), "%s", id);
}
