/* Favorites, Most Played, saved playlists, CUE sheets and remote-control collection playback. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "../library_thumbnails.h"
#include "../../../network/remote/control/remote_control.h"
#include "../../application/gui.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../core/system/logging/db_log.h"
#include <limits.h>
#include <stdatomic.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

/* ---- Playlists (Music submenu) --
 *
 * Favorites and Most Played (top 20 by play count, see metadata_db's
 * song_play_count table) are resolved fresh from the DB on every tap and
 * shown through show_group_songs() -- the exact same drill-down screen
 * Artists/Albums/Album Artist already use -- rather than a screen of their
 * own. Membership can change between visits (unlike Artists/Albums/Album
 * Artist, fixed at scan time), so these two are recomputed fresh every
 * time their row is tapped rather than cached. show_group_songs() takes an
 * owned copy of the small result set, so the DB-returned path array can be
 * released immediately.
 *
 * User-created .m3u playlists (Player screen's "Add to Playlist", or
 * dropped into MUSIC_ROOT_DIR/Playlists by hand) are listed below those
 * from that folder only. ---- */

/* Resolves each of paths[0..count) to a display title via a single-row DB
 * lookup (metadata_db_get_song_by_path()) -- falls back to the raw
 * basename if a path somehow isn't in the library anymore (a stale
 * favorite/playlist entry that outlived a rescan). Caller owns the
 * returned array (free() it); paths[] itself is untouched. Shared by
 * show_favorites()/show_most_played()/show_m3u_playlist() below -- these
 * lists are small (a hand-curated favorites list, MOST_PLAYED_LIMIT, one
 * playlist), so a DB lookup per song costs nothing next to loading the
 * whole library just to resolve a title. */
group_song_entry_t * build_group_song_entries_from_paths(char ** paths, int count) {
    group_song_entry_t * entries = calloc((size_t) (count > 0 ? count : 1), sizeof(*entries));
    if (!entries) return NULL;
    for (int i = 0; i < count; i++) {
        entries[i].path = strdup(paths[i]);
        song_row_t row = {0};
        snprintf(row.path, sizeof(row.path), "%s", paths[i]);
        char title[384];
        if (metadata_db_get_song_by_path(paths[i], &row)) {
            format_song_identity(&row, title, sizeof(title));
        } else {
            snprintf(title, sizeof(title), "%s", basename_of(paths[i]));
        }
        entries[i].title = strdup(title);
        entries[i].song_id = row.id;
        entries[i].artwork_key = song_album_thumbnail_key(&row);
        if (!entries[i].path || !entries[i].title) {
            free_group_song_entries(entries, count);
            return NULL;
        }
    }
    return entries;
}

typedef struct {
    remote_collection_play_request_t request;
    pthread_t thread;
    atomic_bool cancel;
    atomic_bool done;
    bool ok;
    bool lazy;
    char error[96];
    metadata_db_snapshot_t *snapshot;
    int *title_ranks;
    char **paths;
    int count;
    uint64_t playback_epoch;
    int initial_play_mode;
    lv_timer_t *timer;
} remote_collection_job_t;

static remote_collection_job_t *remote_collection_job;

static bool remote_collection_cancelled(void *userdata) {
    remote_collection_job_t *job = userdata;
    return !job || atomic_load_explicit(&job->cancel, memory_order_acquire);
}

