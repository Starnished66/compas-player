/* The Music lists: All Songs, Recently Added, Artists, Albums, Album Artist and Genres, with the album and genre loaders. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_album_header.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "gui_artist_data.h"
#include "../../../core/process/worker_thread.h"
#include "../library_selection_snapshot.h"
#include "../library_thumbnails.h"
#include "../../assets/assets.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../core/ui/display/ui_wake.h"
#include "gui_library_async.h"
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cancel_genre_load(void);

uint64_t album_load_generation;

/* Row fill shared by All Songs and Recently Added; offset is the page's first
 * rank in the source's order, and rows are cached for selection only when the
 * DB generation held still across the page query. */
static void fill_song_page_rows(compact_list_page_row_t out_rows[], const song_row_t * rows, int n,
                                int offset, selection_snapshot_source_t source,
                                bool cacheable, int32_t generation) {
    for (int i = 0; i < n; i++) {
        fill_song_page_visual(&out_rows[i], &rows[i]);
        if (cacheable)
            library_selection_snapshot_store(&rows[i], source, offset + i, generation);
    }
}

int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    if (count <= 0) return 0;
    int32_t generation_before = 0, generation_after = 0;
    bool cacheable = metadata_db_try_get_generation(&generation_before);
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, offset, count, rows) : 0;
    if (cacheable)
        cacheable = metadata_db_try_get_generation(&generation_after) && generation_before == generation_after;
    fill_song_page_rows(out_rows, rows, n, offset, SELECTION_SNAPSHOT_ALL_SONGS,
                        cacheable, generation_before);
    free(rows);
    return n;
}

/* A tapped All Songs row as a position in the title order. */
static int all_songs_song_index(int display_index) {
    if (library_search_showing_results(SEARCH_BINDING_ALL_SONGS))
        return search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    return display_index;
}

/* Resolves one All-Songs display position to a real path -- for the
 * long-press context menu, which needs a path directly (unlike the click
 * handler, which goes through play_track_at_from() -> playlist_path_at()'s
 * own resolution instead). false if display_index is out of range (e.g.
 * raced a rescan shrinking the library between the tap and this lookup --
 * same stale-reference tolerance as playlist_path_at()'s own comment). */
static bool all_songs_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void all_songs_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = all_songs_song_index(display_index);
    if (display_index < 0) return;
    /* on_file_selected_lazy_all_songs() builds the queue lazily, identity-
     * mapped into the DB's own title-sorted order (the same display order
     * this screen's paged provider uses) instead of eagerly strdup'ing
     * every one of the library's paths on every tap -- see its own
     * comment. Tap-to-play from All Songs is DB-driven end to end now. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, display_index, 0, NULL);
    set_player_source_all_songs(display_index);
    on_file_selected_lazy_all_songs(display_index);
}

static void all_songs_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = all_songs_song_index(display_index);
    if (display_index < 0) return;
    char path[600];
    if (all_songs_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

songs_bar_t all_songs_bar, recent_bar;

/* Builds the "All Songs" screen with a paged provider
 * (compact_list_set_paged_provider()) so items are loaded incrementally
 * on scroll rather than materializing the full library in memory. */
lv_obj_t * build_all_songs_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("All Songs"), generic_back_cb, NULL, 0, all_songs_row_click_cb,
                                                all_songs_row_long_press_cb, &all_songs_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(all_songs_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(all_songs_list, all_songs_fetch_page, NULL,
                                     (int) metadata_db_get_song_count());
    library_thumbnails_bind_list(scr, all_songs_list, LIBRARY_THUMB_LIST_ALL_SONGS);
    songs_play_bar_refresh(all_songs_list, &all_songs_bar, PLAY_ALL_SOURCE_ALL_SONGS);
    finalize_screen_navigation(scr);
    return scr;
}

/* Recently Added -- same paged compact_list architecture as All Songs above
 * (see build_all_songs_screen()'s own comment on why: a bounded page at a
 * time regardless of library size, not the show_group_songs()/group_song_
 * entry_t path Favorites/Most Played use, which loads its whole, small,
 * deliberately-capped result set into RAM up front -- Recently Added has no
 * such cap, it's every song in the library just reordered by first_seen, so
 * it needs the same scale guarantee All Songs has). Only difference from
 * All Songs is the ORDER BY (first_seen DESC, rowid DESC via metadata_db_
 * get_songs_page_by_recency() instead of title via metadata_db_get_songs_
 * filtered_page()) -- no search/A-Z index wired up here, unlike All Songs,
 * since neither makes as much sense against a recency-ordered list. */
void format_song_identity(const song_row_t * row, char * out, size_t size) {
    char title[128], subtitle[256];
    gui_library_format_song_identity(row, title, sizeof(title), subtitle, sizeof(subtitle));
    snprintf(out, size, "%s\n%s", title, subtitle);
}

int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    if (count <= 0) return 0;
    int32_t generation_before = 0, generation_after = 0;
    bool cacheable = metadata_db_try_get_generation(&generation_before);
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_page_by_recency(offset, count, rows) : 0;
    if (cacheable)
        cacheable = metadata_db_try_get_generation(&generation_after) && generation_before == generation_after;
    fill_song_page_rows(out_rows, rows, n, offset, SELECTION_SNAPSHOT_RECENTLY_ADDED,
                        cacheable, generation_before);
    free(rows);
    return n;
}

/* Same role as all_songs_resolve_path_at() above, against the recency order. */
static bool recently_added_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_page_by_recency(display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

/* A tapped Recently Added row as a position in the recency order. */
static int recently_added_song_index(int display_index) {
    if (library_search_showing_results(SEARCH_BINDING_RECENTLY_ADDED))
        return search_remap_index(SEARCH_BINDING_RECENTLY_ADDED, display_index);
    return display_index;
}

static void recently_added_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = recently_added_song_index(display_index);
    if (display_index < 0) return;
    /* on_file_selected_lazy_recently_added() builds the queue lazily,
     * identity-mapped into the DB's own first_seen-DESC order (the same
     * display order this screen's paged provider uses) -- see its own
     * comment. Tap-to-play here is DB-driven end to end, same as All Songs. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_RECENTLY_ADDED, display_index, 0, NULL);
    set_player_source_recently_added(display_index);
    on_file_selected_lazy_recently_added(display_index);
}

static void recently_added_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = recently_added_song_index(display_index);
    if (display_index < 0) return;
    char path[600];
    if (recently_added_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

lv_obj_t * build_recently_added_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Recently Added"), generic_back_cb, NULL, 0, recently_added_row_click_cb,
                                                recently_added_row_long_press_cb, &recently_added_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(recently_added_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
                                     (int) metadata_db_get_song_count());
    library_thumbnails_bind_list(scr, recently_added_list, LIBRARY_THUMB_LIST_RECENTLY_ADDED);
    songs_play_bar_refresh(recently_added_list, &recent_bar, PLAY_ALL_SOURCE_RECENTLY_ADDED);
    finalize_screen_navigation(scr);
    return scr;
}

void all_songs_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(all_songs_screen);
}

/* All Songs / Recently Added: the same two starts over the whole library. */
static void songs_bar_start(lv_event_t * e, int source, bool shuffle) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || library_rescan_active) return;
    gui_player_seed_selection_song(NULL, NULL);
    playlist_start_source = source;
    playlist_start_selected(shuffle);
}
static void all_songs_play_cb(lv_event_t * e) { songs_bar_start(e, PLAY_ALL_SOURCE_ALL_SONGS, false); }
static void all_songs_shuffle_cb(lv_event_t * e) { songs_bar_start(e, PLAY_ALL_SOURCE_ALL_SONGS, true); }
static void recent_play_cb(lv_event_t * e) { songs_bar_start(e, PLAY_ALL_SOURCE_RECENTLY_ADDED, false); }
static void recent_shuffle_cb(lv_event_t * e) { songs_bar_start(e, PLAY_ALL_SOURCE_RECENTLY_ADDED, true); }

