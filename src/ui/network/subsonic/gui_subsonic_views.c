#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_covers.h"
#include "gui_subsonic_sections.h"
#include "../../library/library_az_index.h"
#include "../../library/library_az_table.h"
#include "../../library/library_search.h"
#include "gui_subsonic_cover_flow.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../assets/assets.h"
#include "../../../hardware/device/device_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static subsonic_artist_t * subsonic_artists_cache = NULL;

/* What the Artists screen shows: the cache above, or a borrowed list (favorites). */
static const subsonic_artist_t * subsonic_artists_view = NULL;
static int subsonic_artists_count = 0; /* rows in the view */
static int subsonic_artists_cache_count = 0;

/* A successful empty response is a loaded cache too. */
static bool subsonic_artists_loaded = false;

/* Separate caches so opening one list never overwrites another. */
static subsonic_album_t * subsonic_all_albums_cache = NULL;
static int subsonic_all_albums_count = 0;
static bool subsonic_all_albums_loaded = false;

static subsonic_album_t * subsonic_artist_albums_cache = NULL;
static int subsonic_artist_albums_count = 0;

/* What the Albums screen shows right now: an alias of one of the two above. */
const subsonic_album_t * subsonic_albums_cache = NULL;
int subsonic_albums_count = 0;
static bool subsonic_albums_is_all = false; /* the full library, not one artist's albums */

subsonic_song_t * subsonic_songs_cache = NULL;

int subsonic_songs_count = 0;

subsonic_playlist_t * subsonic_playlists_cache = NULL;

/* "N songs" per playlist, parallel to the cache: the rows borrow these. */
#define SUBSONIC_PLAYLIST_SUBTITLE_SIZE 24
typedef char subsonic_playlist_subtitle_t[SUBSONIC_PLAYLIST_SUBTITLE_SIZE];
static subsonic_playlist_subtitle_t * subsonic_playlist_subtitles = NULL;

int subsonic_playlists_count = 0;

static bool subsonic_playlists_loaded = false;

/* Header refresh action of the Artists, Albums and Playlists screens. */
lv_obj_t * subsonic_refresh_buttons[SUBSONIC_LIST_KINDS];

lv_obj_t * subsonic_artists_screen;

static lv_obj_t * subsonic_artists_title_label;

static lv_obj_t * subsonic_artists_list;


lv_obj_t * subsonic_albums_screen;

static lv_obj_t * subsonic_albums_title_label;

static lv_obj_t * subsonic_albums_list;

static lv_obj_t * subsonic_albums_download_btn;

lv_obj_t * subsonic_songs_screen;

lv_obj_t * subsonic_songs_title_label;

static lv_obj_t * subsonic_songs_list;

static lv_obj_t * subsonic_songs_download_btn;

lv_obj_t * subsonic_playlists_screen;

static lv_obj_t * subsonic_playlists_title_label;

static lv_obj_t * subsonic_playlists_list;

char subsonic_albums_context_artist[128] = "";

bool subsonic_songs_context_is_playlist = false;

char subsonic_songs_context_playlist_name[128] = "";



/* The row of each list: label only for songs, label, subtitle and cover for the
 * others. Text is borrowed from the caches, which outlive the rows. */

























/* Fills a virtualised list: only the visible rows exist as widgets, so a
 * library or a 2000-song playlist stays cheap. Text is borrowed from the
 * caller's cache. A cover list first drops cover work for the rows it replaces. */



















/* Frees every browse array and forgets all views and counts. The lists must
 * not be drawing from them any more (emptied first, or already deleted). */
void subsonic_reset_browse_data(void);




























/* Drops every browse cache (new connection). Lists are emptied first because
 * they borrow label text from the caches. */












static const char * subsonic_artist_label_of(int i) { return subsonic_artists_view[i].name; }
static compact_list_item_t subsonic_artist_item_of(int i) {
    const subsonic_artist_t * artist = &subsonic_artists_view[i];
    return (compact_list_item_t){ .label = artist->name, .artwork_key = gui_subsonic_covers_key(artist->cover_art),
                                  .artwork_name = artist->cover_art };
}

