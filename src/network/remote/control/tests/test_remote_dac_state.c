#define _POSIX_C_SOURCE 200809L
#include "src/audio/engine/audio.h"
#include "src/core/persistence/settings.h"
#include "src/network/remote/control/remote_control.h"
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

static bool owns_output;
bool __wrap_gui_shell_bt_dac_owns_output(void) { return owns_output; }

/* Synchronous response avoids the unrelated HTTP timeout integration tests. */
static cJSON * status_json(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handle_status_get(sv[1]);
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

int main(void) {
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
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "bluetooth_dac_mode")));
    cJSON_Delete(json);
    owns_output = true;
    assert(!gui_player_local_playback_allowed());
    assert_local_starts_refused();
    gui_external_controls_publish_status(true);
    json = status_json();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "bluetooth_dac_mode")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "playing")));
    cJSON_Delete(json);
    for (int mode = 0; mode < 2; mode++) {
        remote_control_notify_status(true, true, "Local", "Artist", "Album", "music/test.wav",
                                     12, 60, 0.5, 0, true, 0, mode);
        json = status_json();
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "bluetooth_dac_mode")) == mode);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "playing")) == !mode);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "paused")) == !mode);
        assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "title")), mode ? "" : "Local") == 0);
        assert(cJSON_GetObjectItem(json, "position_seconds")->valueint == (mode ? 0 : 12));
        assert(cJSON_GetObjectItem(json, "duration_seconds")->valueint == (mode ? 0 : 60));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "favorite")) == !mode);
        cJSON_Delete(json);
    }
    puts("remote DAC state, armed-idle permission and owned local start refusal passed");
    return 0;
}
