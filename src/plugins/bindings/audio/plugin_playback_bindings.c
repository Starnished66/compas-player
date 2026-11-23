/* plugin.* bindings for playback control, remote tracks, equalizer, volume, now playing and play modes. */
#include "../../../ui/application/gui.h"
#include "../../../ui/player/screen/gui_lyrics.h"
#include "../../../ui/player/layout/player_layouts.h"
#include "../../../audio/effects/peq.h"
#include "../../../audio/effects/mseb.h"
#include "../../../hardware/power/battery.h"
#include "../../../hardware/power/charge_limiter.h"
#include "../../../audio/output/audio_output.h"
#include "../../storage/plugin_reserved_dirs.h"
#include "../../lua/plugin_lua_runtime.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include "../../manager/plugin_manager_internal.h"
lua_State * mseb_owner;
/* Indexed by audio_output_route_t. */
const char * const plugin_route_names[] = { "wired", "bluetooth", "usb_dac" };

/* ---- plugin.eq_*() -- bridge straight into peq.c's public API, no gui.c
 * layer needed (peq.c has no LVGL dependency, matching l_plugin_list_dir()/
 * l_plugin_sd_root() already calling plain C functions directly). Every
 * setter ends with peq_save(), the same "persist on every change" pattern
 * every native EQ-screen event handler already follows (see e.g. gui.c's
 * eq_bypass_switch_event_cb()), so a plugin-driven EQ change survives
 * reboot with no new persistence code. ---- */