static compact_list_item_t subsonic_album_item_of(int i) {
    const subsonic_album_t * album = &subsonic_albums_cache[i];
    return (compact_list_item_t){ .label = album->name, .subtitle = album->artist,
                                  .artwork_key = gui_subsonic_covers_key(album->cover_art),
                                  .artwork_name = album->cover_art };
}

static compact_list_item_t subsonic_playlist_item_of(int i) {
    const subsonic_playlist_t * playlist = &subsonic_playlists_cache[i];
    return (compact_list_item_t){ .label = playlist->name, .subtitle = subsonic_playlist_subtitles[i],
                                  .trailing_asset = "navigation/chevron-right.png",
                                  .artwork_key = gui_subsonic_covers_key(playlist->cover_art),
                                  .artwork_name = playlist->cover_art };
}

static compact_list_item_t subsonic_song_item_of(int i) {
    return (compact_list_item_t){ .label = subsonic_songs_cache[i].title };
}

static bool subsonic_set_list_items(lv_obj_t * list, int count, library_search_item_of_t item_of, bool keep_scroll) {
    compact_list_item_t * items = malloc(sizeof(*items) * (size_t) (count > 0 ? count : 1));
    if (!items) return false;
    if (list != subsonic_songs_list) {
        subsonic_server_t server = subsonic_server_from_settings();
        gui_subsonic_covers_set_server(&server);
        gui_subsonic_covers_reset();
    }
    for (int i = 0; i < count; i++) items[i] = item_of(i);
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    compact_list_set_items(list, items, count);
    free(items);
    if (keep_scroll) { /* a refresh replaces the rows under the reader's thumb */
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
    }
    return true;
}

void subsonic_views_clear(void) {
    compact_list_set_items(subsonic_artists_list, NULL, 0);
    compact_list_set_items(subsonic_albums_list, NULL, 0);
    compact_list_set_items(subsonic_playlists_list, NULL, 0);
    /* The screens that borrowed these labels are gone. */
    subsonic_reset_browse_data();
}

void subsonic_reset_browse_data(void) {
    free(subsonic_artists_cache);
    free(subsonic_all_albums_cache);
    free(subsonic_artist_albums_cache);
    free(subsonic_playlists_cache);
    free(subsonic_playlist_subtitles);
    subsonic_artists_cache = NULL;
    subsonic_artists_view = NULL;
    subsonic_all_albums_cache = NULL;
    subsonic_artist_albums_cache = NULL;
    subsonic_playlists_cache = NULL;
    subsonic_playlist_subtitles = NULL;
    subsonic_albums_cache = NULL;
    subsonic_albums_is_all = false;
    subsonic_artists_count = subsonic_artists_cache_count = subsonic_all_albums_count = subsonic_artist_albums_count = 0;
    subsonic_albums_count = subsonic_playlists_count = 0;
    subsonic_artists_loaded = subsonic_all_albums_loaded = subsonic_playlists_loaded = false;
    gui_subsonic_favorites_clear();
}

static const char * subsonic_album_label_of(int i) { return subsonic_albums_cache[i].name; }
/* A-Z strip of the Artists and Albums lists: the server sends them alphabetical.
 * Another artist's albums are not in name order, so they get no strip. */
static const char * subsonic_az_artist_name(int i, void * ctx) { (void) ctx; return subsonic_artist_label_of(i); }
static const char * subsonic_az_album_name(int i, void * ctx) { (void) ctx; return subsonic_album_label_of(i); }
static void subsonic_az_artists_table(int jump[27]) {
    library_az_table_build(subsonic_az_artist_name, NULL, subsonic_artists_count, jump);
}

static void subsonic_az_albums_table(int jump[27]) {
    library_az_table_build(subsonic_az_album_name, NULL, subsonic_albums_count, jump);
}

/* Search shows a filtered list, which the full-list table does not match. */
static bool subsonic_az_artists_allowed(void) {
    return subsonic_artists_view == subsonic_artists_cache && !library_search_is_active_on_list(subsonic_artists_list);
}

static bool subsonic_az_albums_allowed(void) {
    return subsonic_albums_is_all && !library_search_is_active_on_list(subsonic_albums_list);
}

/* Points the shared Albums screen's rows at an albums array. On failure the
 * screen keeps showing what it showed before. */
