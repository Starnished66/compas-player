#include "src/ui/application/gui.h"
#include "src/ui/player/screen/gui_player_internal.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/network/http/stream/http_stream.h"
#include "src/ui/player/artwork/player_artwork_jobs.h"
#include "src/library/workers/track_metadata_worker.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static icy_metadata_t metadata;
static bool available, remote, subsonic, result_ready;
static const char * path = "http://station.test/live";
static track_metadata_request_t request;
static int metadata_reads;
static int artwork_requests, artwork_clears;
void __wrap_launch_cover_decode_req(cover_decode_request_t request) {
    assert(!request.stream_url[0] && !request.local_track_path[0]); ++artwork_clears;
}
bool __wrap_gui_player_has_active_track(void) { return true; }
const char * __wrap_gui_player_get_current_track_path(void) { return path; }
void __wrap_track_source_resolve(int index, const char * path, track_source_t * out) {
    (void)index; (void)path; memset(out, 0, sizeof(*out));
    out->is_subsonic = subsonic;
    out->is_remote = remote;
}
bool __wrap_http_stream_metadata_for_url(const char * url, icy_metadata_t * out) {
    assert(!strcmp(url, path)); ++metadata_reads;
    *out = metadata; return available;
}
void __wrap_launch_cover_decode_from_url(int index, const char * url, bool verify) {
    (void)index; (void)verify;
    assert(!strcmp(url, metadata.artwork_url)); ++artwork_requests;
}
bool __wrap_player_artwork_jobs_active(void) { return true; }
const char * __wrap_playlist_path_at(int index) { (void)index; return path; }
int __wrap_player_queue_model_current(void) { return 0; }
bool __wrap_track_metadata_submit(int lane, const track_metadata_request_t * value) {
    if (lane == TRACK_METADATA_CURRENT) request = *value;
    return true;
}
bool __wrap_track_metadata_take(int lane, track_metadata_result_t * result) {
    if (lane != TRACK_METADATA_CURRENT || !result_ready) return false;
    *result = (track_metadata_result_t){ .request = request };
    result_ready = false; return true;
}
void __wrap_launch_cover_decode_from_track(int index, const char * track, const char * artist,
                                          const char * album, const char * album_artist) {
    (void)index; (void)track; (void)artist; (void)album; (void)album_artist;
}
void __wrap_gui_lyrics_load_track(int index, const char * track) { (void)index; (void)track; }
static void finish_metadata(const char * title) {
    snprintf(request.metadata.title, sizeof(request.metadata.title), "%s", title);
    request.metadata.has_title = true;
    strcpy(request.metadata.artist, "Station artist"); request.metadata.has_artist = true;
    strcpy(request.metadata.album, "Station album"); request.metadata.has_album = true;
    result_ready = true;
    gui_player_poll_track_metadata();
}
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init(); gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    prepare_track_metadata(0, false, false, false, true);
    finish_metadata("Station");
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Station"));
    available = true; metadata.stream_id = 1;
    icy_metadata_parse("StreamTitle='Artist - First';StreamUrl='https://station.test/cover';", &metadata);
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "First"));
    assert(!strcmp(gui_player_get_now_playing_folder(), "Artist"));
    assert(artwork_requests == 1);
    gui_player_poll_stream_metadata(); assert(artwork_requests == 1);
    icy_metadata_parse("StreamTitle='Second';", &metadata);
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Second"));
    assert(!gui_player_get_now_playing_folder()[0]);
    assert(artwork_clears == 1);
    icy_metadata_parse("StreamTitle='';StreamUrl='';", &metadata);
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Station"));
    assert(!strcmp(gui_player_get_now_playing_folder(), "Station artist"));
    assert(!strcmp(gui_player_get_now_playing_album(), "Station album"));
    /* Reopening the same URL must not promote the old ICY title to station text. */
    icy_metadata_parse("StreamTitle='Old ICY song';", &metadata);
    gui_player_poll_stream_metadata();
    metadata.stream_id = 2;
    icy_metadata_parse("StreamTitle='';", &metadata);
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Station"));
    /* A new station can send ICY before its async metadata result arrives. */
    path = "http://second.test/live";
    metadata.stream_id = 3;
    icy_metadata_parse("StreamTitle='Early ICY song';", &metadata);
    int reads = metadata_reads;
    gui_player_poll_stream_metadata();
    assert(metadata_reads == reads); /* Old queue metadata is not a fallback. */
    prepare_track_metadata(0, false, false, false, true);
    char pending_title[256];
    snprintf(pending_title, sizeof(pending_title), "%s", gui_player_get_now_playing_title());
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Early ICY song"));
    icy_metadata_parse("StreamTitle='';", &metadata);
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), pending_title));
    finish_metadata("Second station");
    gui_player_poll_stream_metadata();
    assert(!strcmp(gui_player_get_now_playing_title(), "Second station"));
    remote = true; reads = metadata_reads;
    strcpy(metadata.title, "Ignored remote title");
    gui_player_poll_stream_metadata(); assert(artwork_requests == 1);
    assert(metadata_reads == reads);
    assert(!strcmp(gui_player_get_now_playing_title(), "Second station"));
    remote = false; subsonic = true; strcpy(metadata.title, "Ignored");
    gui_player_poll_stream_metadata();
    assert(metadata_reads == reads);
    assert(!strcmp(gui_player_get_now_playing_title(), "Second station"));
    puts("Radio metadata, artwork clearing, source guards and station fallback timing passed");
}
