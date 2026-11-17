#define _POSIX_C_SOURCE 200809L
#include "src/audio/engine/audio.h"
#include "src/core/persistence/settings.h"
#include "src/library/metadata/db/metadata_db.h"
#include "src/network/remote/control/remote_control.h"
#include "src/hardware/usb/usb_dac_bridge.h"
#include "src/ui/application/gui.h"
#include "src/ui/library/browse/gui_library.h"
#include "src/ui/player/queue/player_queue_model.h"
#include "src/ui/settings/controls/gui_external_controls.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "lua/src/lua.h"
#include "lua/src/lauxlib.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern const luaL_Reg plugin_playback_funcs[];
extern void handle_status_get(int fd);
extern void handle_rating_edit_request(int fd, const char * path);
extern void handle_queue_get(int fd, const char * path);
extern void handle_library_sorting_route(int fd, const char *method, const char *path);

static bool sorting_available;
bool __real_metadata_db_album_sort_available(metadata_db_album_sort_t sort);
bool __wrap_metadata_db_album_sort_available(metadata_db_album_sort_t sort) {
    return sorting_available ? (sort == METADATA_DB_ALBUM_SORT_RATING || (sort >= 0 && sort <= 7))
                             : __real_metadata_db_album_sort_available(sort);
}

static cJSON *sorting_response(const char *method, const char *path, int expected_status) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handle_library_sorting_route(sv[1], method, path);
    close(sv[1]);
    char response[8192] = {0};
    size_t used = 0;
    ssize_t count;
    while ((count = read(sv[0], response + used, sizeof(response) - used - 1)) > 0)
        used += (size_t)count;
    close(sv[0]);
    int status = 0;
    assert(sscanf(response, "HTTP/%*s %d", &status) == 1 && status == expected_status);
    char *body = strstr(response, "\r\n\r\n");
    assert(body);
    cJSON *json = cJSON_Parse(body + 4);
    assert(json);
    return json;
}

static void assert_album_rating_sort_route(void) {
    sorting_available = true;
    remote_control_notify_library_sorting(METADATA_DB_ALBUM_SORT_RATING, 0, 0);
    cJSON *json = sorting_response("GET", "/api/library/sorting", 200);
    cJSON *albums = cJSON_GetObjectItem(json, "albums");
    assert(cJSON_GetObjectItem(albums, "configured")->valueint == 8);
    assert(cJSON_GetObjectItem(albums, "effective")->valueint == 8);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(albums, "reverse")));
    assert(cJSON_IsTrue(cJSON_GetArrayItem(cJSON_GetObjectItem(albums, "available"), 8)));
    cJSON_Delete(json);
    const char *valid[] = {
        "/api/library/sorting?scope=albums&mode=8&reverse=1",
        "/api/library/sorting?scope=albums&mode=8"
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        remote_control_notify_library_sorting(METADATA_DB_ALBUM_SORT_REVERSE, 0, 0);
        cJSON_Delete(sorting_response("POST", valid[i], 202));
        int scope = -1, mode = -1;
        assert(remote_control_consume_library_sorting(&scope, &mode));
        assert(scope == 0 && mode == METADATA_DB_ALBUM_SORT_RATING);
    }
    const char *invalid[] = {
        "/api/library/sorting?scope=artists&mode=8",
        "/api/library/sorting?scope=album_artists&mode=8",
        "/api/library/sorting?scope=albums&mode=12",
        "/api/library/sorting?scope=albums&mode=4"
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        cJSON_Delete(sorting_response("POST", invalid[i], 400));
    remote_control_notify_library_sorting(0, 0, 0);
    sorting_available = false;
}

static bool owns_output;
static bool usb_owns_output;
static bool usb_streaming;
bool __wrap_audio_get_current_format_info(audio_current_format_info_t *out) {
    (void)out;
    return false;
}
void __wrap_usb_dac_bridge_get_stream_info(usb_dac_stream_info_t *out) {
    *out = (usb_dac_stream_info_t){ .streaming = usb_streaming,
        .input_sample_rate = usb_streaming ? 96000 : 0, .input_bit_depth = usb_streaming ? 24 : 0 };
}
bool __wrap_gui_shell_bt_dac_owns_output(void) { return owns_output; }
bool __wrap_gui_shell_usb_dac_owns_output(void) { return usb_owns_output; }