static bool subsonic_set_albums_view(const subsonic_album_t * albums, int count, bool keep_scroll) {
    const subsonic_album_t * prev = subsonic_albums_cache;
    int prev_count = subsonic_albums_count;
    subsonic_albums_cache = albums;
    subsonic_albums_count = count;
    if (!subsonic_set_list_items(subsonic_albums_list, count, subsonic_album_item_of, keep_scroll)) {
        subsonic_albums_cache = prev;
        subsonic_albums_count = prev_count;
        return false;
    }
    return true;
}

/* Stops the Albums title before the action the current view shows. */
static void subsonic_fit_albums_title(bool artist_view) {
    reserve_title_width_before(subsonic_albums_title_label,
                               artist_view ? subsonic_albums_download_btn : subsonic_refresh_buttons[SUBSONIC_LIST_ALBUMS]);
}

/* Each list title stops before the leftmost header action of its screen. */
static void subsonic_fit_list_titles(void) {
    reserve_title_width_before(subsonic_artists_title_label, subsonic_refresh_buttons[SUBSONIC_LIST_ARTISTS]);
    reserve_title_width_before(subsonic_playlists_title_label, subsonic_refresh_buttons[SUBSONIC_LIST_PLAYLISTS]);
    subsonic_fit_albums_title(false);
}