/* (Re)builds the bar when the library's track count changed; no bar for an empty library. */
void songs_play_bar_refresh(lv_obj_t * list, songs_bar_t * bar, int source) {
    if (!list) return;
    int64_t tracks = metadata_db_get_song_count();
    if (bar->obj && bar->tracks == tracks) return;
    if (bar->obj) {
        compact_list_set_paged_header(list, NULL, 0);
        lv_obj_delete(bar->obj);
        bar->obj = NULL;
    }
    bar->tracks = tracks;
    if (tracks <= 0) return;
    char count_text[48];
    snprintf(count_text, sizeof(count_text), TR_N("%lld track", "%lld tracks", (long) tracks), (long long) tracks);
    bool all = source == PLAY_ALL_SOURCE_ALL_SONGS;
    bar->obj = gui_play_bar_build(list, LIST_ROW_WIDTH_WIDE, count_text, all ? all_songs_play_cb : recent_play_cb,
                                  all ? all_songs_shuffle_cb : recent_shuffle_cb);
    lv_obj_update_layout(bar->obj);
    compact_list_set_paged_header(list, bar->obj, lv_obj_get_height(bar->obj));
}

/* Accent change: the long-lived All Songs / Recently Added bars recolour their Play label. */
void gui_library_accent_changed(void) {
    if (all_songs_bar.obj) gui_play_bar_restyle(all_songs_bar.obj);
    if (recent_bar.obj) gui_play_bar_restyle(recent_bar.obj);
}

/* Cover key for an album row: by (album artist, album) when the album artist
 * is known, else from the representative song's own tags. */
void fill_album_cover_key(compact_list_page_row_t * out, const group_row_t * row) {
    if (row->album_artist[0]) {
        albumart_info_t info = {0};
        snprintf(info.albumartist, sizeof(info.albumartist), "%s", row->album_artist);
        snprintf(info.album, sizeof(info.album), "%s", row->name);
        out->artwork_key = albumart_thumbnail_key(&info);
    } else {
        song_row_t representative;
        if (metadata_db_get_song_by_id(row->first_song_id, &representative))
            out->artwork_key = song_album_thumbnail_key(&representative);
    }
}

/* Unfiltered (NULL filter, "every album in the library") -- uses metadata_
 * db_get_albums_page_filtered() rather than metadata_db_get_groups_page(
 * METADATA_DB_GROUP_ALBUM, ...), since the latter groups by album name
 * alone (the exact bug this session fixed) and this screen must show the
 * corrected (album, album_artist) grouping. */
int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_albums_page_sorted(NULL, effective_album_sort(), offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        snprintf(out_rows[i].subtitle, sizeof(out_rows[i].subtitle), "%s", rows[i].album_artist);
        out_rows[i].identity = rows[i].first_song_id;
        fill_album_cover_key(&out_rows[i], &rows[i]);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n;
}

#define PRESS_RESOLVED_TTL_MS 3000
static struct {
    bool valid;
    int source, index;
    group_row_t group;
    uint32_t tick;
} press_resolved;

void press_resolved_set(int source, int index, const group_row_t * group) {
    press_resolved.valid = true;
    press_resolved.source = source;
    press_resolved.index = index;
    press_resolved.group = *group;
    press_resolved.tick = lv_tick_get();
}

void press_resolved_clear(void) { press_resolved.valid = false; }

/* True and `out` filled when the press of this very row resolved it a moment ago. */
bool press_resolved_take(int source, int index, group_row_t * out) {
    bool ok = press_resolved.valid && press_resolved.source == source && press_resolved.index == index &&
              lv_tick_elaps(press_resolved.tick) <= PRESS_RESOLVED_TTL_MS && !library_rescan_active;
    press_resolved.valid = false;
    if (ok) *out = press_resolved.group;
    return ok;
}

static void artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    group_row_t group;
    if (!press_resolved_take(PRESS_SRC_ARTISTS, index, &group)) {
        int resolved = search_remap_index(SEARCH_BINDING_ARTISTS, index);
        if (metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, resolved, 1, &group) != 1) return;
    }
    artist_open_song_hint = group.song_count;
    show_artist_albums(group.name, METADATA_DB_GROUP_ARTIST);
}

/* A touch on an artist row starts the aggregate the artist page needs. The page
 * asks for the same artist, sort and generation, which keeps this job; a touch
 * that does not become a tap cancels it (nothing is shown from it meanwhile). */
static void artist_prefetch_press(int index, bool pressed, int binding, metadata_db_group_kind_t kind,
                                  int press_source) {
    press_resolved_clear();
    if (!pressed) { gui_artist_data_close(); return; }
    if (library_rescan_active) return;
    group_row_t group;
    if (metadata_db_get_groups_page(kind, search_remap_index(binding, index), 1, &group) != 1) return;
    press_resolved_set(press_source, index, &group);
    gui_artist_data_open(kind, group.name, effective_group_album_sort(kind));
}

static void artist_row_press_cb(int index, bool pressed) {
    artist_prefetch_press(index, pressed, SEARCH_BINDING_ARTISTS, METADATA_DB_GROUP_ARTIST, PRESS_SRC_ARTISTS);
}

/* artist_filter scopes an album to one credited artist, for the Artists
 * drill-down where a track tagged "A;B" puts the album under both. The album
 * group's own song_count covers the whole album, so a filtered listing has to
 * count its own rows or it would promise tracks it will not show. Album
 * Artists and the plain Albums list pass NULL and keep the whole album. */
group_song_entry_t * load_album_entries_filtered(const char * name, const char * album_artist,
                                                         int song_count, const char * artist_filter,
                                                         int * out_count, const atomic_bool * cancel) {
    *out_count = 0;
    int32_t generation_before = 0;
    bool snapshot_generation_valid = metadata_db_try_get_generation(&generation_before);
    if (artist_filter && artist_filter[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist, name, NULL);
        song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    }
    if (song_count <= 0) return NULL;
    song_row_t * songs = calloc((size_t) song_count, sizeof(*songs));
    group_song_entry_t * entries = calloc((size_t) song_count, sizeof(*entries));
    if (!songs || !entries) { free(songs); free(entries); return NULL; }
    int n = 0;
    while (n < song_count) {
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) break;
        int want = song_count - n;
        if (want > 64) want = 64;
        int got = (artist_filter && artist_filter[0])
                      ? metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist, name, NULL,
                                                            n, want, songs + n)
                      : metadata_db_get_album_songs(name, album_artist, n, songs + n, want);
        if (got <= 0) break;
        n += got;
        if (got < want) break;
    }
    if (n <= 0 || (cancel && atomic_load_explicit(cancel, memory_order_relaxed))) {
        free(songs);
        free_group_song_entries(entries, song_count);
        return NULL;
    }
    int32_t generation_after = 0;
    if (!snapshot_generation_valid || !metadata_db_try_get_generation(&generation_after) ||
        generation_before != generation_after)
        snapshot_generation_valid = false;
    qsort(songs, (size_t)n, sizeof(*songs), cmp_song_disc_track);
    if (snapshot_generation_valid) {
        int snapshot_count = n < GROUP_SONGS_PAGE_SIZE ? n : GROUP_SONGS_PAGE_SIZE;
        for (int i = 0; i < snapshot_count; i++)
            library_selection_snapshot_store(&songs[i], SELECTION_SNAPSHOT_GROUP, -1, generation_before);
    }
    bool multi_disc = album_songs_are_multi_disc(songs, n);
    for (int i = 0; i < n; i++) {
        char title[384];
        format_music_submenu_identity(&songs[i], title, sizeof(title));
        entries[i].path = strdup(songs[i].path);
        entries[i].title = strdup(title);
        entries[i].song_id = songs[i].id;
        entries[i].artwork_key = song_album_thumbnail_key(&songs[i]);
        if (!entries[i].path || !entries[i].title) {
            free(songs);
            free_group_song_entries(entries, song_count);
            return NULL;
        }
    }
    set_album_disc_headers(songs, n, multi_disc, entries);
    gui_album_header_record(songs, n);
    free(songs);
    *out_count = n;
    return entries;
}

/* The artist whose album list is on screen, when that list is a plain artist
 * rather than an album artist. NULL elsewhere, so only the Artists path
 * scopes an album to one contributor. */

#define ALBUM_PREFETCH_TTL_MS 3000
album_load_request_t * album_load_job, * album_load_pending;
static pthread_t album_load_thread;
lv_timer_t * album_load_timer;
atomic_int album_orphans; /* given-up album reads still running, see gui_library_async.h */

static void free_album_load_request(album_load_request_t * request) {
    if (!request) return;
    free_group_song_entries(request->entries, request->count);
    free(request);
}

