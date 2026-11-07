#include "gui_subsonic_artist.h"
#include "gui_subsonic_artist_summary.h"
#include "gui_subsonic_covers.h"
#include "../../library/browse/gui_artist_page.h"
#include "../../library/browse/gui_library_internal.h"
#include "../../player/screen/gui_player.h"
#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * artist_list;
static char artist_cover[256];
static gui_artist_summary_t summary;
static gui_artist_revisions_t revisions;
static subsonic_song_t * artist_songs;
static int song_count;
static bool songs_loaded;
typedef enum { ARTIST_SONGS, ARTIST_PLAY, ARTIST_SHUFFLE } artist_action_t;
static artist_action_t pending_action;

static void snapshot(void * source, gui_artist_summary_t * out, gui_artist_revisions_t * rev) {
    (void) source;
    *out = summary;
    *rev = revisions;
}

static bool copy_tile(void * source, int album, uint16_t * pixels, bool * no_art) {
    (void) source;
    return gui_subsonic_covers_copy(subsonic_albums_cache[album].cover_art, pixels, no_art);
}

static bool copy_artist(void * source, uint16_t * pixels, bool * no_art) {
    (void) source;
    return gui_subsonic_covers_copy(artist_cover, pixels, no_art);
}

static bool perform_action(void) {
    subsonic_song_t * copy = malloc(sizeof(*copy) * (size_t) (song_count > 0 ? song_count : 1));
    if (!copy) return false;
    if (song_count) memcpy(copy, artist_songs, sizeof(*copy) * (size_t) song_count);
    if (pending_action == ARTIST_SONGS)
        return subsonic_open_transient_songs(copy, song_count, subsonic_albums_context_artist);
    if (!subsonic_install_transient_songs(copy, song_count, subsonic_albums_context_artist)) return false;
    bool shuffle = pending_action == ARTIST_SHUFFLE;
    gui_player_set_play_mode(collection_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL));
    if (song_count) gui_subsonic_play_from(shuffle ? (int) (lv_tick_get() % (unsigned) song_count) : 0);
    return true;
}

static void request_action(artist_action_t action) {
    if (subsonic_foreground_busy()) return;
    pending_action = action;
    if (!songs_loaded) {
        start_subsonic_browse(SUBSONIC_BROWSE_ARTIST_SONGS, NULL, subsonic_albums_context_artist);
        return;
    }
    if (!perform_action()) show_error_toast(TR("Not enough memory to load from server"));
}

static void play(void) { request_action(ARTIST_PLAY); }
static void shuffle(void) { request_action(ARTIST_SHUFFLE); }
static void songs(void) { request_action(ARTIST_SONGS); }

bool gui_subsonic_artist_accept_songs(subsonic_song_t * items, int count) {
    free(artist_songs);
    artist_songs = items;
    song_count = count;
    songs_loaded = true;
    gui_subsonic_artist_summary_songs(artist_songs, song_count, &summary);
    revisions.summary++;
    revisions.duration++;
    return perform_action();
}

void gui_subsonic_artist_clear(lv_obj_t * list) {
    if (artist_list != list) return;
    gui_artist_page_release(list);
    gui_artist_page_forget(list);
    compact_list_set_header(list, NULL, 0);
    free(artist_songs);
    artist_songs = NULL;
    song_count = 0;
    songs_loaded = false;
    artist_list = NULL;
}

void gui_subsonic_artist_open(lv_obj_t * list, const char * name, const char * cover_id) {
    gui_subsonic_artist_clear(list);
    artist_list = list;
    snprintf(artist_cover, sizeof(artist_cover), "%s", cover_id ? cover_id : "");
    gui_subsonic_artist_summary(subsonic_albums_cache, subsonic_albums_count, &summary);
    revisions = (gui_artist_revisions_t){ .summary = 1, .duration = 1 };
    for (int i = 0; i < summary.mosaic_count; i++)
        summary.mosaic_key[i] = gui_subsonic_covers_key(subsonic_albums_cache[i].cover_art);
    gui_artist_page_params_t params = { .name = name, .albums = summary.albums, .songs = summary.songs,
        .copy_artist = artist_cover[0] ? copy_artist : NULL, .counts_from_summary = true, .snapshot = snapshot, .copy_tile = copy_tile, .on_play = play, .on_shuffle = shuffle, .on_songs = songs };
    gui_artist_page_build(list, &params);
}

void gui_subsonic_artist_poll(void) {
    if (artist_list) gui_artist_page_poll(artist_list, lv_screen_active() == subsonic_albums_screen);
}