/* Synchronous response avoids the unrelated HTTP timeout integration tests. */
static cJSON * response_json(const char * rating_path) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    if (rating_path) handle_rating_edit_request(sv[1], rating_path);
    else handle_status_get(sv[1]);
    close(sv[1]);
    char response[8192] = {0};
    size_t used = 0;
    ssize_t count;
    while ((count = read(sv[0], response + used, sizeof(response) - used - 1)) > 0)
        used += (size_t) count;
    close(sv[0]);
    char *body = strstr(response, "\r\n\r\n");
    assert(body);
    cJSON *json = cJSON_Parse(body + 4);
    assert(json);
    return json;
}

static cJSON * status_json(void) {
    return response_json(NULL);
}

static cJSON * queue_json(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handle_queue_get(sv[1], "/api/queue?full=1");
    close(sv[1]);
    char response[8192] = {0};
    size_t used = 0;
    ssize_t count;
    while ((count = read(sv[0], response + used, sizeof(response) - used - 1)) > 0)
        used += (size_t) count;
    close(sv[0]);
    char *body = strstr(response, "\r\n\r\n");
    assert(body);
    cJSON *json = cJSON_Parse(body + 4);
    assert(json);
    return json;
}

static void assert_unindexed_queue_titles_and_replacement(void) {
    const char *first[] = {"/music/first-unindexed-track.flac", "/music/second-unindexed-track.mp3"};
    assert(remote_control_sync_queue(first, 2, 0, 1, 10));
    cJSON *json = queue_json();
    cJSON *songs = cJSON_GetObjectItem(json, "songs");
    assert(cJSON_GetArraySize(songs) == 2);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(songs, 0), "title")),
                  "Unknown track") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(songs, 1), "title")),
                  "Unknown track") == 0);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(songs, 0), "index")->valueint == -1);
    cJSON_Delete(json);

    const char *replacement[] = {"/other/replacement.wav"};
    assert(remote_control_sync_queue(replacement, 1, 0, 0, 11));
    json = queue_json();
    songs = cJSON_GetObjectItem(json, "songs");
    assert(cJSON_GetArraySize(songs) == 1);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(songs, 0), "title")),
                  "Unknown track") == 0);
    cJSON_Delete(json);

    const char *url_queue[] = {"https://user:secret@example.test/audio.mp3?token=private"};
    assert(remote_control_sync_queue(url_queue, 1, 0, 0, 12));
    json = queue_json();
    songs = cJSON_GetObjectItem(json, "songs");
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(songs, 0), "title")),
                  "Unknown track") == 0);
    cJSON_Delete(json);
    assert(remote_control_sync_queue(NULL, 0, -1, 0, 12));
}

static void assert_rating_queries(void) {
    const char * values[] = {"0", "1", "2", "3", "4", "5", "6", "-1", "", "1.5", "abc"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), "/api/ratings/set?rating=%s", values[i]);
        cJSON * json = response_json(path);
        assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "error")), "rating_error") == 0);
        /* Valid ratings reach target validation without writing a song. */
        assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "code")),
                      i < 6 ? "invalid_target" : "invalid_rating") == 0);
        cJSON_Delete(json);
    }
    cJSON * json = response_json("/api/ratings/set?current=1");
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "code")), "invalid_rating") == 0);
    cJSON_Delete(json);
}

