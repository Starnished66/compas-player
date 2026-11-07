#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_sections.h"
#include "gui_subsonic_genres.h"
#include "gui_subsonic_artist.h"
#include "../../../core/process/worker_notify.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char ** album_ids;
    int album_count;
    subsonic_browse_kind_t kind;
    subsonic_server_t server;
    char id[64];
    char title[128];
    subsonic_library_stamp_t stamp; /* songs: what the saved file is judged by later */
} subsonic_browse_request_t;


static gui_busy_handle_t subsonic_browse_token = 0;
static pthread_t subsonic_browse_thread;
static http_cancel_token_t subsonic_browse_cancel;

bool subsonic_browse_active = false;
/* Set when the user cancelled or the request timed out: the worker is still
 * winding down and whatever it returns is discarded, never shown. */
static bool subsonic_browse_abandoned = false;
/* The whole-library request pages through many requests, each with its own
 * timeout, so it has no overall deadline. */
static bool subsonic_browse_paged = false;
static uint32_t subsonic_browse_started_at = 0;

static atomic_bool subsonic_browse_done_flag = false;

static volatile bool subsonic_browse_success_flag = false;

static subsonic_browse_kind_t subsonic_browse_result_kind;

static char subsonic_browse_result_title[128];

static char subsonic_browse_result_id[64];

static char subsonic_browse_result_error[256];

/* The server said the album or playlist is gone. */
static bool subsonic_browse_result_not_found = false;

static subsonic_artist_t * subsonic_browse_result_artists = NULL;

static subsonic_song_t * subsonic_browse_result_songs = NULL;

static subsonic_album_t * subsonic_browse_result_albums = NULL;

static subsonic_playlist_t * subsonic_browse_result_playlists = NULL;

static subsonic_genre_t * subsonic_browse_result_genres = NULL;

static subsonic_starred_t subsonic_browse_result_starred;

static int subsonic_browse_result_count = 0;

/* Set by the worker when it kept the list on the card: which list, and its entry. */
static int subsonic_browse_result_list = -1;
static subsonic_cache_entry_t subsonic_browse_result_entry;





/* Keeps a freshly downloaded artists, albums or playlists list on the card. */
static void subsonic_browse_save_cache(const subsonic_browse_request_t * req, int count) {
    subsonic_list_kind_t kind;
    const void * items;
    switch (req->kind) {
        case SUBSONIC_BROWSE_ARTISTS: kind = SUBSONIC_LIST_ARTISTS; items = subsonic_browse_result_artists; break;
        case SUBSONIC_BROWSE_ALL_ALBUMS: kind = SUBSONIC_LIST_ALBUMS; items = subsonic_browse_result_albums; break;
        case SUBSONIC_BROWSE_PLAYLISTS: kind = SUBSONIC_LIST_PLAYLISTS; items = subsonic_browse_result_playlists; break;
        case SUBSONIC_BROWSE_ALBUM_SONGS: kind = SUBSONIC_LIST_ALBUM_SONGS; items = subsonic_browse_result_songs; break;
        case SUBSONIC_BROWSE_PLAYLIST_SONGS: kind = SUBSONIC_LIST_PLAYLIST_SONGS; items = subsonic_browse_result_songs; break;
        default: return;
    }
    subsonic_browse_result_entry = (subsonic_cache_entry_t){ .synced_at = (long long) time(NULL),
                                                             .hash = subsonic_cache_hash(kind, items, count),
                                                             .stamp = req->stamp };
    subsonic_browse_result_list = (int) kind;
    if (subsonic_list_is_songs(kind))
        (void) subsonic_cache_songs_save(&req->server, kind, req->id, items, count, &subsonic_browse_result_entry);
    else
        (void) subsonic_cache_save(&req->server, kind, items, count, &subsonic_browse_result_entry);
}

