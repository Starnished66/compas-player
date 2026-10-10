#!/usr/bin/env python3
"""Run the actual Lua C bindings with native/UI edges isolated by test doubles."""
import pathlib, re, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'src/plugins/plugin_manager.c').read_text()
names = ['l_plugin_set_volume', 'l_plugin_eq_apply_profile', 'l_plugin_set_play_mode',
         'l_plugin_set_replaygain_mode', 'l_plugin_get_replaygain_mode',
         'l_plugin_set_crossfade', 'l_plugin_set_gapless',
         'l_plugin_get_crossfade', 'l_plugin_get_gapless',
         'l_plugin_get_playback_format', 'l_plugin_get_output_info']
functions = []
for name in names:
    match = re.search(r'^static int ' + name + r'\([^\n]*\) \{[^\n]*\} *$', source, re.M)
    if not match:
        match = re.search(r'^static int ' + name + r'\([^\n]*\) \{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError(f'missing production binding: {name}')
    functions.append(match.group(0))
for name, result in [('plugin_output_snapshot', 'bool'), ('push_output_snapshot', 'void'),
                     ('plugin_output_snapshot_equal', 'bool'), ('plugin_output_poll', 'void')]:
    match = re.search(r'^static ' + result + ' ' + name + r'\(.*?\) \{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError(f'missing production event function: {name}')
    functions.append(match.group(0))
prefix = r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "audio.h"
#include "audio_output.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
player_settings_t current_settings;
static int volume, popups, saves, loads, mode, output_reads, headphone_state;
static char codec_cache[32] = "LDAC";
static bool bt_connected = true;
typedef struct {
    int route, requested_route, headphones;
    bool active, bluetooth;
    char bluetooth_codec[32];
} plugin_output_snapshot_t;
typedef struct { lua_State *L; int ref; } plugin_event_subscriber_t;
enum { PLUGIN_EVENT_OUTPUT_CHANGED = 0 };
static int plugin_event_subscriber_count[1];
static plugin_event_subscriber_t plugin_event_subscribers[1][1];
static bool plugin_output_snapshot_valid;
static plugin_output_snapshot_t plugin_last_output_snapshot;
/* The production dispatcher adds a time budget; these handlers are quick. */
static int plugin_call(lua_State *L, int nargs, int nresults, int errfunc) {
    return lua_pcall(L, nargs, nresults, errfunc);
}
int get_headphone_state(void) { return headphone_state; }
bool gui_shell_is_bt_audio_connected(void) { return bt_connected; }
static bool load_ok = true, format_ok;
static audio_current_format_info_t format;
static audio_output_info_t output;
static const char *check_plugin_external_path(lua_State *L, int at, const char *label) {
    (void)label; return luaL_checkstring(L, at);
}
static int volume_saves, topbar, remembered;
static float applied_volume;
void gui_player_set_volume_percent(int v) { volume = v; }
void audio_set_volume(float v) { applied_volume = v; }
void gui_player_remember_volume_percent(int v) { remembered = v; }
void settings_save(const player_settings_t *v) { assert(v == &current_settings); volume_saves++; }
void show_volume_popup(int v) { assert(v == volume); popups++; }
void refresh_volume_topbar(int v) { topbar = v; }
bool peq_load_from_path(const char *p) { assert(!strcmp(p, "/sd/profile")); loads++; return load_ok; }
void peq_save(void) { saves++; }
void gui_player_set_play_mode(int v) { mode = v; }
void gui_player_set_replaygain_mode(int v) { current_settings.replaygain_mode = v; }
void gui_player_set_crossfade_enabled(bool v) { current_settings.crossfade_enabled = v; }
void gui_player_set_gapless_enabled(bool v) { current_settings.gapless_enabled = v; }
bool audio_get_current_format_info(audio_current_format_info_t *f) { *f = format; return format_ok; }
void audio_output_get_info(audio_output_info_t *o) { output_reads++; *o = output; }
bool gui_shell_get_bt_audio_codec(char *buf, size_t n) { snprintf(buf,n,"%s",codec_cache); return true; }
'''
ui_source = (root / 'src/ui/gui_plugins.c').read_text()
ui_functions = []
for declaration in ['static void plugin_set_volume', 'void gui_plugin_set_volume', 'void gui_plugin_set_volume_silent', 'void gui_plugin_set_volume_transient']:
    match = re.search(r'^' + declaration + r'\([^\n]*\) \{.*?^\}', ui_source, re.M | re.S)
    if not match:
        raise RuntimeError(f'missing production volume function: {declaration}')
    ui_functions.append(match.group(0))
functions = ui_functions + functions
register = '\n'.join(f'    lua_pushcfunction(L, {n}); lua_setglobal(L, "{n.removeprefix("l_plugin_")}");' for n in names)
tests = r'''
    assert(luaL_dostring(L, "set_volume(40); set_volume(30,{silent=true}); set_volume(20,{silent=false})") == LUA_OK);
    assert(volume == 20 && popups == 2 && volume_saves == 3 && topbar == 20 && remembered == 20 && applied_volume == 0.2f);
    assert(luaL_dostring(L, "set_volume(math.maxinteger,{silent=true})") == LUA_OK && volume == 100);
    assert(luaL_dostring(L, "set_volume(math.mininteger,{silent=true})") == LUA_OK && volume == 0);
    assert(luaL_dostring(L, "set_volume(18,{silent=true,persist=false}); set_volume(19,{silent=true,persist=false})") == LUA_OK);
    assert(volume == 19 && popups == 2 && volume_saves == 5 && remembered == 0 && topbar == 19 && applied_volume == 0.19f);
    assert(luaL_dostring(L, "set_volume(22,{persist=false})") == LUA_OK);
    assert(volume == 22 && popups == 3 && volume_saves == 5 && remembered == 0);
    assert(luaL_dostring(L, "set_volume(25,{silent=true,persist=true})") == LUA_OK);
    assert(volume == 25 && volume_saves == 6 && remembered == 25);
    assert(luaL_dostring(L, "assert(not pcall(set_volume,1,'bad'))") == LUA_OK);
    assert(luaL_dostring(L, "assert(eq_apply_profile('/sd/profile',{persist=false}))") == LUA_OK);
    assert(loads == 1 && saves == 0);
    assert(luaL_dostring(L, "assert(eq_apply_profile('/sd/profile',{}))") == LUA_OK);
    assert(saves == 1);
    load_ok = false;
    assert(luaL_dostring(L, "assert(not eq_apply_profile('/sd/profile'))") == LUA_OK && saves == 1);
    assert(luaL_dostring(L, "set_play_mode('shuffle'); assert(not pcall(set_play_mode,'bad'))") == LUA_OK && mode == 3);
    assert(luaL_dostring(L, "set_replaygain_mode('album'); assert(get_replaygain_mode()=='album'); assert(not pcall(set_replaygain_mode,'bad'))") == LUA_OK);
    assert(luaL_dostring(L, "set_crossfade(true); set_gapless(false); assert(get_crossfade()); assert(not get_gapless())") == LUA_OK);
    assert(luaL_dostring(L, "assert(get_playback_format()==nil)") == LUA_OK);
    format_ok = true; format.valid = true; format.codec = AUDIO_CODEC_FLAC;
    format.source_sample_rate = 96000; format.source_bit_depth = 24;
    format.output_sample_rate = 96000; format.output_bit_depth = 24;
    format.seekable = true; format.replaygain_applied = true; format.replaygain_applied_db = -3.0;
    format.software_volume_gain = 0.42; format.playback_speed = 1.25; format.crossfade_enabled = true; strcpy(format.path,"/sd/track.flac");
    assert(luaL_dostring(L,"local f=get_playback_format(); assert(f.sample_rate==96000 and f.bit_depth==24 and f.seekable and f.codec=='flac' and f.replaygain_applied and f.replaygain_applied_db==-3.0 and f.software_volume_gain==0.42 and f.playback_speed==1.25 and f.crossfade_enabled)") == LUA_OK);
    output.active = true; output.route = AUDIO_OUTPUT_ROUTE_WIRED;
    output.sample_rate = 48000; output.bit_depth = 16; output.hardware_format_known = true;
    assert(luaL_dostring(L,"local o=get_output_info(); assert(o.route=='wired' and o.hardware_sample_rate==48000 and o.resampling_known and o.resampling)") == LUA_OK);
    output.route = output.requested_route = AUDIO_OUTPUT_ROUTE_BLUETOOTH;
    output.hardware_format_known = false;
    assert(luaL_dostring(L,"local o=get_output_info(); assert(o.route=='bluetooth' and o.bluetooth_codec=='LDAC' and o.hardware_sample_rate==nil and not o.resampling_known)") == LUA_OK);
    int before = output_reads;
    plugin_output_poll(); assert(output_reads == before);
    assert(luaL_dostring(L, "events=0; return function(now,prev) events=events+1; assert(now.route=='bluetooth'); if events==1 then assert(prev.headphone_state==0 and now.headphone_state==2) else assert(now.bluetooth_codec=='aptX' and prev.bluetooth_codec=='LDAC') end end") == LUA_OK);
    plugin_event_subscribers[0][0].L = L;
    plugin_event_subscribers[0][0].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_event_subscriber_count[0] = 1;
    plugin_output_poll();
    assert(luaL_dostring(L,"assert(events==0)") == LUA_OK);
    headphone_state = 2; plugin_output_poll(); plugin_output_poll();
    assert(luaL_dostring(L,"assert(events==1)") == LUA_OK);
    strcpy(codec_cache,"aptX"); plugin_output_poll(); plugin_output_poll();
    assert(luaL_dostring(L,"assert(events==2)") == LUA_OK);
    lua_close(L);
    puts("Lua audio API binding tests passed");
    return 0;
}
'''
# Use the real project's Lua VM, not a reimplementation of Lua conversion rules.
with tempfile.TemporaryDirectory(prefix='compas-plugin-audio-') as folder:
    folder = pathlib.Path(folder)
    c = folder/'bindings.c'
    # Determine settings typedef from repository, keeping the double layout exact.
    c.write_text(prefix + '\n'.join(functions) + '\nint main(void) {\n lua_State *L=luaL_newstate(); assert(L); luaL_openlibs(L);\n' + register + tests)
    lua = [str(p) for p in (root/'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    cmd = ['cc','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-DLUA_USE_LINUX',
           '-I'+str(root/'lua/src'),'-I'+str(root/'src/audio'),'-I'+str(root/'src/core'),'-I'+str(root/'src/library'),'-I'+str(root/'src/hardware'),
           str(c),*lua,'-lm','-ldl','-o',str(folder/'test')]
    subprocess.run(cmd,check=True,cwd=root)
    subprocess.run([str(folder/'test')],check=True,cwd=root)