static void subsonic_set_hidden(lv_obj_t * obj, bool hidden) {
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Shows an albums cache on the shared Albums screen. artist names the artist
 * for the artist mode and is ignored otherwise. */
bool subsonic_show_albums(const subsonic_album_t * albums, int count, const char * title,
                                 subsonic_albums_mode_t mode, const char * artist) {
    if (!subsonic_set_albums_view(albums, count, false)) return false;
    subsonic_albums_is_all = mode == SUBSONIC_ALBUMS_ALL;
    snprintf(subsonic_albums_context_artist, sizeof(subsonic_albums_context_artist), "%s",
             mode == SUBSONIC_ALBUMS_ARTIST ? artist : "");
    subsonic_set_hidden(subsonic_albums_download_btn, mode != SUBSONIC_ALBUMS_ARTIST);
    subsonic_set_hidden(subsonic_refresh_buttons[SUBSONIC_LIST_ALBUMS], mode != SUBSONIC_ALBUMS_ALL);
    lv_label_set_text(subsonic_albums_title_label, title);
    subsonic_fit_albums_title(mode == SUBSONIC_ALBUMS_ARTIST);
    nav_push(subsonic_albums_screen);
    return true;
}

/* Points the Songs screen at an album or playlist, forgetting what the other
 * one had in flight and what its file said. */
void subsonic_set_songs_context(subsonic_list_kind_t kind, const char * id, const char * title) {
    subsonic_sync_songs_context(kind, id);
    subsonic_songs_context_is_playlist = kind == SUBSONIC_LIST_PLAYLIST_SONGS;
    if (subsonic_songs_context_is_playlist)
        snprintf(subsonic_songs_context_playlist_name, sizeof(subsonic_songs_context_playlist_name), "%s", title);
    lv_label_set_text(subsonic_songs_title_label, title);
    lv_obj_clear_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
}

static const subsonic_album_t * subsonic_find_album(const subsonic_album_t * albums, int count) {
    for (int i = 0; i < count; i++) if (strcmp(albums[i].id, subsonic_songs_id) == 0) return &albums[i];
    return NULL;
}

/* The album the Songs screen shows, as its list of albums knows it (year and
 * genre are not on a song); the first song fills in what that list lacks. */
void subsonic_songs_album_info(const subsonic_server_t * server, gui_subsonic_album_info_t * info) {
    const subsonic_song_t * first = &subsonic_songs_cache[0];
    *info = (gui_subsonic_album_info_t){ .server = server, .title = first->album, .cover_id = first->cover_art,
                                         .artist = first->album_artist[0] ? first->album_artist : first->artist };
    const subsonic_album_t * album = subsonic_find_album(subsonic_albums_cache, subsonic_albums_count);
    if (!album) album = subsonic_find_album(subsonic_all_albums_cache, subsonic_all_albums_count);
    if (!album) return;
    if (album->name) info->title = album->name;
    if (album->artist && album->artist[0]) info->artist = album->artist;
    if (album->cover_art && album->cover_art[0]) info->cover_id = album->cover_art;
    info->genre = album->genre;
    info->year = album->year;
}

/* Shows the songs cache: an album that fits gets the album page, anything else
 * the compact list. False when the compact list could not be filled. */
static bool subsonic_show_songs(bool keep_scroll) {
    if (subsonic_songs_kind == SUBSONIC_LIST_ALBUM_SONGS && gui_subsonic_album_fits(subsonic_songs_count)) {
        subsonic_server_t server = subsonic_server_from_settings();
        gui_subsonic_album_info_t info;
        subsonic_songs_album_info(&server, &info);
        compact_list_set_items(subsonic_songs_list, NULL, 0);
        gui_subsonic_songs_bar_set(subsonic_songs_list, 0);
        lv_obj_add_flag(subsonic_songs_list, LV_OBJ_FLAG_HIDDEN);
        gui_subsonic_album_show(&info, subsonic_songs_cache, subsonic_songs_count, keep_scroll);
        return true;
    }
    if (!subsonic_set_list_items(subsonic_songs_list, subsonic_songs_count, subsonic_song_item_of, keep_scroll))
        return false;
    gui_subsonic_album_clear(); /* first: it resets the bar's track count */
    gui_subsonic_songs_bar_set(subsonic_songs_list, subsonic_songs_count);
    lv_obj_remove_flag(subsonic_songs_list, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* Empties the Songs screen: the previous tracks must not stay tappable. */
void subsonic_clear_songs(void) {
    compact_list_set_items(subsonic_songs_list, NULL, 0);
    gui_subsonic_songs_bar_set(subsonic_songs_list, 0);
    gui_subsonic_album_clear();
    lv_obj_remove_flag(subsonic_songs_list, LV_OBJ_FLAG_HIDDEN);
    free(subsonic_songs_cache);
    subsonic_songs_cache = NULL;
    subsonic_songs_count = 0;
}

/* Installs songs as the cache behind the Songs screen. Takes ownership of items;
 * on failure frees them and keeps the old list. */
bool subsonic_install_songs(subsonic_song_t * items, int count, bool keep_scroll) {
    subsonic_song_t * old = subsonic_songs_cache;
    int old_count = subsonic_songs_count;
    subsonic_songs_cache = items;
    subsonic_songs_count = count;
    if (!subsonic_show_songs(keep_scroll)) {
        subsonic_songs_cache = old;
        subsonic_songs_count = old_count;
        free(items);
        return false;
    }
    free(old);
    return true;
}

/* The install helpers take ownership of items. On failure they free it and keep
 * the old list. */
static subsonic_playlist_subtitle_t * subsonic_make_playlist_subtitles(const subsonic_playlist_t * items, int count) {
    subsonic_playlist_subtitle_t * subtitles = malloc(sizeof(*subtitles) * (size_t) (count > 0 ? count : 1));
    for (int i = 0; subtitles && i < count; i++)
        snprintf(subtitles[i], sizeof(subtitles[i]), TR_N("%d song", "%d songs", items[i].song_count),
                 items[i].song_count);
    return subtitles;
}

bool subsonic_install_playlists(subsonic_playlist_t * items, int count, bool keep_scroll) {
    subsonic_playlist_t * old = subsonic_playlists_cache;
    subsonic_playlist_subtitle_t * old_subtitles = subsonic_playlist_subtitles;
    int old_count = subsonic_playlists_count;
    subsonic_playlist_subtitle_t * subtitles = subsonic_make_playlist_subtitles(items, count);
    subsonic_playlists_cache = items;
    subsonic_playlist_subtitles = subtitles;
    subsonic_playlists_count = count;
    if (!subtitles || !subsonic_set_list_items(subsonic_playlists_list, count, subsonic_playlist_item_of, keep_scroll)) {
        subsonic_playlists_cache = old;
        subsonic_playlist_subtitles = old_subtitles;
        subsonic_playlists_count = old_count;
        free(subtitles);
        free(items);
        return false;
    }
    free(old);
    free(old_subtitles);
    subsonic_playlists_loaded = true;
    return true;
}

/* Points the Artists screen's rows at an artists array. On failure the screen
 * keeps showing what it showed before. */
static bool subsonic_set_artists_view(const subsonic_artist_t * artists, int count, bool keep_scroll) {
    const subsonic_artist_t * prev = subsonic_artists_view;
    int prev_count = subsonic_artists_count;
    subsonic_artists_view = artists;
    subsonic_artists_count = count;
    if (!subsonic_set_list_items(subsonic_artists_list, count, subsonic_artist_item_of, keep_scroll)) {
        subsonic_artists_view = prev;
        subsonic_artists_count = prev_count;
        return false;
    }
    return true;
}

/* The full artists list. A favorites view on the screen is left alone. */
bool subsonic_install_artists(subsonic_artist_t * items, int count, bool keep_scroll) {
    if (subsonic_artists_view == subsonic_artists_cache && !subsonic_set_artists_view(items, count, keep_scroll)) {
        free(items);
        return false;
    }
    free(subsonic_artists_cache);
    subsonic_artists_cache = items;
    subsonic_artists_cache_count = count;
    subsonic_artists_loaded = true;
    return true;
}

/* The full-library albums. The Albums screen only changes when it shows them. */
bool subsonic_install_all_albums(subsonic_album_t * items, int count, bool keep_scroll) {
    if (subsonic_albums_is_all && !subsonic_set_albums_view(items, count, keep_scroll)) {
        free(items);
        return false;
    }
    free(subsonic_all_albums_cache);
    subsonic_all_albums_cache = items;
    subsonic_all_albums_count = count;
    subsonic_all_albums_loaded = true;
    return true;
}

void subsonic_show_list(subsonic_list_kind_t kind) {
    if (kind == SUBSONIC_LIST_ALBUMS) {
        if (!subsonic_show_albums(subsonic_all_albums_cache, subsonic_all_albums_count, TR("Albums"),
                                  SUBSONIC_ALBUMS_ALL, NULL))
            show_error_toast(TR("Not enough memory to load from server"));
        return;
    }
    nav_push(kind == SUBSONIC_LIST_ARTISTS ? subsonic_artists_screen : subsonic_playlists_screen);
}

/* The Artists screen goes back to the full list after a favorites view. */
void subsonic_restore_artists_view(void) {
    if (subsonic_artists_view != subsonic_artists_cache)
        (void) subsonic_set_artists_view(subsonic_artists_cache, subsonic_artists_cache_count, false);
    subsonic_set_hidden(subsonic_refresh_buttons[SUBSONIC_LIST_ARTISTS], false);
}

void subsonic_keep_albums(bool all, subsonic_album_t * fresh, int count) {
    subsonic_album_t ** cache = all ? &subsonic_all_albums_cache : &subsonic_artist_albums_cache;
    int * cached_count = all ? &subsonic_all_albums_count : &subsonic_artist_albums_count;
    free(*cache);
    *cache = fresh;
    *cached_count = count;
    subsonic_all_albums_loaded = subsonic_all_albums_loaded || all;
}

static void subsonic_album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ALBUMS, index);

    if (index < 0 || index >= subsonic_albums_count) return;
    subsonic_open_songs(SUBSONIC_LIST_ALBUM_SONGS, subsonic_albums_cache[index].id, subsonic_albums_cache[index].name);
}

static void subsonic_artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, index);

    if (index < 0 || index >= subsonic_artists_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_ARTIST_ALBUMS, subsonic_artists_view[index].id,
                          subsonic_artists_view[index].name);
}