/* A failed songs request: if the server no longer has the item, its file goes. */
static void subsonic_browse_forget_gone(const subsonic_browse_request_t * req) {
    bool album = req->kind == SUBSONIC_BROWSE_ALBUM_SONGS;
    if ((!album && req->kind != SUBSONIC_BROWSE_PLAYLIST_SONGS) || subsonic_last_error_code() != SUBSONIC_ERROR_NOT_FOUND)
        return;
    subsonic_browse_result_not_found = true;
    subsonic_cache_songs_remove(&req->server, album ? SUBSONIC_LIST_ALBUM_SONGS : SUBSONIC_LIST_PLAYLIST_SONGS, req->id);
}

/* One bounded foreground worker and deadline covers the complete artist. */
static bool subsonic_browse_artist_songs(const subsonic_browse_request_t * req, int * count) {
    int capacity = 0;
    for (int i = 0; i < req->album_count; i++) {
        if (http_cancel_token_is_cancelled(&subsonic_browse_cancel)) return false;
        subsonic_song_t * songs = NULL;
        int n = 0;
        if (!subsonic_get_album_songs(&req->server, req->album_ids[i], &songs, &n, &subsonic_browse_cancel)) {
            free(songs);
            return false;
        }
        if (n < 0 || n > 100000 - *count) { free(songs); return false; }
        bool ok = gui_subsonic_append_songs(&subsonic_browse_result_songs, count, &capacity, songs, n);
        free(songs);
        if (!ok) return false;
    }
    return !http_cancel_token_is_cancelled(&subsonic_browse_cancel);
}

static void subsonic_browse_free_request(subsonic_browse_request_t * req) {
    for (int i = 0; i < req->album_count; i++) free(req->album_ids[i]);
    free(req->album_ids);
    free(req);
}