static void assert_local_starts_refused(void) {
    int queue_count = player_queue_model_count();
    uint64_t revision = player_queue_model_revision();
    lua_State *L = luaL_newstate();
    assert(L);
    lua_newtable(L);
    luaL_setfuncs(L, plugin_playback_funcs, 0);
    lua_getfield(L, -1, "audio_loop_start");
    lua_pushstring(L, "music/test.wav");
    assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
    assert(!lua_toboolean(L, -1));
    lua_close(L);
    assert(!audio_loop_is_active());
    play_remote_control_song("music/test.wav", "", "", "", "");
    remote_track_meta_t remote = {.provider = "blocked", .track_id = "dac"};
    gui_plugin_play_remote_tracks(&remote, 1, 0);
    assert(!remote_track_meta_copy_for_path("remote://blocked/dac", &remote));
    assert(!audio_is_playing());
    assert(player_queue_model_count() == queue_count);
    assert(player_queue_model_revision() == revision);
}

static void check_dac_status_modes(void) {
    cJSON *json;
    for (int mode = 0; mode < 2; mode++) {
        remote_control_notify_status(true, true, "Local", "Artist", "Album", "music/test.wav",
                                     12, 60, 0.5, 0, 3, 0, mode ? REMOTE_DAC_BLUETOOTH : REMOTE_DAC_NONE);
        json = status_json();
        assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "dac_mode")), mode ? "bluetooth" : "none") == 0);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "bluetooth_dac_mode")) == mode);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "playing")) == !mode);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "paused")) == !mode);
        assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "title")), mode ? "" : "Local") == 0);
        assert(cJSON_GetObjectItem(json, "position_seconds")->valueint == (mode ? 0 : 12));
        assert(cJSON_GetObjectItem(json, "duration_seconds")->valueint == (mode ? 0 : 60));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "favorite")) == !mode);
        assert(cJSON_GetObjectItem(json, "rating")->valueint == (mode ? 0 : 3));
        cJSON_Delete(json);
    }
    remote_control_notify_status(true, false, "Local", "Artist", "Album", "music/test.wav",
                                 12, 60, 0.5, 0, 3, 0, REMOTE_DAC_USB);
    json = status_json();
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "dac_mode")), "usb") == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "bluetooth_dac_mode")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "playing")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "favorite")));
    assert(cJSON_GetObjectItem(json, "rating")->valueint == 0);
    assert(cJSON_GetObjectItem(json, "position_seconds")->valueint == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "title")), "") == 0);
    cJSON_Delete(json);
    usb_streaming = true;
    remote_control_notify_status(true, false, "Local", "Artist", "Album", "music/test.wav",
                                 12, 60, 0.5, 0, 3, 0, REMOTE_DAC_USB);
    json = status_json();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "playing")));
    assert(cJSON_GetObjectItem(json, "source_sample_rate")->valueint == 96000);
    assert(cJSON_GetObjectItem(json, "source_bit_depth")->valueint == 24);
    cJSON_Delete(json);
}

int main(void) {
    assert_album_rating_sort_route();
    assert_rating_queries();
    assert_unindexed_queue_titles_and_replacement();
    lv_init();
    assert(ui_fonts_init());
    assert(lv_sdl_window_create(360, 640));
    settings_load(&current_settings);
    current_settings.setup_complete = true;
    audio_init();
    gui_init(360, 640);
    current_settings.bt_dac_mode_enabled = true;
    bt_is_powered_cached = false;
    assert(gui_player_local_playback_allowed());
    gui_external_controls_publish_status(true);
    cJSON *json = status_json();
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "dac_mode")), "none") == 0);
    cJSON_Delete(json);
    owns_output = true;
    assert(!gui_player_local_playback_allowed());
    assert_local_starts_refused();
    gui_external_controls_publish_status(true);
    json = status_json();
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "dac_mode")), "bluetooth") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "bluetooth_dac_mode")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "playing")));
    cJSON_Delete(json);
    owns_output = false;
    usb_owns_output = true;
    gui_external_controls_publish_status(true);
    json = status_json();
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "dac_mode")), "usb") == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "bluetooth_dac_mode")));
    cJSON_Delete(json);
    check_dac_status_modes();
    puts("remote DAC state, armed-idle permission and owned local start refusal passed");
    return 0;
}