static void subsonic_playlist_row_click_cb(int index) {
    if (index < 0 || index >= subsonic_playlists_count) return;
    subsonic_open_songs(SUBSONIC_LIST_PLAYLIST_SONGS, subsonic_playlists_cache[index].id,
                        subsonic_playlists_cache[index].name);
}

bool subsonic_list_loaded(subsonic_list_kind_t kind) {
    if (kind == SUBSONIC_LIST_ARTISTS) return subsonic_artists_loaded;
    return kind == SUBSONIC_LIST_ALBUMS ? subsonic_all_albums_loaded : subsonic_playlists_loaded;
}

/* Favorites on the shared screens: the arrays stay owned by the favorites chooser. */
void gui_subsonic_show_artists_view(const subsonic_artist_t * artists, int count) {
    if (!subsonic_set_artists_view(artists, count, false)) {
        show_error_toast(TR("Not enough memory to load from server"));
        return;
    }
    subsonic_set_hidden(subsonic_refresh_buttons[SUBSONIC_LIST_ARTISTS], true);
    nav_push(subsonic_artists_screen);
}

void gui_subsonic_show_albums_view(const subsonic_album_t * albums, int count, const char * title) {
    if (!subsonic_show_albums(albums, count, title, SUBSONIC_ALBUMS_LIST, NULL))
        show_error_toast(TR("Not enough memory to load from server"));
}