/* The album (name, album artist) as the Albums list has it now; false when it is not listed. */
bool album_group_resolve(const char * name, const char * album_artist, group_row_t * out) {
    int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, name, album_artist);
    if (offset < 0 || metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int) offset, 1, out) != 1 ||
        out->song_count <= 0)
        return false;
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", album_artist);
    return true;
}

/* Tracks of an album as the Albums list counts them (0 when it is not listed). */
int album_group_song_count(const char * name, const char * album_artist) {
    group_row_t group;
    return album_group_resolve(name, album_artist, &group) ? group.song_count : 0;
}

static void * album_load_worker(void * arg) {
    album_load_request_t * request = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    int song_count = request->group.song_count;
    if (request->refresh && !request->artist_filter[0])
        song_count = album_group_song_count(request->group.name, request->group.album_artist);
    request->entries = load_album_entries_filtered(request->group.name, request->group.album_artist,
        song_count, request->artist_filter, &request->count, &request->cancel);
    atomic_store_explicit(&request->done, true, memory_order_release);
    if (library_async_worker_done(&request->fin, &album_orphans)) free_album_load_request(request); /* the owner gave it up */
    ui_wake_notify(); /* populate the page on the next loop pass, not the next 25 ms poll */
    return NULL;
}

/* Gives up a running read without waiting for it: the worker (a card read cannot be
 * interrupted) frees the request when it ends, and nothing it produces is looked at. */
static void album_load_abandon(album_load_request_t * request) {
    atomic_store_explicit(&request->cancel, true, memory_order_relaxed);
    pthread_detach(album_load_thread);
    if (library_async_abandon(&request->fin, &album_orphans)) free_album_load_request(request);
}

static bool start_album_load(album_load_request_t * request) {
    if (pthread_create(&album_load_thread, NULL, album_load_worker, request) != 0) return false;
    album_load_job = request;
    return true;
}

static void album_load_poll(lv_timer_t * timer) {
    if (album_load_job && album_load_job->prefetch) {
        /* Not tapped (yet): keep a finished read for the tap, drop it once it
         * was cancelled or has waited too long. */
        if (lv_tick_elaps(album_load_job->prefetch_tick) > ALBUM_PREFETCH_TTL_MS)
            atomic_store_explicit(&album_load_job->cancel, true, memory_order_relaxed);
        if (!atomic_load_explicit(&album_load_job->cancel, memory_order_relaxed) ||
            !atomic_load_explicit(&album_load_job->done, memory_order_acquire)) return;
        pthread_join(album_load_thread, NULL);
        free_album_load_request(album_load_job);
        album_load_job = NULL;
    }
    if (album_load_job && atomic_load_explicit(&album_load_job->done, memory_order_acquire)) {
        /* Do not modify the destination while a transition shows its frozen
         * loading snapshot. Back navigation is allowed throughout the read. */
        if (gui_navigation_transition_in_progress()) return;
        pthread_join(album_load_thread, NULL);
        album_load_request_t * request = album_load_job;
        album_load_job = NULL;
        bool current = request->generation == album_load_generation &&
            library_screen_in_navigation(group_songs_screen) &&
            group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM &&
            !atomic_load_explicit(&request->cancel, memory_order_relaxed);
        if (current && request->refresh) {
            /* Read from the published catalog, so a cover reload or a scan that has not
             * published yet does not make it stale; a publication brings its own refresh. */
            if (request->catalog_generation == tagcache_generation()) {
                group_song_entry_t * entries = request->entries;
                int count = request->count;
                request->entries = NULL;
                request->count = 0;
                group_songs_replace_refreshed_entries(entries, entries ? count : 0);
                gui_navigation_invalidate_back_snapshot(group_songs_screen);
            }
        } else if (current) {
            bool valid = request->catalog_generation == tagcache_generation() && !library_rescan_active;
            if (valid && request->entries) {
                set_group_songs_entries_owned(request->entries, request->count);
                request->entries = NULL;
                request->count = 0;
            }
            populate_group_songs_rows();
            gui_navigation_invalidate_back_snapshot(group_songs_screen);
            if (lv_screen_active() == group_songs_screen) {
                if (!valid) show_info_toast(TR("Library changed. Open the album again."));
                else if (!group_songs_count) show_error_toast(TR("Cannot load album tracks"));
                else if (request->autoplay_mode >= 0)
                    (void) play_current_group((play_mode_t) request->autoplay_mode);
            }
        }
        free_album_load_request(request);
    }
    if (!album_load_job && album_load_pending) {
        album_load_request_t * request = album_load_pending;
        album_load_pending = NULL;
        lv_timer_set_period(timer, POLL_FAST_MS);
        if (request->generation != album_load_generation ||
            !library_screen_in_navigation(group_songs_screen)) {
            free_album_load_request(request);
        } else if (library_async_kind_saturated(&album_orphans)) {
            album_load_pending = request; /* two stalled reads: it starts when one ends */
            lv_timer_set_period(timer, POLL_SATURATED_MS); /* a worker exit wakes the loop anyway */
        } else if (!start_album_load(request)) {
            free_album_load_request(request);
            show_error_toast(TR("Cannot load album tracks"));
            if (lv_screen_active() == group_songs_screen) populate_group_songs_rows();
        }
    }
    if (!album_load_job && !album_load_pending) {
        lv_timer_set_period(timer, POLL_FAST_MS);
        lv_timer_pause(timer);
    }
}

void cancel_album_load(void) {
    cancel_genre_load();
    cancel_artist_songs_load();
    ++album_load_generation;
    if (album_load_job) {
        album_load_abandon(album_load_job);
        album_load_job = NULL;
    }
    free_album_load_request(album_load_pending);
    album_load_pending = NULL;
    if (album_load_timer) lv_timer_delete(album_load_timer);
    album_load_timer = NULL;
    /* Teardown, reload and shutdown only: let the reads we gave up end (bounded) before
     * the library they read from is replaced. */
    library_async_wait_orphans(orphan_wait_deadline());
}

/* The read a touch of this album started, taken over by the tap. NULL when
 * there is none, it was dropped, or it belongs to another album or catalog. */
static album_load_request_t * album_prefetch_adopt(const group_row_t * group, const char * artist_filter) {
    album_load_request_t * request = album_load_job;
    if (!request || !request->prefetch || atomic_load_explicit(&request->cancel, memory_order_relaxed) ||
        request->catalog_generation != tagcache_generation() || library_rescan_active ||
        strcmp(request->group.name, group->name) != 0 ||
        strcmp(request->group.album_artist, group->album_artist) != 0 ||
        strcmp(request->artist_filter, artist_filter ? artist_filter : "") != 0)
        return NULL;
    request->prefetch = false;
    return request;
}

void album_prefetch_cancel(void) {
    if (!album_load_job || !album_load_job->prefetch) return;
    atomic_store_explicit(&album_load_job->cancel, true, memory_order_relaxed);
    if (album_load_timer) lv_timer_resume(album_load_timer); /* reaps it without blocking the touch */
}

/* A finger went down on this album: read its tracks now, so a tap finds them in
 * (or on their way). Never while another album read is running. */
void album_prefetch_start(const group_row_t * group, const char * artist_filter) {
    if (library_rescan_active || album_load_job || album_load_pending || library_async_kind_saturated(&album_orphans)) return;
    album_load_request_t * request = calloc(1, sizeof(*request));
    if (!request) return;
    request->group = *group;
    request->autoplay_mode = -1;
    snprintf(request->artist_filter, sizeof(request->artist_filter), "%s", artist_filter ? artist_filter : "");
    request->catalog_generation = tagcache_generation();
    request->generation = UINT64_MAX; /* never the generation of a shown page */
    request->prefetch = true;
    request->prefetch_tick = lv_tick_get();
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!album_load_timer) album_load_timer = lv_timer_create(album_load_poll, 25, NULL);
    if (!album_load_timer || !start_album_load(request)) { free(request); return; }
    lv_timer_resume(album_load_timer);
}

/* Runs `request` as the album read, or keeps it as the one pending request (the latest wins).
 * A read in flight for another album is given up rather than waited for. On false the request
 * is freed. */
static bool album_load_submit(album_load_request_t * request) {
    if (album_load_job && album_load_job != request && !library_async_kind_has_orphan(&album_orphans)) {
        /* Another album's read (a cancelled touch) is still going: give it up instead of waiting
         * for it. If one was given up already and is still stalled, this tap waits as the latest
         * pending request, so the reads in flight stay at one given up plus one active. */
        album_load_abandon(album_load_job);
        album_load_job = NULL;
    }
    if (!album_load_job && !library_async_kind_saturated(&album_orphans) && !start_album_load(request)) {
        free(request);
        return false;
    }
    if (album_load_job != request) {
        if (album_load_job) atomic_store_explicit(&album_load_job->cancel, true, memory_order_relaxed);
        free_album_load_request(album_load_pending);
        album_load_pending = request;
    }
    return true;
}