static int l_plugin_eq_load_profile(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.eq_load_profile");
    bool ok = peq_load_from_path(path);
    if (ok) peq_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_eq_apply_profile(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.eq_apply_profile");
    bool persist = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    bool ok = peq_load_from_path(path);
    if (ok && persist) peq_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_eq_save_profile(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.eq_save_profile");
    if (plugin_path_in_program_dirs(path)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, peq_save_to_path(path));
    return 1;
}

/* Stock MSEB is a separate native DSP stage. Keep this bridge deliberately
 * transactional: every field is validated before publishing a new config. */
static const char * const mseb_value_fields[] = {
    "temperature", "bass_ext", "bass_texture", "thickness", "vocal_pos",
    "female_vocal", "sibilance_lf", "sibilance_hf", "impulse_tone", "air"
};

static int l_plugin_mseb_apply_state(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    mseb_config_t config = { 0 };
    lua_getfield(L, 1, "enabled");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 1, "enabled must be boolean");
    config.enabled = lua_toboolean(L, -1); lua_pop(L, 1);
    for (int i = 0; i < 10; i++) {
        lua_getfield(L, 1, mseb_value_fields[i]);
        luaL_argcheck(L, lua_isinteger(L, -1), 1, "all control values must be integers");
        lua_Integer value = lua_tointeger(L, -1); lua_pop(L, 1);
        luaL_argcheck(L, value >= -100 && value <= 100, 1, "all control values must be between -100 and 100");
        config.values[i] = (int) value;
    }
    lua_getfield(L, 1, "width");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TNUMBER, 1, "width must be a number");
    config.width = lua_tonumber(L, -1); lua_pop(L, 1);
    luaL_argcheck(L, isfinite(config.width) && config.width >= 0.0 && config.width <= 2.0,
                  1, "width must be finite and between 0 and 2");
    if (!mseb_set_config(&config)) { lua_pushboolean(L, false); return 1; }
    mseb_owner = config.enabled ? plugin_lua_main_state(L) : NULL;
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_get_mseb_state(lua_State * L) {
    mseb_config_t config;
    mseb_get_config(&config);
    lua_createtable(L, 0, 12);
    lua_pushboolean(L, config.enabled); lua_setfield(L, -2, "enabled");
    for (int i = 0; i < 10; i++) {
        lua_pushinteger(L, config.values[i]); lua_setfield(L, -2, mseb_value_fields[i]);
    }
    lua_pushnumber(L, config.width); lua_setfield(L, -2, "width");
    return 1;
}

/* Bounded cache lookup for one validated local SD path. This never scans tags
 * or walks the library; an unopened cache or absent row simply returns nil. */
static int l_plugin_get_track_metadata(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    if (!plugin_song_path_valid(path)) { lua_pushnil(L); return 1; }
    song_row_t row;
    if (!metadata_db_get_song_by_path(path, &row)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, row.id); lua_setfield(L, -2, "id");
    lua_pushstring(L, row.path); lua_setfield(L, -2, "path");
    lua_pushstring(L, row.tags.title); lua_setfield(L, -2, "title");
    lua_pushstring(L, row.tags.artist); lua_setfield(L, -2, "artist");
    lua_pushstring(L, row.tags.album); lua_setfield(L, -2, "album");
    lua_pushstring(L, row.tags.album_artist); lua_setfield(L, -2, "album_artist");
    lua_pushstring(L, row.tags.genre); lua_setfield(L, -2, "genre");
    return 1;
}

/* Same "pure audio-domain, straight into audio.c, no gui.c bridge" shape as
 * plugin.eq_*() above -- setting a hardware volume curve isn't paired with
 * any gui.c-local UI state (it doesn't move the volume slider or its
 * popup, it changes what a given slider position maps to internally), so
 * it doesn't need one. In-process only, like every other plugin config
 * call in this file -- not persisted, a plugin re-applies it at the top of
 * its own script on every boot (same convention set_home_layout() etc.
 * already use, see their own comments). HW_VOLUME_CURVE_LEN is audio.h's
 * own shared definition, not a local one, so this validation can't drift
 * out of sync with audio_set_custom_hw_volume_curve()'s actual buffer
 * size.
 *
 * loading_plugin_slot >= 0 for exactly the duration of a plugin's own
 * top-level script run (set/cleared around plugin_call() in
 * load_plugin_file(), never true from a later callback -- a real Gain
 * Mode plugin's Settings-list on_select handler, for instance, always
 * runs with it back at -1). Inside that window this call is part of
 * plugin_manager_init()'s own load transaction (which may still load more
 * plugin files, or discard this very one if it fails on a later call),
 * so it only stages the state change -- audio_stage_custom_hw_volume_
 * curve(), no hardware write, and no effect on the live state
 * audio_apply_volume() reads -- and load_plugin_file()/plugin_manager_
 * init() own the single deferred commit (see audio.h's own comment on
 * why an immediate write per call here would risk an intermediate value
 * actually reaching the DAC, or surviving a later failure). Outside that
 * window -- a live, already-loaded plugin changing its own curve --
 * audio_set_custom_hw_volume_curve() applies it immediately instead, for
 * real-time audible feedback. */
static int l_plugin_set_hw_volume_curve(lua_State * L) {
    bool during_load = loading_plugin_slot >= 0;

    if (lua_isnoneornil(L, 1)) {
        if (during_load) audio_stage_custom_hw_volume_curve(false, NULL);
        else audio_set_custom_hw_volume_curve(NULL);
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Unsigned raw_n = lua_rawlen(L, 1);
    if (raw_n != HW_VOLUME_CURVE_LEN)
        return luaL_error(L, "plugin.set_hw_volume_curve: table must have exactly %d entries (UI volume 0..100), got %d",
                           HW_VOLUME_CURVE_LEN, (int) raw_n);

    uint8_t curve[HW_VOLUME_CURVE_LEN];
    for (int i = 0; i < HW_VOLUME_CURVE_LEN; i++) {
        lua_rawgeti(L, 1, i + 1);
        /* lua_tointeger() alone silently converts a missing/nonnumeric/
         * non-integral entry to 0 -- and raw 0 is the hardware's LOUDEST
         * point (see compute_hw_raw()'s own comment in audio_volume.c), so a
         * malformed table (a hole, a string, a float like 128.5) could
         * otherwise turn into an unexpected full-volume step instead of a
         * caught error. lua_isinteger() rejects all of those before any
         * conversion happens. */
        if (!lua_isinteger(L, -1)) {
            int t = lua_type(L, -1);
            lua_pop(L, 1);
            return luaL_error(L, "plugin.set_hw_volume_curve: table[%d] (UI volume %d%%) is not an integer (got %s)",
                               i + 1, i, lua_typename(L, t));
        }
        lua_Integer v = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (v < 0 || v > 255)
            return luaL_error(L, "plugin.set_hw_volume_curve: table[%d] (UI volume %d%%) is %lld, must be 0..255",
                               i + 1, i, (long long) v);
        curve[i] = (uint8_t) v;
    }
    if (during_load) audio_stage_custom_hw_volume_curve(true, curve);
    else audio_set_custom_hw_volume_curve(curve);
    return 0;
}

/* ---- plugin.toggle_pause()/stop()/next_track()/prev_track()/seek()/
 * set_volume()/is_playing()/is_paused()/get_position()/get_duration() --
 * thin wrappers over gui.h's gui_plugin_*() bridges, needed here (unlike
 * plugin.eq_*() above) because audio.c's own functions must be paired with
 * gui.c-local UI state (play/pause icon, volume slider/popup, shuffle-aware
 * stepping) -- see gui.h's own comment on gui_plugin_toggle_pause() and
 * neighbors for why. ---- */

static int l_plugin_toggle_pause(lua_State * L) {
    (void) L;
    gui_plugin_toggle_pause();
    return 0;
}

static int l_plugin_stop(lua_State * L) {
    (void) L;
    gui_plugin_stop();
    return 0;
}

static int l_plugin_next_track(lua_State * L) {
    (void) L;
    gui_plugin_next_track();
    return 0;
}

static int l_plugin_prev_track(lua_State * L) {
    (void) L;
    gui_plugin_prev_track();
    return 0;
}

static int l_plugin_seek(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    gui_plugin_seek(seconds);
    return 0;
}

static int l_plugin_set_playback_speed(lua_State * L) {
    const char * directory = lua_isnil(L, 1) ? NULL :
        plugin_lua_check_external_path(L, 1, "plugin.set_playback_speed");
    double speed = luaL_checknumber(L, 2);
    lua_pushboolean(L, audio_set_playback_speed(directory, speed));
    return 1;
}

static int l_plugin_get_playback_speed(lua_State * L) {
    lua_pushnumber(L, audio_get_playback_speed());
    return 1;
}

static int l_plugin_set_ab_loop(lua_State * L) {
    lua_pushboolean(L, audio_set_ab_loop(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
static int l_plugin_get_ab_loop(lua_State * L) {
    double start, end;
    if (!audio_get_ab_loop(&start, &end)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushnumber(L, start); lua_setfield(L, -2, "start");
    lua_pushnumber(L, end); lua_setfield(L, -2, "finish");
    return 1;
}
static int l_plugin_clear_ab_loop(lua_State * L) {
    (void)L;
    audio_clear_ab_loop();
    return 0;
}
/* plugin.audio_loop_start(path) -> true | false. Pauses a playing track
 * first; the clip then repeats until audio_loop_stop() or until a track
 * resumes or starts. See audio_loop_start(). */
static int l_plugin_audio_loop_start(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.audio_loop_start");
    if (!gui_player_local_playback_allowed()) { lua_pushboolean(L, false); return 1; }
    if (audio_is_playing()) gui_plugin_toggle_pause();
    lua_pushboolean(L, audio_loop_start(path));
    return 1;
}
static int l_plugin_audio_loop_stop(lua_State * L) {
    (void)L;
    audio_loop_stop();
    return 0;
}
static int l_plugin_audio_loop_active(lua_State * L) {
    lua_pushboolean(L, audio_loop_is_active());
    return 1;
}
static int l_plugin_prepare_ab_switch(lua_State * L) {
    const char *path = plugin_lua_check_external_path(L, 1, "plugin.prepare_ab_switch");
    lua_pushboolean(L, audio_prepare_ab_switch(path));
    return 1;
}
static int l_plugin_select_ab_source(lua_State * L) {
    const char *source = luaL_checkstring(L, 1);
    if (strcmp(source, "a") != 0 && strcmp(source, "b") != 0)
        return luaL_error(L, "source must be 'a' or 'b'");
    lua_pushboolean(L, audio_select_ab_source(strcmp(source, "b") == 0));
    return 1;
}
static int l_plugin_clear_ab_switch(lua_State * L) {
    (void)L;
    audio_clear_ab_switch();
    return 0;
}
static int l_plugin_get_ab_switch(lua_State * L) {
    bool preparing, ready, source_b;
    audio_get_ab_switch(&preparing, &ready, &source_b);
    lua_newtable(L);
    lua_pushboolean(L, preparing); lua_setfield(L, -2, "preparing");
    lua_pushboolean(L, ready); lua_setfield(L, -2, "ready");
    lua_pushstring(L, source_b ? "b" : "a"); lua_setfield(L, -2, "source");
    return 1;
}

static int l_plugin_get_playback_format(lua_State * L) {
    audio_current_format_info_t f;
    if (!audio_get_current_format_info(&f) || !f.valid) { lua_pushnil(L); return 1; }
    static const char * const codecs[] = { "unknown", "flac", "mp3", "pcm", "dsd", "aac", "alac", "ape", "wma", "opus", "vorbis", "wavpack" };
    lua_newtable(L);
#define FMT_NUM(name, val) do { lua_pushinteger(L, (lua_Integer)(val)); lua_setfield(L, -2, name); } while (0)
    lua_pushstring(L, f.path); lua_setfield(L, -2, "path");
    int codec = (int)f.codec;
    lua_pushstring(L, codec >= 0 && (size_t)codec < sizeof(codecs) / sizeof(codecs[0]) ? codecs[codec] : "unknown"); lua_setfield(L, -2, "codec");
    FMT_NUM("sample_rate", f.source_sample_rate);
    FMT_NUM("bit_depth", f.source_bit_depth);
    FMT_NUM("output_sample_rate", f.output_sample_rate);
    FMT_NUM("output_bit_depth", f.output_bit_depth);
    FMT_NUM("channels", f.channels);
    FMT_NUM("bitrate_kbps", f.bitrate_kbps);
    lua_pushnumber(L, f.duration_seconds); lua_setfield(L, -2, "duration_seconds");
    lua_pushboolean(L, f.is_stream); lua_setfield(L, -2, "is_stream");
    lua_pushboolean(L, f.seekable); lua_setfield(L, -2, "seekable");
    lua_pushboolean(L, f.is_dsd); lua_setfield(L, -2, "is_dsd");
    lua_pushboolean(L, f.dsd_native); lua_setfield(L, -2, "dop");
    lua_pushboolean(L, f.replaygain_applied); lua_setfield(L, -2, "replaygain_applied");
    lua_pushnumber(L, f.replaygain_applied_db); lua_setfield(L, -2, "replaygain_applied_db");
    lua_pushnumber(L, f.software_volume_gain); lua_setfield(L, -2, "software_volume_gain");
    lua_pushnumber(L, f.playback_speed); lua_setfield(L, -2, "playback_speed");
    lua_pushboolean(L, f.crossfade_enabled); lua_setfield(L, -2, "crossfade_enabled");
    FMT_NUM("generation", f.generation);
#undef FMT_NUM
    return 1;
}

static int l_plugin_get_output_info(lua_State * L) {
    audio_current_format_info_t f = {0};
    bool have_format = audio_get_current_format_info(&f) && f.valid;
    audio_output_info_t out = {0};
#ifndef HOST_BUILD
    audio_output_get_info(&out);
#else
    out.active = have_format;
    out.route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.requested_route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.sample_rate = have_format ? f.output_sample_rate : 0;
    out.bit_depth = have_format ? f.output_bit_depth : 0;
    out.hardware_format_known = false;
#endif
    const char * route = plugin_route_names[out.active ? out.route : out.requested_route];
    lua_newtable(L);
    lua_pushstring(L, route); lua_setfield(L, -2, "route");
    lua_pushboolean(L, out.active); lua_setfield(L, -2, "active");
    lua_pushboolean(L, out.dop); lua_setfield(L, -2, "dop");
    lua_pushboolean(L, out.native_dsd); lua_setfield(L, -2, "native_dsd");
    bool resampling_known = have_format && out.active && out.hardware_format_known;
    lua_pushboolean(L, resampling_known && !out.native_dsd && (out.sample_rate != f.output_sample_rate)); lua_setfield(L, -2, "resampling");
    lua_pushboolean(L, resampling_known); lua_setfield(L, -2, "resampling_known");
    if (have_format) {
        lua_pushinteger(L, out.active ? out.sample_rate : f.output_sample_rate); lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, out.active ? out.bit_depth : f.output_bit_depth); lua_setfield(L, -2, "bit_depth");
    } else { lua_pushnil(L); lua_setfield(L, -2, "sample_rate"); lua_pushnil(L); lua_setfield(L, -2, "bit_depth"); }
    if (out.hardware_format_known) {
        lua_pushinteger(L, out.sample_rate); lua_setfield(L, -2, "hardware_sample_rate");
        lua_pushinteger(L, out.bit_depth); lua_setfield(L, -2, "hardware_bit_depth");
    } else { lua_pushnil(L); lua_setfield(L, -2, "hardware_sample_rate"); lua_pushnil(L); lua_setfield(L, -2, "hardware_bit_depth"); }
#ifndef HOST_BUILD
    char codec[32] = {0};
    if ((out.active ? out.route : out.requested_route) == AUDIO_OUTPUT_ROUTE_BLUETOOTH && gui_shell_get_bt_audio_codec(codec, sizeof(codec))) {
        lua_pushstring(L, codec); lua_setfield(L, -2, "bluetooth_codec");
    } else { lua_pushnil(L); lua_setfield(L, -2, "bluetooth_codec"); }
#else
    lua_pushnil(L); lua_setfield(L, -2, "bluetooth_codec");
#endif
    return 1;
}

static int l_plugin_set_transport_skip(lua_State * L) {
    const char * directory = plugin_lua_check_external_path(L, 1, "plugin.set_transport_skip");
    lua_Integer seconds = luaL_checkinteger(L, 2);
    if (seconds < 0 || seconds > 300) return luaL_error(L, "skip seconds must be 0..300");
    if (strlen(directory) >= PATH_MAX) return luaL_error(L, "skip directory too long");
    gui_player_set_transport_skip(directory, (int) seconds);
    return 0;
}

static int l_plugin_set_volume(lua_State * L) {
    lua_Integer percent = luaL_checkinteger(L, 1);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bool silent = false;
    bool persist = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "silent");
        silent = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    if (!persist) gui_plugin_set_volume_transient((int) percent, !silent);
    else if (silent) gui_plugin_set_volume_silent((int) percent);
    else gui_plugin_set_volume((int) percent);
    return 0;
}

static int l_plugin_is_playing(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_playing());
    return 1;
}

static int l_plugin_is_paused(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_paused());
    return 1;
}

static int l_plugin_get_position(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_position_seconds());
    return 1;
}

static int l_plugin_get_duration(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_duration_seconds());
    return 1;
}