static bool subsonic_browse_fetch(const subsonic_browse_request_t * req, int * count) {
    switch (req->kind) {
        case SUBSONIC_BROWSE_ARTIST_SONGS:
            return subsonic_browse_artist_songs(req, count);
        case SUBSONIC_BROWSE_ARTISTS:
            return subsonic_get_artists(&req->server, &subsonic_browse_result_artists, count,
                                      &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_ALBUM_SONGS:
            return subsonic_get_album_songs(&req->server, req->id, &subsonic_browse_result_songs, count,
                                           &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_ARTIST_ALBUMS:
            return subsonic_get_artist_albums(&req->server, req->id, &subsonic_browse_result_albums, count,
                                             &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_PLAYLIST_SONGS:
            return subsonic_get_playlist_songs(&req->server, req->id, &subsonic_browse_result_songs, count,
                                              &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_PLAYLISTS:
            return subsonic_get_playlists(&req->server, &subsonic_browse_result_playlists, count,
                                         &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_ALL_ALBUMS:
            return subsonic_get_all_albums(&req->server, &subsonic_browse_result_albums, count,
                                          &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_ALBUM_LIST:
            return subsonic_get_album_list(&req->server, req->id, strcmp(req->id, "byGenre") == 0 ? req->title : NULL,
                                         &subsonic_browse_result_albums, count, &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_GENRES:
            return subsonic_get_genres(&req->server, &subsonic_browse_result_genres, count, &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_RANDOM_SONGS:
            return subsonic_get_random_songs(&req->server, 0, &subsonic_browse_result_songs, count,
                                           &subsonic_browse_cancel);
        case SUBSONIC_BROWSE_STARRED:
            return subsonic_get_starred(&req->server, &subsonic_browse_result_starred, &subsonic_browse_cancel);
    }
    return false;
}

static void subsonic_browse_publish(const subsonic_browse_request_t * req, int count, bool ok) {
    subsonic_browse_result_kind = req->kind;
    snprintf(subsonic_browse_result_title, sizeof(subsonic_browse_result_title), "%s", req->title);
    snprintf(subsonic_browse_result_id, sizeof(subsonic_browse_result_id), "%s", req->id);
    subsonic_browse_result_count = count;
    subsonic_browse_success_flag = ok;
    atomic_store_explicit(&subsonic_browse_done_flag, true, memory_order_release);
}

static void * subsonic_browse_thread_func(void * arg) {
    subsonic_browse_request_t * req = (subsonic_browse_request_t *) arg;
    int count = 0;

    bool ok = subsonic_browse_fetch(req, &count);

    if (ok) {
        subsonic_browse_save_cache(req, count);
    } else {
        subsonic_browse_forget_gone(req);
        subsonic_copy_request_error(subsonic_browse_result_error, sizeof(subsonic_browse_result_error),
                                    TR("Failed to load from server"));
    }
    subsonic_browse_publish(req, count, ok);
    subsonic_browse_free_request(req);
    worker_notify_ui();
    return NULL;
}

/* Busy screen Cancel or Back: stop the request and drop its result. The poller
 * dismisses the busy screen and reaps the worker. */

static void subsonic_browse_cancel_cb(void * user) {
    (void)user;
    if (!subsonic_browse_active) return;
    subsonic_browse_abandoned = true;
    http_cancel_token_cancel(&subsonic_browse_cancel);
}

static subsonic_browse_request_t * subsonic_browse_new_request(subsonic_browse_kind_t kind, const char * id,
                                                                const char * title) {
    subsonic_browse_request_t * req = calloc(1, sizeof(*req));
    if (!req) {
        show_error_toast(TR("Not enough memory to load from server"));
        return NULL;
    }
    if (kind == SUBSONIC_BROWSE_ARTIST_SONGS) {
        req->album_ids = gui_subsonic_copy_album_ids(subsonic_albums_cache, subsonic_albums_count);
        if (!req->album_ids) {
            free(req);
            show_error_toast(TR("Not enough memory to load from server"));
            return NULL;
        }
        req->album_count = subsonic_albums_count;
    }
    req->kind = kind;
    req->server = subsonic_server_from_settings();
    if (id) snprintf(req->id, sizeof(req->id), "%s", id);
    if (title) snprintf(req->title, sizeof(req->title), "%s", title);
    if (kind == SUBSONIC_BROWSE_ALBUM_SONGS) subsonic_songs_stamp(SUBSONIC_LIST_ALBUM_SONGS, req->id, &req->stamp);
    if (kind == SUBSONIC_BROWSE_PLAYLIST_SONGS) subsonic_songs_stamp(SUBSONIC_LIST_PLAYLIST_SONGS, req->id, &req->stamp);
    return req;
}

/* Clears the previous result and marks a request as under way. */
static void subsonic_browse_begin(subsonic_browse_kind_t kind) {
    subsonic_browse_result_not_found = false;
    subsonic_browse_result_artists = NULL;
    subsonic_browse_result_error[0] = '\0';
    subsonic_browse_result_songs = NULL;
    subsonic_browse_result_albums = NULL;
    subsonic_browse_result_playlists = NULL;
    subsonic_browse_result_genres = NULL;
    memset(&subsonic_browse_result_starred, 0, sizeof(subsonic_browse_result_starred));
    subsonic_browse_result_count = 0;
    subsonic_browse_result_list = -1;
    atomic_store_explicit(&subsonic_browse_done_flag, false, memory_order_relaxed);
    subsonic_browse_success_flag = false;
    subsonic_browse_abandoned = false;
    subsonic_browse_paged = kind == SUBSONIC_BROWSE_ALL_ALBUMS;
    subsonic_browse_started_at = lv_tick_get();
    http_cancel_token_init(&subsonic_browse_cancel);
    subsonic_browse_active = true;
}

void start_subsonic_browse(subsonic_browse_kind_t kind, const char * id, const char * title) {
    if (subsonic_foreground_busy()) return;
    if (!gui_subsonic_wifi_connected_guard()) return; /* cache-first opens never get here */
    subsonic_refresh_cancel(); /* a foreground request has the network to itself */
    subsonic_next_clear(&subsonic_next);

    subsonic_browse_request_t * req = subsonic_browse_new_request(kind, id, title);
    if (!req) return;
    subsonic_browse_begin(kind);
    subsonic_browse_token = gui_busy_show(TR("Loading from server..."), "");
    gui_busy_set_cancel(subsonic_browse_token, subsonic_browse_cancel_cb, NULL);
    if (pthread_create(&subsonic_browse_thread, NULL, subsonic_browse_thread_func, req) != 0) {
        subsonic_browse_active = false;
        http_cancel_token_destroy(&subsonic_browse_cancel);
        subsonic_browse_free_request(req);
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
        show_error_toast(TR("Thread launch failed"));
    }
}

/* Stores a finished albums request in its own cache and shows it. The old
 * array is freed only after the list stopped pointing at it; if the list
 * cannot be filled the new data is dropped and the old stays. */

static bool subsonic_commit_albums_result(void) {
    bool all = subsonic_browse_result_kind == SUBSONIC_BROWSE_ALL_ALBUMS;
    subsonic_album_t * fresh = subsonic_browse_result_albums;
    subsonic_browse_result_albums = NULL;
    subsonic_albums_mode_t mode = all ? SUBSONIC_ALBUMS_ALL
        : subsonic_browse_result_kind == SUBSONIC_BROWSE_ALBUM_LIST ? SUBSONIC_ALBUMS_LIST : SUBSONIC_ALBUMS_ARTIST;
    if (!subsonic_show_albums(fresh, subsonic_browse_result_count, subsonic_browse_result_title, mode,
                              subsonic_browse_result_title, subsonic_browse_result_id)) {
        free(fresh);
        return false;
    }
    subsonic_keep_albums(all, fresh, subsonic_browse_result_count);
    return true;
}

static bool subsonic_commit_albums_for_result(void) {
    if (!subsonic_cover_flow_pending || subsonic_browse_result_kind != SUBSONIC_BROWSE_ALL_ALBUMS)
        return subsonic_commit_albums_result();
    subsonic_album_t * fresh = subsonic_browse_result_albums;
    subsonic_browse_result_albums = NULL;
    return subsonic_install_all_albums(fresh, subsonic_browse_result_count, false);
}

/* A finished album or playlist request: show it and note a file was added. */
static bool subsonic_commit_songs_result(void) {
    subsonic_song_t * fresh = subsonic_browse_result_songs;
    subsonic_browse_result_songs = NULL;
    /* The context comes first: the album page reads the album's id. */
    subsonic_set_songs_context(subsonic_browse_result_kind == SUBSONIC_BROWSE_ALBUM_SONGS ? SUBSONIC_LIST_ALBUM_SONGS
                                                                                          : SUBSONIC_LIST_PLAYLIST_SONGS,
                               subsonic_browse_result_id, subsonic_browse_result_title);
    if (!subsonic_install_songs(fresh, subsonic_browse_result_count, false)) {
        subsonic_clear_songs(); /* the old tracks no longer match the title */
        return false;
    }
    subsonic_songs_prune_due = true;
    nav_push(subsonic_songs_screen);
    return true;
}

/* A songs list that is not an album or playlist: nothing saved, nothing to refresh. */
/* Songs that are not a saved list (genre, random, an artist's): never cached. */
bool subsonic_install_transient_songs(subsonic_song_t * items, int count, const char * title) {
    subsonic_set_songs_context(SUBSONIC_LIST_PLAYLIST_SONGS, "~", title);
    subsonic_songs_context_is_playlist = false;
    if (subsonic_install_songs(items, count, false)) return true;
    subsonic_clear_songs();
    return false;
}

bool subsonic_open_transient_songs(subsonic_song_t * items, int count, const char * title) {
    if (!subsonic_install_transient_songs(items, count, title)) return false;
    nav_push(subsonic_songs_screen);
    return true;
}

/* Genres, random songs and favorites: shown at once, never saved or refreshed. */
static bool subsonic_commit_section_result(void) {
    switch (subsonic_browse_result_kind) {
        case SUBSONIC_BROWSE_GENRES: {
            subsonic_genre_t * fresh = subsonic_browse_result_genres;
            subsonic_browse_result_genres = NULL;
            return gui_subsonic_genres_show(fresh, subsonic_browse_result_count);
        }
        case SUBSONIC_BROWSE_RANDOM_SONGS: {
            subsonic_song_t * fresh = subsonic_browse_result_songs;
            subsonic_browse_result_songs = NULL;
            return subsonic_open_transient_songs(fresh, subsonic_browse_result_count, subsonic_browse_result_title);
        }
        default:
            gui_subsonic_favorites_show(&subsonic_browse_result_starred);
            return true;
    }
}

static void subsonic_set_memory_error(void) {
    snprintf(subsonic_browse_result_error, sizeof(subsonic_browse_result_error), "%s",
             TR("Not enough memory to load from server"));
}

/* Installs the finished request's result and shows it. False sets the error text. */
static bool subsonic_commit_artists_result(void) {
    subsonic_artist_t * fresh = subsonic_browse_result_artists;
    subsonic_browse_result_artists = NULL;
    if (!subsonic_install_artists(fresh, subsonic_browse_result_count, false)) {
        snprintf(subsonic_browse_result_error, sizeof(subsonic_browse_result_error),
                 TR("Not enough memory to load artists"));
        return false;
    }
    nav_push(subsonic_artists_screen);
    return true;
}

static bool subsonic_commit_playlists_result(void) {
    subsonic_playlist_t * fresh = subsonic_browse_result_playlists;
    subsonic_browse_result_playlists = NULL;
    if (!subsonic_install_playlists(fresh, subsonic_browse_result_count, false)) return false;
    nav_push(subsonic_playlists_screen);
    return true;
}

static bool subsonic_commit_browse_result(void) {
    bool ok = true;
    switch (subsonic_browse_result_kind) {
        case SUBSONIC_BROWSE_ARTIST_SONGS: {
            subsonic_song_t * songs = subsonic_browse_result_songs;
            subsonic_browse_result_songs = NULL;
            ok = gui_subsonic_artist_accept_songs(songs, subsonic_browse_result_count);
            break;
        }
        case SUBSONIC_BROWSE_ARTISTS:
            return subsonic_commit_artists_result();
        case SUBSONIC_BROWSE_ALBUM_SONGS:
        case SUBSONIC_BROWSE_PLAYLIST_SONGS:
            ok = subsonic_commit_songs_result();
            break;
        case SUBSONIC_BROWSE_ARTIST_ALBUMS:
        case SUBSONIC_BROWSE_ALBUM_LIST:
        case SUBSONIC_BROWSE_ALL_ALBUMS:
            ok = subsonic_commit_albums_for_result();
            break;
        case SUBSONIC_BROWSE_GENRES:
        case SUBSONIC_BROWSE_RANDOM_SONGS:
        case SUBSONIC_BROWSE_STARRED:
            ok = subsonic_commit_section_result();
            break;
        case SUBSONIC_BROWSE_PLAYLISTS:
            ok = subsonic_commit_playlists_result();
            break;
    }
    if (!ok) subsonic_set_memory_error();
    return ok;
}

/* True when the poll has nothing more to do yet: a cancel is hidden, a timeout
 * abandons the request, or the worker is still running. */

static bool subsonic_browse_poll_waiting(void) {
    if (subsonic_browse_abandoned && subsonic_browse_token) {
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
    }
    if (!subsonic_browse_abandoned && !subsonic_browse_paged &&
        lv_tick_elaps(subsonic_browse_started_at) >= SUBSONIC_REQUEST_TIMEOUT_MS) {
        subsonic_browse_abandoned = true;
        http_cancel_token_cancel(&subsonic_browse_cancel);
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
        show_error_toast(TR("Server request timed out after 30 seconds"));
        /* Keep the worker and its buffers alive; a later poll reaps it. */
        return true;
    }
    return !atomic_load_explicit(&subsonic_browse_done_flag, memory_order_acquire);
}

/* Reaps the finished worker. */
static void subsonic_browse_collect(void) {
    /* Left the busy screen some other way: that counts as a cancel. */
    if (lv_screen_active() != gui_busy_get_screen()) subsonic_browse_abandoned = true;
    subsonic_browse_active = false;
    pthread_join(subsonic_browse_thread, NULL);
    http_cancel_token_destroy(&subsonic_browse_cancel);
    /* Finished: leaving the busy screen for the result is not a cancel. */
    gui_busy_set_cancel(subsonic_browse_token, NULL, NULL);
}

/* A failed or cancelled request leaves nothing behind. */
static void subsonic_browse_dispose_results(void) {
    free(subsonic_browse_result_artists);
    subsonic_browse_result_artists = NULL;
    free(subsonic_browse_result_songs);
    subsonic_browse_result_songs = NULL;
    free(subsonic_browse_result_albums);
    subsonic_browse_result_albums = NULL;
    free(subsonic_browse_result_playlists);
    subsonic_browse_result_playlists = NULL;
    free(subsonic_browse_result_genres);
    subsonic_browse_result_genres = NULL;
    gui_subsonic_starred_free(&subsonic_browse_result_starred);
}

/* Takes the busy screen away (or the stack slot it was pushed over) and reports. */
static void subsonic_browse_finish(bool success, int depth_before) {
    /* Cancel or timeout already dismissed busy (and told the user). */
    if (subsonic_browse_abandoned) return;
    subsonic_dismiss_busy(depth_before, subsonic_browse_token);
    subsonic_browse_token = 0;
    if (!success) show_error_toast(subsonic_browse_result_error);
    if (!success && subsonic_browse_result_not_found)
        subsonic_refresh_parent_list(subsonic_browse_result_kind == SUBSONIC_BROWSE_ALBUM_SONGS);
}

void poll_subsonic_browse(void) {
    if (!subsonic_browse_active) return;
    /* A swipe or unlock still animating has already changed the stack: let it
     * land before dismissing the busy screen or navigating to a result. */
    if (gui_navigation_transition_in_progress()) return;
    if (subsonic_browse_poll_waiting()) return;
    subsonic_browse_collect();
    bool success = subsonic_browse_success_flag && !subsonic_browse_abandoned;
    int depth_before = gui_navigation_get_depth();
    if (success) {
        success = subsonic_commit_browse_result();
        if (success && subsonic_browse_result_list >= 0)
            subsonic_entries[subsonic_browse_result_list] = subsonic_browse_result_entry;
    } else {
        subsonic_browse_dispose_results();
    }
    subsonic_browse_finish(success, depth_before);
}

/* The server lists behind the menu rows. The request title doubles as the genre for "byGenre". */
void gui_subsonic_browse_album_list(const char * type, const char * title) {
    start_subsonic_browse(SUBSONIC_BROWSE_ALBUM_LIST, type, title);
}

void gui_subsonic_browse_genres(void) { start_subsonic_browse(SUBSONIC_BROWSE_GENRES, NULL, TR("Genres")); }
void gui_subsonic_browse_random_songs(void) {
    start_subsonic_browse(SUBSONIC_BROWSE_RANDOM_SONGS, NULL, TR("Random Songs"));
}

void gui_subsonic_browse_starred(void) { start_subsonic_browse(SUBSONIC_BROWSE_STARRED, NULL, TR("Favorites")); }

void gui_subsonic_browse_cancel(void) {
    if (subsonic_browse_active) {
        http_cancel_token_cancel(&subsonic_browse_cancel);
        subsonic_browse_abandoned = true; /* a result already parsed is dropped too */
    }
}

void gui_subsonic_browse_join(void) {
    if (subsonic_browse_active) {
        pthread_join(subsonic_browse_thread, NULL);
        subsonic_browse_active = false;
        http_cancel_token_destroy(&subsonic_browse_cancel);
        subsonic_browse_dispose_results();
        gui_busy_hide(subsonic_browse_token);
        subsonic_browse_token = 0;
    }
}