/* Reads the album page already shown again (after a targeted metadata publish) and replaces its
 * rows in place when it lands: no screen is pushed, and a result for a page that has since
 * changed or left is dropped. False when the read could not be started (nothing is pending). */
bool album_refresh_in_place_start(const char * name, const char * album_artist, const char * artist_filter) {
    /* A read that opens an album page is not displaced by a refresh (its follow-up Play would be
     * lost): the caller reads in place, as it always did, while one is in flight. */
    if ((album_load_job && !album_load_job->prefetch && !album_load_job->refresh) ||
        (album_load_pending && !album_load_pending->refresh))
        return false;
    album_load_request_t * request = calloc(1, sizeof(*request));
    if (!request) return false;
    snprintf(request->group.name, sizeof(request->group.name), "%s", name);
    snprintf(request->group.album_artist, sizeof(request->group.album_artist), "%s", album_artist);
    request->autoplay_mode = -1;
    request->refresh = true;
    snprintf(request->artist_filter, sizeof(request->artist_filter), "%s", artist_filter ? artist_filter : "");
    request->catalog_generation = tagcache_generation();
    request->generation = album_load_generation;
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!album_load_timer) album_load_timer = lv_timer_create(album_load_poll, 25, NULL);
    if (!album_load_timer) { free(request); return false; }
    if (!album_load_submit(request)) return false;
    lv_timer_resume(album_load_timer);
    return true;
}

/* `built_now` (optional) is set when the page was built from tracks that were already in, so no
 * load is pending that a caller could attach a follow-up action (Play, Shuffle) to. */
bool show_album_group_ex(const group_row_t * group, const char * artist_filter, bool * built_now) {
    if (built_now) *built_now = false;
    album_load_request_t * request = album_prefetch_adopt(group, artist_filter);
    if (request && atomic_load_explicit(&request->done, memory_order_acquire)) {
        /* The tracks are already in: build the page once, with them. */
        pthread_join(album_load_thread, NULL);
        album_load_job = NULL;
        if (request->entries && request->count > 0) {
            group_songs_loading = false;
            /* The page takes the entries over, no copy of them is made. */
            group_song_entry_t * entries = request->entries;
            int count = request->count;
            request->entries = NULL;
            request->count = 0;
            show_music_group_songs_impl(group->name, group->album_artist, artist_filter, entries, count, true);
            free_album_load_request(request);
            if (album_load_timer) lv_timer_resume(album_load_timer); /* it pauses itself when idle */
            if (built_now) *built_now = true;
            return true;
        }
        free_album_load_request(request);
        request = NULL;
    }
    if (request) {
        /* Still reading: the usual loading page follows, the read just began earlier. */
        album_load_job = request;
    } else {
        request = calloc(1, sizeof(*request)); /* with two stalled reads it waits as the pending request */
        if (!request) return false;
        request->group = *group;
        request->autoplay_mode = -1;
        snprintf(request->artist_filter, sizeof(request->artist_filter), "%s", artist_filter ? artist_filter : "");
        request->catalog_generation = tagcache_generation();
        atomic_init(&request->cancel, false);
        atomic_init(&request->done, false);
    }
    if (!album_load_timer) album_load_timer = lv_timer_create(album_load_poll, 25, NULL);
    if (!album_load_timer) { free(request); return false; }
    if (!album_load_submit(request)) return false;
    group_songs_loading = true;
    show_music_group_songs_impl(group->name, group->album_artist, artist_filter, NULL, 0, false);
    group_songs_loading = false;
    request->generation = album_load_generation;
    lv_timer_resume(album_load_timer);
    return true;
}

bool show_album_group_filtered(const group_row_t * group, const char * artist_filter) {
    return show_album_group_ex(group, artist_filter, NULL);
}

static bool show_album_group(const group_row_t * group) {
    return show_album_group_filtered(group, NULL);
}

/* Shared library screens may already be below Now Playing. Once the target
 * has loaded successfully, return to its original slot instead of keeping
 * a second copy and the intervening player entries in the Back history. */
static void collapse_now_playing_library_return(lv_obj_t * target) {
    if (!target || lv_screen_active() != target) return;
    int depth = gui_navigation_get_depth();
    for (int i = 0; i < depth - 1; i++) {
        if (gui_navigation_get_screen_at(i) != target) continue;
        while (gui_navigation_get_depth() > i + 1)
            nav_remove_stack_slot(gui_navigation_get_depth() - 1);
        return;
    }
}

bool gui_library_open_now_playing_group(const char * path, bool album) {
    if (!path || !path[0] || library_rescan_active || gui_library_navigation_blocked()) return false;

    /* A local tagcache path is the membership guard. This keeps provider,
     * radio, and Subsonic metadata from accidentally opening a local group
     * that happens to share the same display name. The indexed path lookup
     * and following group lookup stay bounded; no library-wide song list is
     * materialized on the UI thread. */
    song_row_t song;
    if (!metadata_db_get_song_by_path(path, &song)) return false;

    if (!album) {
        if (!song.tags.artist[0]) return false;
        char artist_name[TAGCACHE_TAG_MAX];
        tagcache_artist_primary(song.tags.artist, artist_name, sizeof(artist_name));
        if (!artist_name[0]) return false;
        int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ARTIST,
                                                       artist_name, NULL);
        group_row_t artist;
        if (offset < 0 || offset > INT_MAX ||
            metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, (int) offset, 1, &artist) != 1)
            return false;
        if (tagcache_cmp_ascii(artist.name, artist_name) != 0) return false;
        artist_open_song_hint = artist.song_count;
        show_artist_albums(artist.name, METADATA_DB_GROUP_ARTIST);
        collapse_now_playing_library_return(artist_albums_screen);
        return true;
    }

    if (!song.tags.album[0]) return false;
    /* The scanner normalizes missing ALBUMARTIST to ARTIST; keep that same
     * effective identity for older cache rows as well. This remains distinct
     * from song.tags.artist when an album artist is explicitly present. */
    const char * album_artist = song.tags.album_artist[0]
        ? song.tags.album_artist : song.tags.artist;
    int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                                                   song.tags.album, album_artist);
    /* Rows written by the current scanner use ARTIST when ALBUMARTIST is
     * absent. Older tagcache generations can still have an empty album-artist
     * key, so allow that exact legacy pair if the normalized identity misses. */
    const char * group_album_artist = album_artist;
    if (offset < 0 && !song.tags.album_artist[0]) {
        offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                                              song.tags.album, "");
        group_album_artist = "";
    }
    group_row_t group;
    if (offset < 0 || offset > INT_MAX ||
        metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int) offset, 1, &group) != 1)
        return false;
    /* Guard against an offset resolving to a different same-titled album
     * if the database changed between the path and group lookups. */
    if (tagcache_cmp_ascii(group.name, song.tags.album) != 0 ||
        tagcache_cmp_ascii(group.album_artist, group_album_artist) != 0) return false;
    if (!show_album_group(&group)) return false;
    collapse_now_playing_library_return(group_songs_screen);
    return true;
}

static void album_row_click_cb(int index) {
    if (library_rescan_active) return;
    /* Resolve this specific (album, album_artist) pair at its current
     * display position via a single-row offset lookup (same pattern as All
     * Songs' own row-click resolution) -- disambiguated by album_artist too,
     * so two different artists sharing an album title never collide. */
    group_row_t group;
    if (!press_resolved_take(PRESS_SRC_ALBUMS, index, &group) && !resolve_album_display_row(index, &group)) return;
    (void) show_album_group(&group);
}

/* An album by identity, as tapping it in the Albums list does (Cover Flow); nothing if it is gone. */
void gui_library_open_album(const char * name, const char * album_artist) {
    group_row_t group;
    if (library_rescan_active || !album_group_resolve(name, album_artist, &group)) return;
    (void) show_album_group(&group);
}