/* plugin.get_playback_progress(expected_path) -> coherent snapshot | nil.
 * This query runs on every plugin progress tick, so keep its path check
 * bounded and allocation-free. audio.c compares it with the exact current
 * path under the playback-state lock; no filesystem lookup is needed here. */
static int l_plugin_get_playback_progress(lua_State * L) {
    size_t path_len = 0;
    const char * expected_path = luaL_checklstring(L, 1, &path_len);
    if (path_len == 0 || path_len >= PATH_MAX || expected_path[0] != '/' ||
        memchr(expected_path, '\0', path_len) != NULL) {
        lua_pushnil(L);
        return 1;
    }

    audio_playback_progress_t progress;
    if (!audio_get_playback_progress(expected_path, &progress)
        || progress.state == AUDIO_PLAYBACK_PROGRESS_NOT_READY) {
        lua_pushnil(L);
        return 1;
    }

    const char * terminal = NULL;
    switch (progress.state) {
    case AUDIO_PLAYBACK_PROGRESS_ACTIVE: terminal = "active"; break;
    case AUDIO_PLAYBACK_PROGRESS_NATURAL_EOF: terminal = "natural_eof"; break;
    case AUDIO_PLAYBACK_PROGRESS_MANUAL_STOP: terminal = "manual_stop"; break;
    case AUDIO_PLAYBACK_PROGRESS_NOT_READY:
    default:
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushnumber(L, progress.position_seconds); lua_setfield(L, -2, "position");
    lua_pushnumber(L, progress.duration_seconds); lua_setfield(L, -2, "duration");
    lua_pushboolean(L, progress.is_playing); lua_setfield(L, -2, "playing");
    lua_pushboolean(L, progress.is_paused); lua_setfield(L, -2, "paused");
    lua_pushstring(L, terminal); lua_setfield(L, -2, "terminal");
    return 1;
}

plugin_battery_info_t plugin_read_battery_info(void) {
    plugin_battery_info_t info;
    int level = battery_get_display_percent(); /* same value the status bar shows */
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    battery_external_power_state_t power_state = battery_get_external_power_state();
    bool power_disconnected = power_state == BATTERY_EXTERNAL_POWER_DISCONNECTED;
    bool capped = charge_limiter_is_confirmed_off();
    info.level = level;
    info.external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
    info.full = !power_disconnected && (battery_is_full() || capped);
    info.charging = !info.full && !power_disconnected && battery_is_charging();
    return info;
}

void push_battery_info(lua_State * L, const plugin_battery_info_t * info) {
    lua_newtable(L);
    lua_pushinteger(L, info->level); lua_setfield(L, -2, "level");
    lua_pushboolean(L, info->charging); lua_setfield(L, -2, "charging");
    lua_pushboolean(L, info->full); lua_setfield(L, -2, "full");
    lua_pushboolean(L, info->external_power); lua_setfield(L, -2, "external_power");
}

int current_volume_percent(void) {
    int percent = (int) (audio_get_volume() * 100.0f + 0.5f);
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static int l_plugin_get_volume(lua_State * L) {
    lua_pushinteger(L, current_volume_percent());
    return 1;
}

static int l_plugin_get_battery(lua_State * L) {
    plugin_battery_info_t info = plugin_read_battery_info();
    push_battery_info(L, &info);
    return 1;
}

/* plugin.get_now_playing() -> title, artist, album, duration_seconds, or a
 * single nil if nothing is loaded -- pull accessor for anything that isn't
 * reacting to the "track_started" event directly (e.g. a set_interval()
 * tick double-checking what's still playing). Backed by gui.c's own cached
 * now-playing globals, populated at the same two call sites that fire
 * "track_started" -- see gui_plugin_get_now_playing()'s own comment. */
static int l_plugin_get_now_playing(lua_State * L) {
    char title[128], artist[128], album[128];
    double duration_seconds = 0.0;
    if (!gui_plugin_get_now_playing(title, sizeof(title), artist, sizeof(artist), album, sizeof(album),
                                     &duration_seconds)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, title);
    lua_pushstring(L, artist);
    lua_pushstring(L, album);
    lua_pushnumber(L, duration_seconds);
    return 4;
}

/* plugin.get_play_mode() -> "sequential" | "repeat_all" | "repeat_one" |
 * "shuffle" | "shuffle_once" -- thin wrap of gui_plugin_get_play_mode(), same "no gui-state
 * code in this file" boundary as is_playing()/get_position() above. */
static int l_plugin_get_play_mode(lua_State * L) {
    lua_pushstring(L, gui_plugin_get_play_mode());
    return 1;
}

static int l_plugin_set_play_mode(lua_State * L) {
    const char * mode = luaL_checkstring(L, 1);
    int value;
    if (!strcmp(mode, "sequential")) value = 0;
    else if (!strcmp(mode, "repeat_all")) value = 1;
    else if (!strcmp(mode, "repeat_one")) value = 2;
    else if (!strcmp(mode, "shuffle")) value = PLAY_MODE_SHUFFLE;
    else if (!strcmp(mode, "shuffle_once")) value = PLAY_MODE_SHUFFLE_ONCE;
    else return luaL_error(L, "plugin.set_play_mode: expected sequential, repeat_all, repeat_one, shuffle, or shuffle_once");
    gui_player_set_play_mode(value);
    return 0;
}

static int l_plugin_get_crossfade(lua_State * L) { lua_pushboolean(L, current_settings.crossfade_enabled); return 1; }
static int l_plugin_get_gapless(lua_State * L) { lua_pushboolean(L, current_settings.gapless_enabled); return 1; }
static int l_plugin_get_replaygain_mode(lua_State * L) {
    static const char * const modes[] = { "off", "track", "album" };
    int mode = current_settings.replaygain_mode;
    if (mode < 0 || mode > 2) mode = 0;
    lua_pushstring(L, modes[mode]); return 1;
}
static int l_plugin_set_crossfade(lua_State * L) {
    gui_player_set_crossfade_enabled(lua_toboolean(L, 1));
    return 0;
}
static int l_plugin_set_gapless(lua_State * L) {
    gui_player_set_gapless_enabled(lua_toboolean(L, 1));
    return 0;
}
static int l_plugin_set_replaygain_mode(lua_State * L) {
    const char * mode = luaL_checkstring(L, 1);
    int value;
    if (!strcmp(mode, "off")) value = 0;
    else if (!strcmp(mode, "track")) value = 1;
    else if (!strcmp(mode, "album")) value = 2;
    else return luaL_error(L, "plugin.set_replaygain_mode: expected off, track, or album");
    gui_player_set_replaygain_mode(value);
    return 0;
}

/* plugin.get_current_track_path() -> path | nil, if nothing is loaded. */
static int l_plugin_get_current_track_path(lua_State * L) {
    const char * path = gui_plugin_get_current_track_path();
    if (!path) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, path);
    return 1;
}

/* A fetch completion must only refresh the exact track it saved for. The
 * native loader schedules disk/tag work asynchronously and rejects stale jobs. */
static int l_plugin_refresh_lyrics(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!inst || !inst->defined)
        return luaL_error(L, "plugin.refresh_lyrics requires plugin.define first");
    size_t len = 0;
    const char * expected = luaL_checklstring(L, 1, &len);
    if (!len || len >= PATH_MAX || memchr(expected, '\0', len))
        return luaL_error(L, "plugin.refresh_lyrics: invalid track path");
    const char * current = gui_plugin_get_current_track_path();
    if (!current || strcmp(expected, current) != 0) {
        lua_pushboolean(L, false);
        lua_pushliteral(L, "track_changed");
        return 2;
    }
    gui_lyrics_load_track(gui_player_get_playlist_index(), current);
    lua_pushboolean(L, true);
    return 1;
}

