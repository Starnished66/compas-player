#include "gui_subsonic_sections.h"
#include "gui_subsonic_genres.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include <stdlib.h>
#include <string.h>

#define CHOOSER_MAX_ROWS 8

static lv_obj_t * albums_chooser;
static lv_obj_t * favorites_chooser;
static subsonic_starred_t starred_items;

/* Row 0 of the Albums chooser is the full library; the others are server lists. */
static const char * const album_list_types[] = { NULL, "newest", "recent", "frequent", "highest", "random" };
#define ALBUM_LIST_COUNT ((int) (sizeof(album_list_types) / sizeof(album_list_types[0])))
static const char * album_list_labels[ALBUM_LIST_COUNT];

/* A screen of plain chevron rows; click reads the row number from the event data. */
static lv_obj_t * build_chooser(const char * title, const char * const * labels, int count, lv_event_cb_t click) {
    pill_list_item_t rows[CHOOSER_MAX_ROWS] = { 0 };
    for (int i = 0; i < count && i < CHOOSER_MAX_ROWS; i++)
        rows[i] = (pill_list_item_t){ labels[i], PILL_ACCESSORY_CHEVRON, false, click, NULL, (void *) (intptr_t) i };
    lv_obj_t * scr = build_pill_list_screen(title, generic_back_cb, rows, count, gui_theme_accent_style(),
                                            GUI_ROW_GAP, 100);
    if (scr) finalize_screen_navigation(scr);
    return scr;
}

static void albums_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= ALBUM_LIST_COUNT) return;
    if (album_list_types[index]) gui_subsonic_browse_album_list(album_list_types[index], album_list_labels[index]);
    else gui_subsonic_open_all_albums();
}

static void favorites_pick_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    switch ((int) (intptr_t) lv_event_get_user_data(e)) {
        case 0: gui_subsonic_show_artists_view(starred_items.artists, starred_items.artist_count); break;
        case 1: gui_subsonic_show_albums_view(starred_items.albums, starred_items.album_count, TR("Albums")); break;
        case 2: gui_subsonic_show_songs_copy(starred_items.songs, starred_items.song_count, TR("Songs")); break;
    }
}

void gui_subsonic_albums_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(albums_chooser);
}

void gui_subsonic_genres_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_subsonic_browse_genres();
}

void gui_subsonic_random_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_subsonic_browse_random_songs();
}

void gui_subsonic_favorites_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_subsonic_browse_starred();
}

static void genre_pick(const char * name) { gui_subsonic_browse_album_list("byGenre", name); }

void gui_subsonic_sections_init(void) {
    album_list_labels[0] = TR("All Albums");
    album_list_labels[1] = TR_C("albums", "Recently Added");
    album_list_labels[2] = TR_C("albums", "Recently Played");
    album_list_labels[3] = TR_C("albums", "Most Played");
    album_list_labels[4] = TR("Top Rated");
    album_list_labels[5] = TR("Random");
    albums_chooser = build_chooser(TR("Albums"), album_list_labels, ALBUM_LIST_COUNT, albums_pick_cb);
    const char * favorites_labels[] = { TR("Artists"), TR("Albums"), TR("Songs") };
    favorites_chooser = build_chooser(TR("Favorites"), favorites_labels, 3, favorites_pick_cb);
    gui_subsonic_genres_init(genre_pick);
}

void gui_subsonic_starred_free(subsonic_starred_t * starred) {
    free(starred->artists);
    free(starred->albums);
    free(starred->songs);
    memset(starred, 0, sizeof(*starred));
}

void gui_subsonic_favorites_clear(void) {
    gui_subsonic_release_views(starred_items.artists, starred_items.albums);
    gui_subsonic_starred_free(&starred_items);
}

void gui_subsonic_favorites_show(subsonic_starred_t * starred) {
    gui_subsonic_favorites_clear();
    starred_items = *starred;
    memset(starred, 0, sizeof(*starred));
    nav_push(favorites_chooser);
}

void gui_subsonic_sections_teardown(void) {
    gui_subsonic_favorites_clear();
    gui_subsonic_genres_teardown();
    if (albums_chooser) lv_obj_delete(albums_chooser);
    if (favorites_chooser) lv_obj_delete(favorites_chooser);
    albums_chooser = favorites_chooser = NULL;
}