static void album_row_press_cb(int index, bool pressed) {
    press_resolved_clear();
    if (!pressed) { album_prefetch_cancel(); return; }
    group_row_t group;
    if (library_rescan_active || !resolve_album_display_row(index, &group)) return;
    press_resolved_set(PRESS_SRC_ALBUMS, index, &group);
    album_prefetch_start(&group, NULL);
}

/* Builds the Artists screen using a paged provider
 * (compact_list_set_paged_provider()) to load artist groups incrementally. */
lv_obj_t * build_artists_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Artists"), generic_back_cb, NULL, 0, artist_row_click_cb,
                                                artist_collection_long_press_cb, &artists_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(artists_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    compact_list_set_paged_provider(artists_list, artists_fetch_page, NULL, artist_count);
    library_thumbnails_bind_list(scr, artists_list, LIBRARY_THUMB_LIST_ARTISTS);
    compact_list_set_trailing_click(artists_list, artist_collection_more_click_cb);
    compact_list_set_press_cb(artists_list, artist_row_press_cb);
    finalize_screen_navigation(scr);
    return scr;
}

lv_obj_t * build_albums_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Albums"), generic_back_cb, NULL, 0, album_row_click_cb,
                                                album_row_long_press_cb, &albums_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(albums_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    /* Cache invalidation belongs to the operation that replaces the library
     * (refresh_library_screens_after_reload(), SD reinsertion, UI reload),
     * done once there. Clearing here repeated it and, mid-rebuild, redrew
     * lists that were not rebuilt yet. */
    /* Decorator before the paged provider so the first (now synchronous)
     * window fill already has the thumbnail pad_left and cover slot; names
     * then lay out to the right of the art instead of under it. */
    library_thumbnails_bind_list(scr, albums_list, LIBRARY_THUMB_LIST_ALBUMS);
    compact_list_set_trailing_click(albums_list, album_more_click_cb);
    compact_list_set_press_cb(albums_list, album_row_press_cb);
    compact_list_set_paged_provider(albums_list, albums_fetch_page, NULL, album_count);
    finalize_screen_navigation(scr);
    return scr;
}

static void album_artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    group_row_t group;
    if (!press_resolved_take(PRESS_SRC_ALBUM_ARTISTS, index, &group)) {
        int resolved = search_remap_index(SEARCH_BINDING_ALBUM_ARTIST, index);
        if (metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, resolved, 1, &group) != 1) return;
    }
    artist_open_song_hint = group.song_count;
    show_artist_albums(group.name, METADATA_DB_GROUP_ALBUM_ARTIST);
}

static void album_artist_row_press_cb(int index, bool pressed) {
    artist_prefetch_press(index, pressed, SEARCH_BINDING_ALBUM_ARTIST, METADATA_DB_GROUP_ALBUM_ARTIST,
                          PRESS_SRC_ALBUM_ARTISTS);
}

lv_obj_t * build_album_artist_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Album Artist"), generic_back_cb, NULL, 0, album_artist_row_click_cb,
                                                album_artist_collection_long_press_cb, &album_artist_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    compact_list_set_row_height(album_artist_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    library_thumbnails_resize_alias_cache();
    compact_list_set_paged_provider(album_artist_list, album_artists_fetch_page, NULL, album_artist_count);
    library_thumbnails_bind_list(scr, album_artist_list, LIBRARY_THUMB_LIST_ALBUM_ARTISTS);
    compact_list_set_trailing_click(album_artist_list, album_artist_collection_more_click_cb);
    compact_list_set_press_cb(album_artist_list, album_artist_row_press_cb);
    finalize_screen_navigation(scr);
    return scr;
}

int count_genres_from_database(void) {
    return metadata_db_get_genre_count();
}

int genres_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    if (!rows) return 0;
    int n = metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, offset, count, rows);
    for (int i = 0; i < n; i++) {
        const char * name = rows[i].genre_name[0] ? rows[i].genre_name : rows[i].name;
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", name);
        snprintf(out_rows[i].subtitle, sizeof(out_rows[i].subtitle), TR_N("%d song", "%d songs", rows[i].song_count),
                 rows[i].song_count);
        out_rows[i].identity = rows[i].first_song_id;
    }
    free(rows);
    return n;
}

#define GENRE_SONGS_CAP 4000

/* The songs of a genre as page entries, read in one pass over the song list
 * (see metadata_db_walk_genre_songs()). Runs on the genre-load worker, or on the
 * UI thread for the rare in-place refresh after a database change. */
typedef struct {
    group_song_entry_t * entries;
    int count, capacity;
    bool failed;
    const atomic_bool * cancel; /* set when the result is no longer wanted */
} genre_collect_t;

static bool genre_collect_song(const song_row_t * song, void * user) {
    genre_collect_t * collect = user;
    if (collect->cancel && atomic_load_explicit(collect->cancel, memory_order_relaxed)) return false;
    if (collect->count == collect->capacity) {
        int capacity = collect->capacity ? collect->capacity * 2 : 256;
        if (capacity > GENRE_SONGS_CAP) capacity = GENRE_SONGS_CAP;
        if (capacity <= collect->count) return false;
        group_song_entry_t * grown = realloc(collect->entries, sizeof(*grown) * (size_t) capacity);
        if (!grown) { collect->failed = true; return false; }
        memset(grown + collect->capacity, 0, sizeof(*grown) * (size_t) (capacity - collect->capacity));
        collect->entries = grown;
        collect->capacity = capacity;
    }
    group_song_entry_t * entry = &collect->entries[collect->count];
    char title[384];
    format_music_submenu_identity(song, title, sizeof(title));
    entry->path = strdup(song->path);
    entry->title = strdup(title);
    entry->song_id = song->id;
    entry->artwork_key = song_album_thumbnail_key(song);
    entry->disc_number = song->tags.disc_number > 0 ? song->tags.disc_number : 1;
    entry->show_disc_header = false;
    collect->count++; /* counted before the check so a half-built entry is freed with the rest */
    if (!entry->path || !entry->title) { collect->failed = true; return false; }
    return true;
}

/* NULL (and *out_count 0) when the genre has no songs, memory ran out, or the
 * database changed while it was read. */
static group_song_entry_t * load_genre_entries(const char * genre, int * out_count, const atomic_bool * cancel) {
    *out_count = 0;
    genre_collect_t collect = { .cancel = cancel };
    bool walked = metadata_db_walk_genre_songs(genre, GENRE_SONGS_CAP, genre_collect_song, &collect, cancel);
    if (!walked || collect.failed || collect.count <= 0) {
        free_group_song_entries(collect.entries, collect.count);
        return NULL;
    }
    if (collect.count < collect.capacity) {
        group_song_entry_t * shrunk = realloc(collect.entries, sizeof(*shrunk) * (size_t) collect.count);
        if (shrunk) collect.entries = shrunk;
    }
    *out_count = collect.count;
    return collect.entries;
}

static void show_genre_songs_loaded(const char * genre_name, group_song_entry_t * entries, int count,
                                    bool push_screen) {
    group_songs_refresh_source = GROUP_SONGS_REFRESH_GENRE;
    snprintf(group_songs_refresh_genre, sizeof(group_songs_refresh_genre), "%s", genre_name);
    show_group_songs_take_ownership(genre_name, entries, count, push_screen);
}

/* In-place refresh after a database change: same screen, read right here. */
bool show_genre_songs_internal(bool push_screen, const char * genre_name) {
    if (!genre_name || !genre_name[0]) return false;
    /* Database refresh passes the saved genre buffer back into this function. */
    char genre[sizeof(group_songs_refresh_genre)];
    snprintf(genre, sizeof(genre), "%s", genre_name);
    int count = 0;
    group_song_entry_t * entries = load_genre_entries(genre, &count, NULL);
    if (!entries) return false;
    show_genre_songs_loaded(genre, entries, count, push_screen);
    return true;
}

#define LANDING_WAIT_MAX_MS 5000

static bool landing_modal_open(void) {
    return gui_popup_visible_count() > 0 || gui_library_navigation_blocked() ||
           lv_screen_active() == gui_busy_get_screen() ||
           library_collection_menu_visible();
}

landing_verdict_t landing_verdict(lv_obj_t * origin, bool * waiting, uint32_t * since) {
    if (lv_screen_active() != origin || landing_modal_open()) return LANDING_DROP;
    if (!landing_pointer_busy()) return LANDING_PUSH;
    if (!*waiting) { *waiting = true; *since = lv_tick_get(); }
    return lv_tick_elaps(*since) > LANDING_WAIT_MAX_MS ? LANDING_DROP : LANDING_WAIT;
}