static void remote_collection_free_paths(char **paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static bool remote_collection_load_paths(remote_collection_job_t *job) {
    const remote_collection_play_request_t *request = &job->request;
    switch (request->scope) {
        case REMOTE_COLLECTION_FAVORITES:
            metadata_db_load_favorite_songs(&job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_RECENTLY_ADDED:
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_RECENTLY_PLAYED:
            metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX,
                                                   &job->paths, &job->count);
            break;
        case REMOTE_COLLECTION_PLAYLIST: {
            if (!request->name[0] || strchr(request->name, '/') || strstr(request->name, "..")) {
                snprintf(job->error, sizeof(job->error), "%s", "Playlist could not be loaded");
                return false;
            }
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s.m3u", PLAYLISTS_DIR, request->name);
            if (n < 0 || (size_t)n >= sizeof(path) ||
                !file_browser_build_playlist_from_m3u(path, &job->paths, &job->count))
                return false;
            break;
        }
        default:
            return false;
    }
    if (remote_collection_cancelled(job)) return false;
    if (job->count <= 0 || !job->paths) {
        snprintf(job->error, sizeof(job->error), "%s", "Collection is empty or unavailable");
        return false;
    }
    return true;
}

static void *remote_collection_worker(void *userdata) {
    remote_collection_job_t *job = userdata;
    const remote_collection_play_request_t *request = &job->request;
    const char *artist = NULL, *album_artist = NULL, *album = NULL, *genre = NULL;
    switch (request->scope) {
        case REMOTE_COLLECTION_ALL:
            break;
        case REMOTE_COLLECTION_ALBUM:
            album = request->album[0] ? request->album : request->name;
            if (request->artist[0]) artist = request->artist;
            else album_artist = request->album_artist;
            if (!album[0]) {
                snprintf(job->error, sizeof(job->error), "%s", "Album identity is incomplete");
                goto done;
            }
            break;
        case REMOTE_COLLECTION_ARTIST:
            artist = request->artist[0] ? request->artist : request->name;
            break;
        case REMOTE_COLLECTION_ALBUM_ARTIST:
            album_artist = request->album_artist[0] ? request->album_artist : request->name;
            break;
        case REMOTE_COLLECTION_GENRE:
            genre = request->genre[0] ? request->genre : request->name;
            break;
        case REMOTE_COLLECTION_PLAYLIST:
        case REMOTE_COLLECTION_FAVORITES:
        case REMOTE_COLLECTION_RECENTLY_ADDED:
        case REMOTE_COLLECTION_RECENTLY_PLAYED:
            job->ok = remote_collection_load_paths(job);
            goto done;
        default:
            snprintf(job->error, sizeof(job->error), "%s", "Unsupported collection scope");
            goto done;
    }

    metadata_db_catalog_result_t result = metadata_db_filtered_snapshot_open(
        request->expected_revision, artist, album_artist,
        request->scope == REMOTE_COLLECTION_ALBUM && !artist, album, genre,
        &job->snapshot, &job->title_ranks, &job->count,
        remote_collection_cancelled, job);
    if (result == METADATA_DB_CATALOG_STALE)
        snprintf(job->error, sizeof(job->error), "%s", "Catalog changed before playback");
    else if (remote_collection_cancelled(job))
        snprintf(job->error, sizeof(job->error), "%s", "Playback request was cancelled");
    else if (result != METADATA_DB_CATALOG_OK)
        snprintf(job->error, sizeof(job->error), "%s", "Unable to prepare collection playback");
    else if (job->count <= 0)
        snprintf(job->error, sizeof(job->error), "%s", "Collection is empty or unavailable");
    else
        job->ok = true;
done:
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static void remote_collection_job_destroy(remote_collection_job_t *job) {
    if (!job) return;
    if (job->snapshot) metadata_db_snapshot_close(job->snapshot);
    free(job->title_ranks);
    remote_collection_free_paths(job->paths, job->count);
    free(job);
}

void remote_collection_cancel_and_finish(void) {
    remote_collection_job_t *job = remote_collection_job;
    if (!job) return;
    atomic_store_explicit(&job->cancel, true, memory_order_release);
}

static void remote_collection_poll(lv_timer_t *timer) {
    remote_collection_job_t *job = remote_collection_job;
    if (!job || job->timer != timer) return;
    if (gui_player_playback_intent_epoch() != job->playback_epoch ||
        current_settings.play_mode != job->initial_play_mode)
        atomic_store_explicit(&job->cancel, true, memory_order_release);
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;

    bool context_current = !atomic_load_explicit(&job->cancel, memory_order_acquire) &&
        gui_player_playback_intent_epoch() == job->playback_epoch &&
        current_settings.play_mode == job->initial_play_mode;
    if (context_current && job->ok) {
        metadata_db_catalog_result_t check = metadata_db_try_catalog_revision_check(
            job->request.expected_revision);
        if (check == METADATA_DB_CATALOG_BUSY) return;
        context_current = check == METADATA_DB_CATALOG_OK;
        if (!context_current && check == METADATA_DB_CATALOG_STALE)
            snprintf(job->error, sizeof(job->error), "%s", "Catalog changed before playback");
        else if (!context_current)
            snprintf(job->error, sizeof(job->error), "%s", "Catalog is unavailable");
    }
    lv_timer_delete(timer);
    job->timer = NULL;
    pthread_join(job->thread, NULL);
    bool ok = job->ok && context_current;
    if (ok) {
        if (job->lazy) {
            ok = gui_player_play_lazy_collection(job->snapshot, job->title_ranks,
                                                  job->count, job->request.play_mode);
            if (ok) { job->snapshot = NULL; job->title_ranks = NULL; }
        } else {
            ok = gui_player_play_collection_paths(job->paths, job->count,
                                                  job->request.play_mode);
            if (ok) job->paths = NULL;
        }
    }
    if (!ok && !job->error[0]) {
        snprintf(job->error, sizeof(job->error), "%s",
                 context_current ? "Unable to start collection playback"
                                 : "Playback request was cancelled");
    }
    remote_collection_job = NULL;
    remote_control_finish_collection(ok, ok ? "" : job->error);
    remote_collection_job_destroy(job);
}

bool gui_library_play_remote_collection(const remote_collection_play_request_t *request) {
    bool catalog_scope = request && request->scope <= REMOTE_COLLECTION_GENRE;
    if (!request || remote_collection_job || request->play_mode < PLAY_MODE_SEQUENTIAL ||
        request->play_mode > PLAY_MODE_SHUFFLE_ONCE ||
        (catalog_scope && !request->expected_revision[0]) ||
        !memchr(request->expected_revision, '\0', sizeof(request->expected_revision)) ||
        !memchr(request->name, '\0', sizeof(request->name)) ||
        !memchr(request->artist, '\0', sizeof(request->artist)) ||
        !memchr(request->album_artist, '\0', sizeof(request->album_artist)) ||
        !memchr(request->album, '\0', sizeof(request->album)) ||
        !memchr(request->genre, '\0', sizeof(request->genre))) return false;
    remote_collection_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->request = *request;
    job->lazy = request->scope <= REMOTE_COLLECTION_GENRE;
    job->playback_epoch = gui_player_playback_intent_epoch();
    job->initial_play_mode = current_settings.play_mode;
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);
    remote_collection_job = job;
    job->timer = lv_timer_create(remote_collection_poll, 50, NULL);
    if (!job->timer) {
        remote_collection_job = NULL;
        remote_collection_job_destroy(job);
        return false;
    }
    if (pthread_create(&job->thread, NULL, remote_collection_worker, job) != 0) {
        lv_timer_delete(job->timer);
        job->timer = NULL;
        remote_collection_job = NULL;
        remote_collection_job_destroy(job);
        return false;
    }
    return true;
}

void gui_library_cancel_remote_collection(void) {
    if (remote_collection_job)
        atomic_store_explicit(&remote_collection_job->cancel, true, memory_order_release);
}

bool remote_collection_busy(void) {
    return remote_collection_job != NULL;
}

/* Plays a song selected via the remote-control web UI within its intended
 * scope (album, playlist, or artist). Falls back to full-library queue if
 * no scope is provided or resolved. */
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter) {
    if (!song_path || !song_path[0]) return;
    if (!gui_player_local_playback_allowed()) return;
    group_song_entry_t * scoped_entries = NULL;
    int scoped_count = 0;
    char scoped_title[128] = "";

    if (playlist_name[0] != '\0') {
        char ** paths = NULL;
        int count = 0;
        bool loaded = false;
        if (strcmp(playlist_name, "@favorites") == 0 || strcmp(playlist_name, "Favorites") == 0) {
            metadata_db_load_favorite_songs(&paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Favorites"));
        } else if (strcmp(playlist_name, "@most_played") == 0 || strcmp(playlist_name, "Most Played") == 0) {
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Most Played"));
        } else if (strcmp(playlist_name, "@recently_added") == 0 || strcmp(playlist_name, "Recently Added") == 0) {
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Recently Added"));
        } else if (strcmp(playlist_name, "@recently_played") == 0) {
            metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "%s", TR("Recently Played"));
        } else {
            char m3u_path[512];
            snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, playlist_name);
            loaded = file_browser_build_playlist_from_m3u(m3u_path, &paths, &count);
        }
        if (loaded && count > 0) {
            scoped_entries = build_group_song_entries_from_paths(paths, count);
            scoped_count = scoped_entries ? count : 0;
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            if (scoped_title[0] == '\0') snprintf(scoped_title, sizeof(scoped_title), "%s", playlist_name);
        }
    } else if (album_filter[0] != '\0' && (artist_filter[0] != '\0' || album_artist_filter[0] != '\0')) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist_filter, album_filter, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            scoped_entries = calloc((size_t) count64, sizeof(*scoped_entries));
            song_row_t rows[64];
            while (scoped_entries && scoped_count < count64) {
                int want = (int) (count64 - scoped_count);
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist_filter, album_filter, NULL,
                                                               scoped_count, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    scoped_entries[scoped_count + i].path = strdup(rows[i].path);
                    scoped_entries[scoped_count + i].title = strdup(title);
                    scoped_entries[scoped_count + i].song_id = rows[i].id;
                    scoped_entries[scoped_count + i].artwork_key = song_album_thumbnail_key(&rows[i]);
                    if (!scoped_entries[scoped_count + i].path || !scoped_entries[scoped_count + i].title) {
                        free_group_song_entries(scoped_entries, (int) count64);
                        scoped_entries = NULL;
                        scoped_count = 0;
                        break;
                    }
                }
                if (!scoped_entries) break;
                scoped_count += got;
                if (got < want) break;
            }
            snprintf(scoped_title, sizeof(scoped_title), "%s", album_filter);
        }
    }

    if (scoped_entries && scoped_count > 0) {
        int pos = -1;
        for (int i = 0; i < scoped_count; i++) {
            if (strcmp(scoped_entries[i].path, song_path) == 0) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            char ** playlist_copy = malloc(sizeof(char *) * (size_t) scoped_count);
            for (int i = 0; i < scoped_count; i++) playlist_copy[i] = strdup(scoped_entries[i].path);
            set_player_source_group_songs_direct(scoped_entries, scoped_count, scoped_title, pos);
            on_file_selected(playlist_copy, scoped_count, pos);
            free_group_song_entries(scoped_entries, scoped_count);
            return;
        }
    }
    free_group_song_entries(scoped_entries, scoped_count);

    /* A caller that supplied a collection context asked for that exact
     * source. If it no longer resolves (for example after a rescan), do not
     * surprise them by replacing it with the entire library. */
    if ((playlist_name && playlist_name[0]) ||
        (artist_filter && artist_filter[0]) ||
        (album_artist_filter && album_artist_filter[0]) ||
        (album_filter && album_filter[0])) return;

    int64_t offset = metadata_db_get_song_title_offset(song_path);
    if (offset >= 0 && offset <= INT_MAX) {
        set_player_source_all_songs((int) offset);
        on_file_selected_lazy_all_songs((int) offset);
    }
}

