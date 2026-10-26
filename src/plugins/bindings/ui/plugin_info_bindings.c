#include "plugin_info_bindings.h"

#include "lauxlib.h"

#include "../../../core/system/runtime/app_version.h"
#include "../../manager/plugin_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

int l_plugin_api_version(lua_State * L) {
    lua_pushinteger(L, PLUGIN_API_VERSION);
    return 1;
}

static const char * const plugin_capabilities[] = {
    "ui.settings_dashboard",
    "ui.music_layout", "ui.quick_drawer_layout",
    "ui.progress", "network.http.download_progress", "ui.list", "ui.settings", "ui.row_width", "ui.text_input", "ui.confirmation", "ui.toast", "ui.screenshot", "ui.theme",
    "filesystem.sd", "filesystem.realpath", "playback.control", "playback.state", "playback.events",
    "library.artist_albums", "library.paged", "library.album_cover", "network.http.sync", "network.http.async",
    "network.http.download", "filesystem.mkdir", "crypto.md5", "audio.peq", "data.json",
    "storage.namespaced", "storage.secrets", "playback.remote", "playback.remote_resolve", "filesystem.playlists", "library.refresh",
    "ui.home_layout", "ui.theme_refresh", "ui.reload", "ui.home_tiles", "ui.launcher_layout",
    "audio.stereo_width", "audio.crossfeed", "audio.crossfeed.angle", "playback.speed", "ui.home_background", "audio.hw_volume_curve", "ui.lock_screen", "ui.quick_toggle",
    "data.zip", "data.html", "ui.text_view", "data.zip_image", "data.image_thumbnail", "data.wallpaper", "ui.list_grid", "ui.list_showing",
    "ui.list_update", "ui.list_scroll_reset", "ui.text_view_images", "ui.list_wrap", "ui.settings_list_wrap", "ui.order_editor", "playback.transport_skip",
    "ui.player_layout_xml", "storage.secrets_get", "playback.progress",
    "playback.format", "playback.output_info", "playback.silent_volume", "audio.peq.transient",
    "playback.output_events", "playback.settings", "playback.ab_loop", "playback.ab_switch", "playback.http_seek",
    "audio.peq.state", "library.track_metadata", "playback.transient_volume",
    "ui.player_actions", "ui.player_layout_restore", "ui.now_playing.set_image", "ui.now_playing.set_text",
    "filesystem.file_ops", "ui.file_manager", "ui.player_menu", "ui.more_menu", "ui.file_picker",
    "ui.startup_image", "audio.mseb.stock", "playback.stay_on_screen",
    "ui.settings_list_10_sliders", "ui.button_lock", "playback.lyrics_refresh",
    "audio.loop", "ui.breeze_stream_icons"
};



int l_plugin_has_capability(lua_State * L) {
    const char * requested = luaL_checkstring(L, 1);
    bool found = false;
    for (size_t i = 0; i < sizeof(plugin_capabilities) / sizeof(plugin_capabilities[0]); i++) {
        if (strcmp(requested, plugin_capabilities[i]) == 0) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

/* plugin.media_capabilities() -- see NEXT_TODO-style remote-provider plan,
 * requirement 11. Every field here is a verified fact about this exact
 * pipeline (audio.c's decoder_open()/audio_output.c), not an aspirational
 * one:
 * - mp3/aac/flac are all three already confirmed working for a direct
 *   http(s):// stream today (decoder_open()'s is_stream_url() branch --
 *   MP3 default, FLAC via the "#.flac" hint, AAC via "#.aac"/"#.aacp" or
 *   an "audio/aac" Content-Type, ADTS framing via aac_open_stream()).
 * - max_bit_depth is 16 for what this function actually describes: a
 *   plugin-provided REMOTE stream (see decoder_open()'s is_stream_url()
 *   branch this whole comment block is about). audio.c's can_use_wide_path()
 *   explicitly excludes any decoder with a net_stream set, so a plugin's
 *   direct http(s):// stream stays on the s16 decode/output path regardless
 *   of what the source claims -- this is still an accurate, verified fact,
 *   not a stale one. It is NOT true of local file playback any more: local
 *   FLAC/WAV/ALAC/APE/AIFF sources above 16-bit now reach the DAC at
 *   PCM_FORMAT_S24_LE (audio_output.c's open_device()) when hw_params
 *   negotiation for it succeeds. USB-DAC output can also use S24_LE, and
 *   Bluetooth uses a stable S24_LE ALSA pipe; its negotiated codec may
 *   still convert to a lower precision.
 * - max_channels is 2: nothing in this pipeline remaps or mixes channel
 *   counts; every path above passes `channels` straight through with no
 *   multichannel-aware code found anywhere in the codebase.
 * - max_sample_rate is deliberately left 0 (unspecified) rather than a
 *   guessed number: config.rate is passed straight through to pcm_open()
 *   with no software-side cap anywhere in this pipeline, so the real
 *   ceiling is whatever this device's DAC/kernel driver negotiates --
 *   that's a hardware fact this codebase doesn't state anywhere and this
 *   function should not invent one. Fill this in once a real number is
 *   confirmed against the actual hardware, rather than guessing here. */

int l_plugin_media_capabilities(lua_State * L) {
    lua_newtable(L);

    lua_newtable(L);
    lua_pushstring(L, "mp3");  lua_rawseti(L, -2, 1);
    lua_pushstring(L, "aac");  lua_rawseti(L, -2, 2);
    lua_pushstring(L, "flac"); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "codecs");

    lua_newtable(L);
    lua_pushstring(L, "mp3");  lua_rawseti(L, -2, 1);
    lua_pushstring(L, "adts"); lua_rawseti(L, -2, 2);
    lua_pushstring(L, "flac"); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "containers");

    lua_pushinteger(L, 0);  lua_setfield(L, -2, "max_sample_rate"); /* see comment above -- unverified, do not treat as 0 Hz */
    lua_pushinteger(L, 16); lua_setfield(L, -2, "max_bit_depth");
    lua_pushinteger(L, 2);  lua_setfield(L, -2, "max_channels");

    lua_pushboolean(L, true);  lua_setfield(L, -2, "direct_http_streaming");
    lua_pushboolean(L, false); lua_setfield(L, -2, "range_seeking"); /* Phase 2, not built yet */
    lua_pushboolean(L, false); lua_setfield(L, -2, "hls");
    lua_pushboolean(L, false); lua_setfield(L, -2, "dash");

    lua_newtable(L); lua_setfield(L, -2, "encryption_modes"); /* empty: none supported */
    lua_newtable(L); lua_setfield(L, -2, "drm_systems");      /* empty: none supported */

    return 1;
}

int l_plugin_get_app_info(lua_State * L) {
    lua_newtable(L);
    lua_pushstring(L, app_version_label()); lua_setfield(L, -2, "version");
    lua_pushstring(L, app_build_identifier()); lua_setfield(L, -2, "build");
#ifdef HOST_BUILD
    lua_pushstring(L, "host");
#else
    lua_pushstring(L, "hiby-r1");
#endif
    lua_setfield(L, -2, "platform");
    lua_pushinteger(L, PLUGIN_API_VERSION); lua_setfield(L, -2, "plugin_api");
    return 1;
}