/* A tap on a genre reads its songs on a worker and pushes the screen when they
 * are in, instead of scanning the whole song list on the UI thread (about
 * 130 ms) before anything could move. As before, the screen appears only once
 * the songs are loaded, and nothing appears for a genre with none. One read at
 * a time, the last tap wins; only the completion timer touches the LVGL tree. */
typedef struct {
    char genre[METADATA_DB_TEXT_MAX];
    uint64_t generation;
    int32_t catalog_generation;
    lv_obj_t * origin;   /* the screen the tap came from: the push is only valid while it is shown */
    bool left;           /* the origin screen was left since the tap */
    atomic_bool cancel, done;
    group_song_entry_t * entries;
    int count;
    /* Started by a touch, before the tap: reads the songs and pushes nothing
     * until a tap on the same genre adopts it (prefetch cleared). Dropped (left
     * set) by a scroll, a lost press, a TTL or leaving the screen. */
    bool prefetch;
    uint32_t prefetch_tick;
    bool landing_waiting;   /* the songs are in but a touch or a menu holds the push back */
    uint32_t landing_since;
    atomic_int fin;         /* worker/owner handoff, see gui_library_async.h */
} genre_load_request_t;
#define GENRE_PREFETCH_TTL_MS 3000
static genre_load_request_t * genre_load_job;
static pthread_t genre_load_thread;
static lv_timer_t * genre_load_timer;
static uint64_t genre_load_generation;
static atomic_int genre_orphans; /* given-up genre reads still running */
/* The latest tap that arrived while a given-up read and a stalled active one already exist:
 * it starts when the active read ends. */
static char genre_load_pending[METADATA_DB_TEXT_MAX];
static lv_obj_t * genre_load_pending_origin;
static bool genre_load_launch(const char * genre_name);

static void free_genre_load_request(genre_load_request_t * request) {
    if (!request) return;
    free_group_song_entries(request->entries, request->count);
    free(request);
}

static void * genre_load_worker(void * arg) {
    genre_load_request_t * request = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    request->entries = load_genre_entries(request->genre, &request->count, &request->cancel);
    atomic_store_explicit(&request->done, true, memory_order_release);
    if (library_async_worker_done(&request->fin, &genre_orphans)) free_genre_load_request(request); /* the owner gave it up */
    ui_wake_notify();
    return NULL;
}

static bool genre_load_launch(const char * genre_name);
static void genre_origin_unloaded_cb(lv_event_t * e);
static void genre_load_poll(lv_timer_t * timer) {
    if (!genre_load_job && genre_load_pending[0]) {
        /* The tap that found two stalled reads waits here for a slot (the last tap wins). */
        if (library_async_kind_saturated(&genre_orphans)) {
            lv_timer_set_period(timer, POLL_SATURATED_MS);
            return;
        }
        lv_timer_set_period(timer, POLL_FAST_MS);
        char name[METADATA_DB_TEXT_MAX];
        snprintf(name, sizeof(name), "%s", genre_load_pending);
        genre_load_pending[0] = '\0';
        if (genre_load_pending_origin == lv_screen_active()) (void) genre_load_launch(name);
        if (!genre_load_job) lv_timer_pause(timer);
        return;
    }
    if (!genre_load_job) {
        /* Nothing left to wait for (a pending tap whose screen was left, a request given up). */
        lv_timer_set_period(timer, POLL_FAST_MS);
        lv_timer_pause(timer);
        return;
    }
    if (genre_load_job && genre_load_job->prefetch) {
        if (lv_tick_elaps(genre_load_job->prefetch_tick) > GENRE_PREFETCH_TTL_MS) {
            genre_load_job->left = true;
            atomic_store_explicit(&genre_load_job->cancel, true, memory_order_relaxed);
        }
        if (!genre_load_job->left) return; /* waiting for a tap, finished or not */
    }
    if (!atomic_load_explicit(&genre_load_job->done, memory_order_acquire)) return;
    /* Do not push while another slide is running. */
    if (gui_navigation_transition_in_progress()) return;
    bool wanted = genre_load_job->generation == genre_load_generation && genre_load_job->entries &&
        !library_rescan_active && !genre_load_job->left &&
        genre_load_job->catalog_generation == tagcache_generation();
    landing_verdict_t verdict = wanted ? landing_verdict(genre_load_job->origin, &genre_load_job->landing_waiting,
                                                         &genre_load_job->landing_since)
                                       : LANDING_DROP;
    if (verdict == LANDING_WAIT) return; /* the finished read waits for the pointer to be released */
    pthread_join(genre_load_thread, NULL); /* finished: returns at once */
    genre_load_request_t * request = genre_load_job;
    genre_load_job = NULL;
    lv_timer_pause(timer);
    bool current = verdict == LANDING_PUSH;
    if (current) {
        group_song_entry_t * entries = request->entries;
        int count = request->count;
        request->entries = NULL;
        request->count = 0;
        show_genre_songs_loaded(request->genre, entries, count, true);
    }
    free_genre_load_request(request);
    if (genre_load_pending[0]) {
        char name[METADATA_DB_TEXT_MAX];
        snprintf(name, sizeof(name), "%s", genre_load_pending);
        genre_load_pending[0] = '\0';
        if (genre_load_pending_origin == lv_screen_active()) (void) genre_load_launch(name);
    }
}

bool genre_load_busy(void) {
    return genre_load_job && !(genre_load_job->prefetch && atomic_load_explicit(&genre_load_job->done, memory_order_acquire));
}

void genre_load_ready_timer(void) {
    if (genre_load_timer && !lv_timer_get_paused(genre_load_timer) &&
        (genre_load_job ? atomic_load_explicit(&genre_load_job->done, memory_order_acquire)
                        : (genre_load_pending[0] && !library_async_kind_saturated(&genre_orphans))))
        lv_timer_ready(genre_load_timer);
}

/* Leaving the screen the tap came from drops the result, even if the user comes
 * back before it is in. */
static void genre_origin_unloaded_cb(lv_event_t * e) {
    if (genre_load_pending[0] && lv_event_get_target(e) == genre_load_pending_origin) genre_load_pending[0] = '\0';
    if (!genre_load_job || lv_event_get_target(e) != genre_load_job->origin) return;
    genre_load_job->left = true;
    atomic_store_explicit(&genre_load_job->cancel, true, memory_order_relaxed);
}

static void cancel_genre_load(void) {
    ++genre_load_generation;
    genre_load_pending[0] = '\0';
    if (genre_load_job) {
        /* Never joined here: the UI thread must not wait for a card read (the restart of a
         * cancelled touch, a new tap). The worker frees the request when it ends. */
        atomic_store_explicit(&genre_load_job->cancel, true, memory_order_relaxed);
        pthread_detach(genre_load_thread);
        if (library_async_abandon(&genre_load_job->fin, &genre_orphans)) free_genre_load_request(genre_load_job);
        genre_load_job = NULL;
    }
    if (genre_load_timer) lv_timer_delete(genre_load_timer);
    genre_load_timer = NULL;
}