static void show_favorites(void) {
    char ** paths;
    int count;
    metadata_db_load_favorite_songs(&paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs(TR("Favorites"), entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void show_most_played(void) {
    char ** paths;
    int count;
    metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs(TR("Most Played"), entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static char playlist_m3u_name[128];

/* The currently open M3U's title survives an in-place row removal. Song
 * paths and titles themselves live in group_songs_entries, independent of
 * the full-library arrays. */
/* Shows an .m3u's contents the same way Favorites/Most Played do -- a
 * tappable song list (show_group_songs_editable()), not straight into
 * playback -- rather than the old behavior of jumping directly to track 0
 * the moment the playlist row itself was tapped, matching every other entry
 * point into a group of songs elsewhere in this screen (Artists/Albums/
 * Album Artist all list first too). m3u_path is passed through as the
 * editable-playlist marker so the Edit UI (group_songs_edit_m3u_path,
 * see its own comment) only ever shows up here, not for Favorites/Most
 * Played/Artists/Albums. */
static void show_m3u_playlist(const char * name, const char * m3u_path, char ** paths, int count) {
    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (count && !entries) return;
    snprintf(playlist_m3u_name, sizeof(playlist_m3u_name), "%s", name);
    group_songs_refresh_source = GROUP_SONGS_REFRESH_M3U;
    snprintf(group_songs_refresh_m3u_path, sizeof(group_songs_refresh_m3u_path), "%s", m3u_path ? m3u_path : "");
    show_group_songs_editable(playlist_m3u_name, entries, count, m3u_path, false);
    free_group_song_entries(entries, count);
}

/* A missing Playlists folder is recreated so the user always has one to use;
 * a read-only card simply fails the mkdir. Callers check the card is mounted
 * so an empty mount point never gets one. */
void ensure_playlists_dir(void) {
    if (mkdir(PLAYLISTS_DIR, 0755) != 0 && errno != EEXIST)
        DB_LOG("DB", "playlists_mkdir_failed errno=%d", errno);
}

void library_playlist_open_row(gui_playlist_row_kind_t kind, const char * name,
                                      const char * path, char ** songs, int count) {
    switch (kind) {
        case GUI_PLAYLIST_ROW_FAVORITES: show_favorites(); break;
        case GUI_PLAYLIST_ROW_MOST_PLAYED: show_most_played(); break;
        case GUI_PLAYLIST_ROW_QUEUE: open_queue_screen(); break;
        case GUI_PLAYLIST_ROW_RECENTLY_ADDED: open_recently_added_screen(); break;
        case GUI_PLAYLIST_ROW_SAVED_M3U: show_m3u_playlist(name, path, songs, count); break;
    }
}

/* ---- CUE sheet track list (File Browser -> tap a .cue) ----
 * External and embedded sheets produce stable virtual queue entries.
 * Each entry keeps its own metadata and bounded playback timeline. */
static cue_sheet_t current_cue_sheet;
static bool current_cue_sheet_valid = false;
static char current_cue_source_path[PATH_MAX];
static char current_cue_source_dir[PATH_MAX];
static int current_cue_source_row = -1;

static void cue_track_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (!current_cue_sheet_valid || index < 0 || index >= current_cue_sheet.track_count) return;

    int count = current_cue_sheet.track_count;
    char ** paths = cue_sheet_references(current_cue_source_path, count);
    if (!paths) {
        show_error_toast(TR("Not enough memory to load CUE tracks"));
        return;
    }
    set_player_source_file_browser(current_cue_source_dir, current_cue_source_row);
    on_file_selected(paths, count, index);
}

static void populate_cue_tracks_screen(void) {
    lv_obj_clean(cue_tracks_list);
    for (int i = 0; i < current_cue_sheet.track_count; i++) {
        cue_track_t * t = &current_cue_sheet.tracks[i];
        char label[160];
        /* Falls back to the plain track number when a sheet doesn't set
         * TITLE for a track (rare but real -- some auto-generated sheets
         * only carry INDEX times) rather than showing an empty row. */
        if (t->title[0]) {
            snprintf(label, sizeof(label), "%d. %s", t->number, t->title);
        } else {
            snprintf(label, sizeof(label), TR("Track %d"), t->number);
        }
        lv_obj_t * row = build_wide_list_row(cue_tracks_list, label, false);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cue_track_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

lv_obj_t * build_cue_tracks_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen(TR("Tracks"), &cue_tracks_title_label, &cue_tracks_list);
    lv_obj_set_flex_align(cue_tracks_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return scr;
}

/* file_browser.h's on_cue_select callback -- see file_browser_create()'s own
 * comment. Parses fresh on every tap (a .cue sheet is tiny, no reason to
 * cache) and replaces whatever sheet this screen was last showing. */
void on_cue_file_selected(const char * cue_path) {
    cue_sheet_free(&current_cue_sheet);
    const char * ext = strrchr(cue_path, '.');
    current_cue_sheet_valid = ext && strcasecmp(ext, ".cue") == 0
        ? cue_parse_file(cue_path, &current_cue_sheet)
        : cue_parse_embedded(cue_path, &current_cue_sheet);
    if (!current_cue_sheet_valid) {
        show_error_toast(TR("Couldn't read this .cue file"));
        return;
    }
    snprintf(current_cue_source_path, sizeof(current_cue_source_path), "%s", cue_path);
    snprintf(current_cue_source_dir, sizeof(current_cue_source_dir), "%s", file_browser_get_last_selected_dir());
    current_cue_source_row = file_browser_get_last_selected_row();
    lv_label_set_text(cue_tracks_title_label, current_cue_sheet.title[0]
                      ? current_cue_sheet.title : basename_of(current_cue_sheet.audio_path));
    populate_cue_tracks_screen();
    nav_push(cue_tracks_screen);
}