static int plugin_now_playing_set_value(lua_State * L, bool image) {
    const char * api = image ? "plugin.now_playing.set_image" : "plugin.now_playing.set_text";
    plugin_instance_t * inst = plugin_instance_for_state(plugin_lua_main_state(L));
    if (!inst || !inst->defined || inst->disposing || inst->aborted)
        return luaL_error(L, "%s requires an active plugin.define({id=...})", api);
    size_t name_len;
    const char * name = luaL_checklstring(L, 1, &name_len);
    if (strlen(name) != name_len) return luaL_error(L, "%s: invalid widget name", api);
    const char * value = NULL;
    char source[515];
    if (!lua_isnoneornil(L, 2)) {
        size_t len;
        value = luaL_checklstring(L, 2, &len);
        if (strlen(value) != len || len > (image ? 511u : 4095u))
            return luaL_error(L, "%s: invalid or oversized value", api);
        if (image) {
            char path[512];
            int n = value[0] == '/' ? snprintf(path, sizeof(path), "%s", value) :
                snprintf(path, sizeof(path), "%s/.plugins/%s", MUSIC_ROOT_DIR, value);
            if (!value[0] || n < 0 || (size_t)n >= sizeof(path))
                return luaL_error(L, "%s: invalid or oversized image path", api);
            lua_pushstring(L, path);
            plugin_lua_check_external_path(L, -1, api);
            lua_pop(L, 1);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
                return luaL_error(L, "%s: image file does not exist", api);
            snprintf(source, sizeof(source), "S:%s", path);
            /* The same path may have been replaced since LVGL last decoded
             * it. Drop cached pixels and dimensions before validation;
             * lv_image_cache_drop also clears the header cache and notifies
             * draw units to discard source-keyed rendering resources. */
            lv_image_cache_drop(source);
            lv_image_header_t header;
            if (lv_image_decoder_get_info(source, &header) != LV_RESULT_OK)
                return luaL_error(L, "%s: unsupported image file", api);
            value = source;
        }
    }
    const char * error = player_layouts_set_plugin_value(inst->id, name, value, image);
    if (error) return luaL_error(L, "%s: %s", api, error);
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_now_playing_set_image(lua_State * L) {
    return plugin_now_playing_set_value(L, true);
}

static int l_plugin_now_playing_set_text(lua_State * L) {
    return plugin_now_playing_set_value(L, false);
}

const luaL_Reg plugin_now_playing_funcs[] = {
    { "set_image", l_plugin_now_playing_set_image },
    { "set_text", l_plugin_now_playing_set_text },
    { NULL, NULL }
};

const luaL_Reg plugin_playback_funcs[] = {
    { "eq_load_profile",           l_plugin_eq_load_profile },
    { "eq_apply_profile",          l_plugin_eq_apply_profile },
    { "eq_save_profile",           l_plugin_eq_save_profile },
    { "mseb_apply_state",            l_plugin_mseb_apply_state },
    { "get_mseb_state",              l_plugin_get_mseb_state },
    { "get_track_metadata",         l_plugin_get_track_metadata },
    { "set_hw_volume_curve",       l_plugin_set_hw_volume_curve },
    { "toggle_pause",              l_plugin_toggle_pause },
    { "stop",                      l_plugin_stop },
    { "next_track",                l_plugin_next_track },
    { "prev_track",                l_plugin_prev_track },
    { "seek",                      l_plugin_seek },
    { "set_playback_speed",        l_plugin_set_playback_speed },
    { "get_playback_speed",        l_plugin_get_playback_speed },
    { "set_transport_skip",        l_plugin_set_transport_skip },
    { "set_volume",                l_plugin_set_volume },
    { "set_ab_loop",               l_plugin_set_ab_loop },
    { "clear_ab_loop",             l_plugin_clear_ab_loop },
    { "get_ab_loop",               l_plugin_get_ab_loop },
    { "audio_loop_start",          l_plugin_audio_loop_start },
    { "audio_loop_stop",           l_plugin_audio_loop_stop },
    { "audio_loop_active",         l_plugin_audio_loop_active },
    { "prepare_ab_switch",         l_plugin_prepare_ab_switch },
    { "select_ab_source",          l_plugin_select_ab_source },
    { "get_ab_switch",             l_plugin_get_ab_switch },
    { "clear_ab_switch",           l_plugin_clear_ab_switch },
    { "get_playback_format",       l_plugin_get_playback_format },
    { "get_output_info",           l_plugin_get_output_info },
    { "get_volume",                l_plugin_get_volume },
    { "get_battery",               l_plugin_get_battery },
    { "is_playing",                l_plugin_is_playing },
    { "is_paused",                 l_plugin_is_paused },
    { "get_position",              l_plugin_get_position },
    { "get_duration",              l_plugin_get_duration },
    { "get_playback_progress",     l_plugin_get_playback_progress },
    { "get_now_playing",           l_plugin_get_now_playing },
    { "get_play_mode",             l_plugin_get_play_mode },
    { "set_play_mode",             l_plugin_set_play_mode },
    { "get_crossfade",             l_plugin_get_crossfade },
    { "set_crossfade",             l_plugin_set_crossfade },
    { "get_gapless",               l_plugin_get_gapless },
    { "set_gapless",               l_plugin_set_gapless },
    { "get_replaygain_mode",       l_plugin_get_replaygain_mode },
    { "set_replaygain_mode",       l_plugin_set_replaygain_mode },
    { "get_current_track_path",    l_plugin_get_current_track_path },
    { "refresh_lyrics",            l_plugin_refresh_lyrics },
    { NULL, NULL }
};