static bool start_genre_songs_load(const char * genre_name) {
    if (!genre_name || !genre_name[0]) return false;
    genre_load_request_t * touched = genre_load_job;
    if (touched && touched->prefetch && !touched->left && genre_load_timer &&
        touched->origin == lv_screen_active() && touched->catalog_generation == tagcache_generation() &&
        strcmp(touched->genre, genre_name) == 0) {
        /* The touch already started this read: the tap just lets it push. */
        touched->prefetch = false;
        lv_timer_resume(genre_load_timer);
        lv_timer_ready(genre_load_timer);
        return true;
    }
    if (touched && !touched->prefetch && !touched->left && genre_load_timer && touched->origin == lv_screen_active() &&
        !atomic_load_explicit(&touched->cancel, memory_order_relaxed) &&
        touched->catalog_generation == tagcache_generation() && strcmp(touched->genre, genre_name) == 0)
        return true; /* the same genre is already on its way: a second tap adopts that read */
    if (genre_load_job && !atomic_load_explicit(&genre_load_job->done, memory_order_acquire) &&
        library_async_kind_has_orphan(&genre_orphans)) {
        /* A given-up read is still stalled and so is the active one: keep it, drop its result and
         * start this genre when it ends (the last tap wins) rather than adding a third. */
        ++genre_load_generation;
        genre_load_job->left = true;
        atomic_store_explicit(&genre_load_job->cancel, true, memory_order_relaxed);
        snprintf(genre_load_pending, sizeof(genre_load_pending), "%s", genre_name);
        genre_load_pending_origin = lv_screen_active();
        lv_obj_remove_event_cb(genre_load_pending_origin, genre_origin_unloaded_cb);
        lv_obj_add_event_cb(genre_load_pending_origin, genre_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
        if (genre_load_timer) lv_timer_resume(genre_load_timer);
        return true;
    }
    cancel_genre_load(); /* the last tap wins: a load still running is superseded */
    return genre_load_launch(genre_name);
}

static bool genre_load_launch(const char * genre_name) {
    if (library_async_kind_saturated(&genre_orphans)) {
        /* Two stalled reads already: keep this tap as the pending request, it starts when one ends. */
        if (!genre_load_timer) genre_load_timer = lv_timer_create(genre_load_poll, 25, NULL);
        if (!genre_load_timer) return false;
        snprintf(genre_load_pending, sizeof(genre_load_pending), "%s", genre_name);
        genre_load_pending_origin = lv_screen_active();
        lv_obj_remove_event_cb(genre_load_pending_origin, genre_origin_unloaded_cb);
        lv_obj_add_event_cb(genre_load_pending_origin, genre_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
        lv_timer_resume(genre_load_timer);
        return true;
    }
    genre_load_request_t * request = calloc(1, sizeof(*request));
    if (!request) return false;
    snprintf(request->genre, sizeof(request->genre), "%s", genre_name);
    request->generation = ++genre_load_generation;
    request->catalog_generation = tagcache_generation();
    request->origin = lv_screen_active();
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!genre_load_timer) genre_load_timer = lv_timer_create(genre_load_poll, 25, NULL);
    if (!genre_load_timer) { free(request); return false; }
    if (pthread_create(&genre_load_thread, NULL, genre_load_worker, request) != 0) {
        free(request);
        return false;
    }
    genre_load_job = request;
    lv_obj_remove_event_cb(request->origin, genre_origin_unloaded_cb);
    lv_obj_add_event_cb(request->origin, genre_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_timer_resume(genre_load_timer);
    return true;
}

/* ---- Genre browsing by artist or album (issue #191) ----
 *
 * The Genres title-bar settings icon picks how a genre opens: All Songs keeps
 * the flat song list, Artists / Album Artist list the genre's artists and
 * then each artist's albums in that genre, Albums lists the genre's albums.
 * An album opens the normal album screen with all of its tracks. The page
 * providers can run on the compact-list fetch worker, so the current query
 * is read under genre_browse_mutex. */
lv_obj_t * genre_groups_screen = NULL;
lv_obj_t * genre_groups_list = NULL;
lv_obj_t * genre_groups_title_label = NULL;
lv_obj_t * genre_albums_screen = NULL;
lv_obj_t * genre_albums_list = NULL;
lv_obj_t * genre_albums_title_label = NULL;
lv_obj_t * genre_view_screen = NULL;
lv_obj_t * genre_view_options[GENRE_VIEW_COUNT];
static pthread_mutex_t genre_browse_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    char genre[METADATA_DB_TEXT_MAX];
    int view;
    char artist[128];
} genre_browse_query_t;
static genre_browse_query_t genre_browse_current;

static genre_browse_query_t genre_browse_snapshot(void) {
    pthread_mutex_lock(&genre_browse_mutex);
    genre_browse_query_t query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    return query;
}

int genre_browse_view(void) {
    return genre_browse_snapshot().view;
}

static metadata_db_group_kind_t genre_view_artist_kind(int view) {
    return view == GENRE_VIEW_ALBUM_ARTISTS ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST;
}

static metadata_db_group_kind_t genre_view_group_kind(int view) {
    return view == GENRE_VIEW_ALBUMS ? METADATA_DB_GROUP_ALBUM : genre_view_artist_kind(view);
}

/* Same row content as Music -> Albums and Music -> Artists. */
static void fill_genre_album_row(compact_list_page_row_t * out, const group_row_t * row) {
    snprintf(out->label, sizeof(out->label), "%s", row->name);
    snprintf(out->subtitle, sizeof(out->subtitle), "%s", row->album_artist);
    out->identity = row->first_song_id;
    fill_album_cover_key(out, row);
}

static void fill_genre_artist_row(compact_list_page_row_t * out, const group_row_t * row) {
    snprintf(out->label, sizeof(out->label), "%s", row->name);
    snprintf(out->subtitle, sizeof(out->subtitle), TR_N("%d song", "%d songs", row->song_count), row->song_count);
    out->identity = row->first_song_id;
    out->artwork_key = albumart_artist_thumbnail_key(row->name);
    snprintf(out->artwork_name, sizeof(out->artwork_name), "%s", row->name);
}

static int genre_groups_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    metadata_db_group_kind_t kind = genre_view_group_kind(query.view);
    int n = rows ? metadata_db_get_genre_groups_page(kind, query.genre, METADATA_DB_GROUP_ARTIST, NULL,
                                                     offset, count, rows, NULL) : 0;
    for (int i = 0; i < n; i++) {
        if (kind == METADATA_DB_GROUP_ALBUM) fill_genre_album_row(&out_rows[i], &rows[i]);
        else fill_genre_artist_row(&out_rows[i], &rows[i]);
    }
    free(rows);
    return n;
}

static int genre_albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query.genre,
                                                     genre_view_artist_kind(query.view), query.artist,
                                                     offset, count, rows, NULL) : 0;
    for (int i = 0; i < n; i++) fill_genre_album_row(&out_rows[i], &rows[i]);
    free(rows);
    return n;
}

static int genre_groups_count(const genre_browse_query_t * query, bool albums_of_artist) {
    int total = 0;
    if (albums_of_artist)
        (void) metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query->genre,
                                                 genre_view_artist_kind(query->view), query->artist,
                                                 0, 0, NULL, &total);
    else
        (void) metadata_db_get_genre_groups_page(genre_view_group_kind(query->view), query->genre,
                                                 METADATA_DB_GROUP_ARTIST, NULL, 0, 0, NULL, &total);
    return total;
}

/* Genre rows carry the scanner's album identity; resolve the real album
 * group (with the legacy empty album-artist key as fallback) so the album
 * screen gets its full track count. */
static void open_genre_album(const group_row_t * row) {
    const char * album_artist = row->album_artist;
    int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row->name, album_artist);
    if (offset < 0) {
        album_artist = "";
        offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row->name, album_artist);
    }
    group_row_t group;
    if (offset < 0 || offset > INT_MAX ||
        metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, (int) offset, 1, &group) != 1) return;
    if (tagcache_cmp_ascii(group.name, row->name) != 0 ||
        tagcache_cmp_ascii(group.album_artist, album_artist) != 0) return;
    (void) show_album_group(&group);
}

static void show_genre_list(lv_obj_t * screen, lv_obj_t * list, lv_obj_t * title_label, const char * title,
                            compact_list_fetch_page_cb_t fetch_page, int total) {
    lv_label_set_text(title_label, title);
    screen_title_fit(title_label);
    compact_list_set_paged_provider(list, fetch_page, NULL, total);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    nav_push(screen);
}

void genre_albums_row_click_cb(int index) {
    if (library_rescan_active) return;
    genre_browse_query_t query = genre_browse_snapshot();
    group_row_t row;
    if (metadata_db_get_genre_groups_page(METADATA_DB_GROUP_ALBUM, query.genre, genre_view_artist_kind(query.view),
                                          query.artist, index, 1, &row, NULL) != 1) return;
    open_genre_album(&row);
}

void genre_groups_row_click_cb(int index) {
    if (library_rescan_active) return;
    genre_browse_query_t query = genre_browse_snapshot();
    metadata_db_group_kind_t kind = genre_view_group_kind(query.view);
    group_row_t row;
    if (metadata_db_get_genre_groups_page(kind, query.genre, METADATA_DB_GROUP_ARTIST, NULL,
                                          index, 1, &row, NULL) != 1) return;
    if (kind == METADATA_DB_GROUP_ALBUM) {
        open_genre_album(&row);
        return;
    }
    pthread_mutex_lock(&genre_browse_mutex);
    snprintf(genre_browse_current.artist, sizeof(genre_browse_current.artist), "%s", row.name);
    query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    show_genre_list(genre_albums_screen, genre_albums_list, genre_albums_title_label, row.name,
                    genre_albums_fetch_page, genre_groups_count(&query, true));
}