void gui_subsonic_show_songs_copy(const subsonic_song_t * songs, int count, const char * title) {
    subsonic_song_t * copy = malloc(sizeof(*copy) * (size_t) (count > 0 ? count : 1));
    if (copy && count > 0) memcpy(copy, songs, sizeof(*copy) * (size_t) count);
    if (!copy || !subsonic_open_transient_songs(copy, count, title))
        show_error_toast(TR("Not enough memory to load from server"));
}

/* Empties a list and its view without allocating, so it cannot fail. The next
 * open of the full list rebuilds the rows. */
void gui_subsonic_release_views(const subsonic_artist_t * artists, const subsonic_album_t * albums) {
    if (artists && subsonic_artists_view == artists) {
        compact_list_set_items(subsonic_artists_list, NULL, 0);
        gui_subsonic_covers_reset();
        subsonic_artists_view = NULL;
        subsonic_artists_count = 0;
    }
    if (albums && subsonic_albums_cache == albums) {
        compact_list_set_items(subsonic_albums_list, NULL, 0);
        gui_subsonic_covers_reset();
        subsonic_albums_cache = NULL;
        subsonic_albums_count = 0;
    }
}

void subsonic_open_cover_flow(void) {
    subsonic_server_t server = subsonic_server_from_settings();
    gui_subsonic_cover_flow_open(&server, subsonic_all_albums_cache, subsonic_all_albums_count,
                                 subsonic_cover_flow_open_album);
}

