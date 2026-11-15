#include "src/ui/application/gui.h"
#include "src/ui/network/subsonic/gui_subsonic_internal.h"
#include "src/ui/library/browse/gui_library.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
bool __wrap_gui_navigation_transition_in_progress(void) { return false; }
static pthread_t ui;
static bool run_worker, offline;
static int album_opens, artist_opens, errors;
static atomic_bool album_saved;
static subsonic_browse_kind_t requested_kind;
static atomic_int song_reads, artist_reads;
void __wrap_show_error_toast(const char * text) { assert(text && text[0]); ++errors; }
bool __wrap_wifi_get_status(int * level) { if (level) *level = 3; return !offline; }
void __real_gui_subsonic_global_open_artist(const char *, const char *);
void __wrap_gui_subsonic_global_open_artist(const char * id, const char * name) {
    assert(!strcmp(id, "artist-id") && !strcmp(name, "Shared name")); ++artist_opens;
    if (offline) __real_gui_subsonic_global_open_artist(id, name);
}
void __wrap_gui_subsonic_global_open_album(const char * id, const char * name) {
    assert(!strcmp(id, "album-id") && !strcmp(name, "Shared name")); ++album_opens;
}
void __real_start_subsonic_browse(subsonic_browse_kind_t, const char *, const char *);
void __wrap_start_subsonic_browse(subsonic_browse_kind_t kind, const char * id, const char * title) {
    requested_kind = kind;
    if (run_worker || offline) __real_start_subsonic_browse(kind, id, title);
}
bool __wrap_subsonic_get_song(const subsonic_server_t * server, const char * id, subsonic_song_t * song, http_cancel_token_t * token) {
    (void)server; (void)token;
    assert(!pthread_equal(ui, pthread_self()) && !strcmp(id, "song-id"));
    memset(song, 0, sizeof(*song));
    strcpy(song->artist_id, "artist-id"); strcpy(song->artist, "Shared name");
    strcpy(song->album_id, "album-id"); strcpy(song->album, "Shared name");
    atomic_fetch_add(&song_reads, 1); return true;
}
bool __wrap_subsonic_get_artist_albums(const subsonic_server_t * server, const char * id, subsonic_album_t ** albums, int * count, http_cancel_token_t * token) {
    (void)server; (void)token;
    assert(!pthread_equal(ui, pthread_self()) && !strcmp(id, "artist-id"));
    *albums = NULL; *count = 0; atomic_fetch_add(&artist_reads, 1); return false;
}
bool __wrap_subsonic_get_album_songs(const subsonic_server_t * server, const char * id,
                                     subsonic_song_t ** songs, int * count, http_cancel_token_t * token) {
    (void)server; (void)token;
    assert(!pthread_equal(ui, pthread_self()) && !strcmp(id, "album-id"));
    *songs = NULL; *count = 0; return true;
}
bool __wrap_subsonic_cache_songs_save(const subsonic_server_t * server, subsonic_list_kind_t kind,
                                      const char * id, const void * songs, int count, const subsonic_cache_entry_t * entry) {
    (void)server; (void)songs; (void)count;
    assert(kind == SUBSONIC_LIST_ALBUM_SONGS && !strcmp(id, "album-id"));
    assert(entry->stamp.last_modified == 42);
    atomic_store(&album_saved, true); return true;
}
bool __wrap_metadata_db_get_song_by_path(const char * path, song_row_t * song) {
    (void)path; (void)song; assert(!"Remote navigation must not query the local database"); return false;
}
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init(); gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    ui = pthread_self();
    subsonic_stream_meta = calloc(1, sizeof(*subsonic_stream_meta)); subsonic_stream_meta_count = 1;
    subsonic_stream_song_meta_t * song = subsonic_stream_meta;
    strcpy(song->url, "https://server.test/stream"); strcpy(song->song_id, "song-id");
    strcpy(song->artist_id, "artist-id"); strcpy(song->album_id, "album-id");
    strcpy(song->artist, "Shared name"); strcpy(song->album, "Shared name");
    subsonic_server_t server = subsonic_server_from_settings();
    subsonic_cache_server_key(&server, song->server_key);
    assert(gui_library_open_now_playing_group(song->url, false));
    assert(gui_library_open_now_playing_group(song->url, true));
    assert(artist_opens == 1 && album_opens == 1);
    offline = true;
    assert(gui_library_open_now_playing_group(song->url, false));
    assert(errors == 1 && !subsonic_browse_active); offline = false;
    song->artist_id[0] = 0; run_worker = true;
    assert(gui_library_open_now_playing_group(song->url, false));
    assert(requested_kind == SUBSONIC_BROWSE_SONG_ARTIST);
    for (int i = 0; i < 2000 && subsonic_browse_active; ++i) { poll_subsonic_browse(); usleep(1000); }
    assert(!subsonic_browse_active && atomic_load(&song_reads) == 1 && atomic_load(&artist_reads) == 1);
    assert(!strcmp(song->artist_id, "artist-id"));
    assert(gui_library_open_now_playing_group(song->url, false));
    assert(atomic_load(&song_reads) == 1);
    song->album_id[0] = 0;
    subsonic_entries[SUBSONIC_LIST_ALBUMS].stamp.last_modified = 42;
    assert(gui_library_open_now_playing_group(song->url, true));
    lv_screen_load(gui_busy_get_screen());
    for (int i = 0; i < 2000 && subsonic_browse_active; ++i) { poll_subsonic_browse(); usleep(1000); }
    assert(!subsonic_browse_active && atomic_load(&album_saved));
    assert(!strcmp(song->album_id, "album-id"));
    assert(gui_library_open_now_playing_group(song->url, true));
    assert(atomic_load(&song_reads) == 2);
    strcpy(song->server_key, "different-server");
    int before = errors;
    assert(gui_library_open_now_playing_group(song->url, true)); assert(errors == before + 1);
    assert(!gui_library_open_now_playing_group("https://radio.test/live", false));
    free(subsonic_stream_meta); subsonic_stream_meta = NULL; subsonic_stream_meta_count = 0;
    puts("Server navigation IDs, missing-ID worker fallback, server guard and offline toast passed");
}