static void show_genre_groups(const char * genre_name, int view) {
    pthread_mutex_lock(&genre_browse_mutex);
    snprintf(genre_browse_current.genre, sizeof(genre_browse_current.genre), "%s", genre_name);
    genre_browse_current.view = view;
    genre_browse_current.artist[0] = '\0';
    genre_browse_query_t query = genre_browse_current;
    pthread_mutex_unlock(&genre_browse_mutex);
    show_genre_list(genre_groups_screen, genre_groups_list, genre_groups_title_label, genre_name,
                    genre_groups_fetch_page, genre_groups_count(&query, false));
}

/* Library reloads keep these screens; re-read whatever they show. */
void rearm_genre_browse_lists(void) {
    genre_browse_query_t query = genre_browse_snapshot();
    if (!query.genre[0]) return;
    rearm_library_list(genre_groups_list, genre_groups_fetch_page, genre_groups_count(&query, false));
    if (query.artist[0])
        rearm_library_list(genre_albums_list, genre_albums_fetch_page, genre_groups_count(&query, true));
}

lv_obj_t * build_genre_browse_screen(lv_obj_t ** out_list, lv_obj_t ** out_title,
                                            compact_list_click_cb_t on_click) {
    lv_obj_t * scr = build_compact_list_screen(TR("Genres"), generic_back_cb, NULL, 0, on_click, NULL,
                                                out_list, out_title, LIST_ROW_WIDTH_WIDE, true,
                                                gui_theme_accent_style());
    compact_list_set_row_height(*out_list, MUSIC_LIST_ROW_HEIGHT);
    library_thumbnails_bind_list(scr, *out_list,
        on_click == genre_groups_row_click_cb ? LIBRARY_THUMB_LIST_GENRE_GROUPS : LIBRARY_THUMB_LIST_GENRE_ALBUMS);
    finalize_screen_navigation(scr);
    return scr;
}

static void refresh_genre_view_options(void) {
    for (int i = 0; i < GENRE_VIEW_COUNT; i++)
        if (genre_view_options[i])
            lv_obj_set_style_border_width(genre_view_options[i],
                                          current_settings.genre_view_mode == i ? BOARD_SCALE_PX(3) : 0, 0);
}

static void genre_view_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int view = (int) (intptr_t) lv_event_get_user_data(e);
    if (view < 0 || view >= GENRE_VIEW_COUNT || current_settings.genre_view_mode == view) return;
    ++genre_load_generation; /* a songs page still loading was asked for under the old view */
    current_settings.genre_view_mode = view;
    settings_save_async(&current_settings);
    refresh_genre_view_options();
}

static lv_obj_t * build_genre_view_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Display"), &title, &list);
    (void) title;
    static const char * labels[GENRE_VIEW_COUNT] = { N_("All Songs"), N_("Artists"), N_("Album Artist"),
                                                     N_("Albums") };
    for (int i = 0; i < GENRE_VIEW_COUNT; i++) {
        genre_view_options[i] = add_pill_option_row(list, TR(labels[i]), false, genre_view_option_cb,
                                                    (void *) (intptr_t) i);
        lv_obj_add_style(genre_view_options[i], gui_theme_accent_outline_style(), 0);
    }
    refresh_genre_view_options();
    return screen;
}

static void genre_view_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!genre_view_screen) genre_view_screen = build_genre_view_screen();
    if (!genre_view_screen) return;
    refresh_genre_view_options();
    nav_push(genre_view_screen);
}

/* A finger on a genre row starts reading its songs (flat Songs view only; the
 * artist and album views open a list instead). */
static void genre_row_press_cb(int index, bool pressed) {
    press_resolved_clear();
    if (!pressed) {
        if (genre_load_job && genre_load_job->prefetch) {
            genre_load_job->left = true;
            atomic_store_explicit(&genre_load_job->cancel, true, memory_order_relaxed);
            if (genre_load_timer) lv_timer_resume(genre_load_timer); /* reaped there, no join in the touch */
        }
        return;
    }
    int view = current_settings.genre_view_mode;
    if (library_rescan_active || genre_load_job || genre_load_pending[0] || library_async_kind_saturated(&genre_orphans) ||
        (view > GENRE_VIEW_SONGS && view < GENRE_VIEW_COUNT)) return;
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, index, 1, &group) != 1) return;
    press_resolved_set(PRESS_SRC_GENRES, index, &group);
    const char * genre_name = group.genre_name[0] ? group.genre_name : group.name;
    if (!genre_name[0]) return;
    genre_load_request_t * request = calloc(1, sizeof(*request));
    if (!request) return;
    snprintf(request->genre, sizeof(request->genre), "%s", genre_name);
    request->generation = ++genre_load_generation;
    request->catalog_generation = tagcache_generation();
    request->origin = lv_screen_active();
    request->prefetch = true;
    request->prefetch_tick = lv_tick_get();
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!genre_load_timer) genre_load_timer = lv_timer_create(genre_load_poll, 25, NULL);
    if (!genre_load_timer || pthread_create(&genre_load_thread, NULL, genre_load_worker, request) != 0) {
        free(request);
        return;
    }
    genre_load_job = request;
    lv_obj_remove_event_cb(request->origin, genre_origin_unloaded_cb);
    lv_obj_add_event_cb(request->origin, genre_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_timer_resume(genre_load_timer);
}

static void genre_row_click_cb(int index) {
    if (library_rescan_active) return;
    group_row_t group;
    if (!press_resolved_take(PRESS_SRC_GENRES, index, &group) &&
        metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, index, 1, &group) != 1) return;
    const char * genre_name = group.genre_name[0] ? group.genre_name : group.name;
    if (!genre_name[0]) return;
    int view = current_settings.genre_view_mode;
    if (view > GENRE_VIEW_SONGS && view < GENRE_VIEW_COUNT && genre_groups_screen) {
        show_genre_groups(genre_name, view);
        return;
    }
    (void) start_genre_songs_load(genre_name);
}

lv_obj_t * build_genres_screen(void) {
    lv_obj_t * scr = build_compact_list_screen(TR("Genres"), generic_back_cb, NULL, 0, genre_row_click_cb,
                                                NULL, &genres_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, false, NULL);
    compact_list_set_row_height(genres_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(genres_list, genres_fetch_page, NULL, count_genres_from_database());
    compact_list_set_press_cb(genres_list, genre_row_press_cb);
    build_top_right_icon_button(scr, asset_path("settings/playback.png"), genre_view_open_cb);
    finalize_screen_navigation(scr);
    return scr;
}

/* Unlike Favorites/Most Played above, Recently Added is not built through
 * show_group_songs() -- it's the whole library just reordered, potentially
 * far larger than either of those two's own small deliberately-capped
 * result sets, so it needs recently_added_screen's own paged compact_list
 * (build_recently_added_screen()) instead. Same "prebuilt screen, just
 * nav_push it" shape as open_queue_screen(). */
void open_recently_added_screen(void) {
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
        (int) metadata_db_get_song_count());
    songs_play_bar_refresh(recently_added_list, &recent_bar, PLAY_ALL_SOURCE_RECENTLY_ADDED);
    nav_push(recently_added_screen);
}

/* Paged Artists/Album Artist -- see build_artists_screen()'s own comment.
 * offset is a position in the DB's own name-sorted order (ORDER BY artist/
 * album_artist COLLATE NOCASE) -- a row tapped here (artist_row_click_cb()/
 * album_artist_row_click_cb() below) re-resolves that same offset via
 * metadata_db_get_groups_page() itself to get the tapped name, rather than
 * caching anything from this page fetch. */
static int group_fetch_page(metadata_db_group_kind_t kind, int offset, int count,
                            compact_list_page_row_t out_rows[]) {
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(kind, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].artwork_key = albumart_artist_thumbnail_key(rows[i].name);
        snprintf(out_rows[i].artwork_name, sizeof(out_rows[i].artwork_name), "%s", rows[i].name);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "navigation/more.png");
    }
    free(rows);
    return n;
}

int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    return group_fetch_page(METADATA_DB_GROUP_ARTIST, offset, count, out_rows);
}

int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    return group_fetch_page(METADATA_DB_GROUP_ALBUM_ARTIST, offset, count, out_rows);
}

void album_load_bump_generation(void) {
    ++album_load_generation;
}