/* Breeze download glyph where available; the stock art keeps its accent. */
static void subsonic_set_download_icon(lv_obj_t * image) {
    const char * asset = asset_path("stream_media/download.png");
    const char * breeze = titlebar_breeze_icon(asset);
    lv_image_set_src(image, breeze ? breeze : asset);
    if (breeze) {
        style_titlebar_breeze_icon(image);
        return;
    }
    lv_obj_set_style_image_recolor(image, accent_lv_color(), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

/* Artists, Albums and Playlists look like the library's lists: tall rows with a cover. */
static void subsonic_setup_cover_lists(void) {
    lv_obj_t * cover_lists[] = { subsonic_artists_list, subsonic_albums_list, subsonic_playlists_list };
    for (size_t i = 0; i < sizeof(cover_lists) / sizeof(cover_lists[0]); i++) {
        compact_list_set_row_height(cover_lists[i], MUSIC_LIST_ROW_HEIGHT);
        gui_subsonic_covers_bind(cover_lists[i]);
    }
}

void subsonic_delete_browse_screens(void) {
    memset(subsonic_refresh_buttons, 0, sizeof(subsonic_refresh_buttons)); /* children of the screens deleted below */
    library_az_index_unregister(subsonic_artists_screen);
    library_az_index_unregister(subsonic_albums_screen);
    if (subsonic_artists_screen) { lv_obj_delete(subsonic_artists_screen); subsonic_artists_screen = NULL; }
    if (subsonic_albums_screen) { lv_obj_delete(subsonic_albums_screen); subsonic_albums_screen = NULL; }
    gui_subsonic_album_teardown();
    if (subsonic_songs_screen) { lv_obj_delete(subsonic_songs_screen); subsonic_songs_screen = NULL; }
    if (subsonic_playlists_screen) { lv_obj_delete(subsonic_playlists_screen); subsonic_playlists_screen = NULL; }
}

void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, const char * (*name_of)(int), const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind, compact_list_fetch_page_cb_t restore_fetch_page);


static lv_obj_t * subsonic_build_download_button(lv_obj_t * screen, int offset, lv_event_cb_t cb) {
    lv_obj_t * button = lv_image_create(screen);
    subsonic_set_download_icon(button);
    align_screen_header_action(button, offset);
    lv_obj_set_ext_click_area(button, BOARD_SCALE_PX(16));
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    return button;
}

void gui_subsonic_views_build(void) {
    subsonic_artists_screen = build_compact_list_screen(TR("Artists"), generic_back_cb, NULL, 0, subsonic_artist_row_click_cb,
                                                          NULL, &subsonic_artists_list, &subsonic_artists_title_label,
                                                          LIST_ROW_WIDTH_WIDE, false, NULL);
    subsonic_albums_screen = build_compact_list_screen(TR("Albums"), generic_back_cb, NULL, 0, subsonic_album_row_click_cb,
                                                         NULL, &subsonic_albums_list, &subsonic_albums_title_label,
                                                         LIST_ROW_WIDTH_WIDE, false, NULL);
    /* Finalize navigation handlers and gesture support for swipe-back navigation. */
    finalize_screen_navigation(subsonic_artists_screen);
    finalize_screen_navigation(subsonic_albums_screen);
    subsonic_songs_screen = build_compact_list_screen(TR("Songs"), generic_back_cb, NULL, 0, gui_subsonic_play_from,
                                                        NULL, &subsonic_songs_list, &subsonic_songs_title_label,
                                                        LIST_ROW_WIDTH_WIDE, false, NULL);
    gui_subsonic_album_list_build(subsonic_songs_screen); /* shown instead of the compact list for an album */
    lv_obj_add_event_cb(subsonic_songs_screen, gui_subsonic_album_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(subsonic_songs_screen, gui_subsonic_album_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    subsonic_playlists_screen = build_compact_list_screen(TR("Playlists"), generic_back_cb, NULL, 0,
                                                            subsonic_playlist_row_click_cb, NULL, &subsonic_playlists_list,
                                                            &subsonic_playlists_title_label, LIST_ROW_WIDTH_WIDE, false, NULL);
    finalize_screen_navigation(subsonic_songs_screen);
    finalize_screen_navigation(subsonic_playlists_screen);
    subsonic_setup_cover_lists();
    library_az_index_register_table(subsonic_artists_screen, subsonic_artists_list, subsonic_az_artists_table,
                                    subsonic_az_artists_allowed);
    library_az_index_register_table(subsonic_albums_screen, subsonic_albums_list, subsonic_az_albums_table,
                                    subsonic_az_albums_allowed);

    subsonic_albums_download_btn = subsonic_build_download_button(subsonic_albums_screen, BOARD_SCALE_PX(87),
                                                                  gui_subsonic_download_artist_btn_cb);
    subsonic_songs_download_btn = subsonic_build_download_button(subsonic_songs_screen, BOARD_SCALE_PX(20),
                                                                 gui_subsonic_download_songs_btn_cb);
    reserve_title_width_before(subsonic_songs_title_label, subsonic_songs_download_btn);
}

void gui_subsonic_views_build_headers(void) {
    lv_obj_t * list_screens[SUBSONIC_LIST_KINDS] = { subsonic_artists_screen, subsonic_albums_screen,
                                                      subsonic_playlists_screen };
    for (int kind = 0; kind < SUBSONIC_LIST_KINDS; kind++) {
        subsonic_refresh_buttons[kind] = build_header_refresh_action(list_screens[kind], subsonic_refresh_button_cb);
        if (kind != SUBSONIC_LIST_PLAYLISTS) /* the search icon holds the right edge: refresh sits beside it */
            align_screen_header_action(subsonic_refresh_buttons[kind], HEADER_BUTTON_WIDTH);
    }
    lv_obj_add_flag(subsonic_refresh_buttons[SUBSONIC_LIST_ALBUMS], LV_OBJ_FLAG_HIDDEN); /* shown for the full library */

    register_search(SEARCH_BINDING_SUBSONIC_ARTISTS, subsonic_artists_screen, subsonic_artists_list, subsonic_artist_label_of,
                     &subsonic_artists_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
    register_search(SEARCH_BINDING_SUBSONIC_ALBUMS, subsonic_albums_screen, subsonic_albums_list, subsonic_album_label_of,
                     &subsonic_albums_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
    library_search_set_item_of(SEARCH_BINDING_SUBSONIC_ARTISTS, subsonic_artist_item_of);
    library_search_set_item_of(SEARCH_BINDING_SUBSONIC_ALBUMS, subsonic_album_item_of);
    subsonic_fit_list_titles(); /* register_search widened the titles to its own icon */
}
