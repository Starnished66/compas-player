#!/usr/bin/env python3
"""Exercise the production Lua registration/rollback code against real Lua."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'src/plugins/plugin_manager.c').read_text()
header = (root / 'src/plugins/plugin_manager.h').read_text()

def function(name):
    match = re.search(r'^(?:static )?(?:int|void|bool) ' + name + r'\([^\n]*\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError(f'missing production function {name}')
    return match.group()

event_enum = re.search(r'typedef enum \{\n    PLUGIN_EVENT_TRACK_STARTED.*?\n\} plugin_event_t;', src, re.S).group()
subscriber_type = re.search(r'typedef struct \{\n    lua_State \* L;\n    int ref;\n\} plugin_event_subscriber_t;', src).group()
interval_type = re.search(r'typedef struct \{\n    lua_State \* L;\n    int ref;\n    bool active;\n\} plugin_interval_t;', src).group()
max_intervals = re.search(r'^#define PLUGIN_MAX_INTERVALS (\d+)', header, re.M).group(1)
min_interval = re.search(r'^#define PLUGIN_INTERVAL_MIN_MS (\d+)', header, re.M).group(1)
max_subscribers = re.search(r'^#define PLUGIN_MAX_EVENT_SUBSCRIBERS (\d+)', header, re.M).group(1)

c_source = r'''#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LUA_NOREF (-2)
#define PLUGIN_MAX_INTERVALS INTERVAL_LIMIT
#define PLUGIN_INTERVAL_MIN_MS INTERVAL_FLOOR
#define INTERVAL_LIMIT ''' + max_intervals + r'''
#define INTERVAL_FLOOR ''' + min_interval + '\n' + event_enum + '\n' + subscriber_type + '\n' + interval_type + r'''
#define PLUGIN_MAX_EVENT_SUBSCRIBERS ''' + max_subscribers + r'''
static plugin_event_subscriber_t plugin_event_subscribers[PLUGIN_EVENT_COUNT][PLUGIN_MAX_EVENT_SUBSCRIBERS];
static int plugin_event_subscriber_count[PLUGIN_EVENT_COUNT];
static plugin_interval_t plugin_intervals[PLUGIN_MAX_INTERVALS];
static int cleared_intervals;
static int hits;
static int finalizer_runs;
static int gc_hit(lua_State *L) {(void)L;finalizer_runs++;return 0;}
typedef struct {lua_State *L; bool disposing;} plugin_instance_t;
static plugin_instance_t plugin_instances[3];
static plugin_instance_t *plugin_instance_for_state(lua_State *L) {
    for (int i=0;i<3;i++) if(plugin_instances[i].L==L) return &plugin_instances[i];
    return NULL;
}
static int l_plugin_api_guard(lua_State *L);
static int plugin_call_native(lua_State *L, lua_CFunction fn) {
    int nargs=lua_gettop(L); lua_pushcfunction(L,fn); lua_insert(L,1);
    int rc=lua_pcall(L,nargs,LUA_MULTRET,0); if(rc!=LUA_OK) return lua_error(L);
    return lua_gettop(L);
}
static void bind_guarded(lua_State *L,const char *name,lua_CFunction fn) {
    lua_pushcfunction(L,fn); lua_pushcclosure(L,l_plugin_api_guard,1); lua_setglobal(L,name);
}
static void gui_plugin_set_interval(int slot, uint32_t ms) {(void)slot;(void)ms;}
static void gui_plugin_clear_interval(int slot) {(void)slot;cleared_intervals++;}
static lua_State *plugin_main_state(lua_State *L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State *main = lua_tothread(L, -1); lua_pop(L, 1); return main ? main : L;
}
static int plugin_call(lua_State *L, int nargs, int nresults, int errfunc) {
    return lua_pcall(L, nargs, nresults, errfunc);
}
static int hit(lua_State *L) {(void)L;hits++;return 0;}
''' + '\n'.join(function(n) for n in ('l_plugin_on', 'l_plugin_set_interval', 'notify_event_no_args',
                                      'plugin_manager_interval_fired', 'failed_load_owner_matches',
                                      'discard_failed_plugin_events', 'discard_failed_plugin_intervals', 'l_plugin_api_guard')) + r'''
int main(void) {
    lua_State *good=luaL_newstate(), *bad=luaL_newstate();
    luaL_openlibs(good); luaL_openlibs(bad);
    plugin_instances[0].L=good; plugin_instances[1].L=bad;
    bind_guarded(good,"on",l_plugin_on); bind_guarded(good,"set_interval",l_plugin_set_interval);
    lua_pushcfunction(good,hit);lua_setglobal(good,"hit");
    bind_guarded(bad,"on",l_plugin_on); bind_guarded(bad,"set_interval",l_plugin_set_interval);
    lua_pushcfunction(bad,hit);lua_setglobal(bad,"hit");
    lua_pushcfunction(bad,gc_hit);lua_setglobal(bad,"gc_hit");
    lua_pushinteger(bad,PLUGIN_MAX_INTERVALS-1);lua_setglobal(bad,"available_intervals");
    assert(luaL_dostring(good,"on('paused', function() hit() end); set_interval(1, function() hit() end)")==LUA_OK);
    assert(luaL_dostring(bad,"local co=coroutine.create(function() for i=1,15 do on('paused', function() end) end; assert(not pcall(on,'paused',function() end)); for i=1,available_intervals do set_interval(1, function() end) end; assert(not pcall(set_interval,1,function() end)); gc_obj=setmetatable({}, {__gc=function() gc_hit(); on('paused',function() end) end}); error('load failure') end); assert(coroutine.resume(co))")!=LUA_OK);
    assert(plugin_event_subscriber_count[PLUGIN_EVENT_PAUSED]==16);
    for(int i=0;i<PLUGIN_MAX_INTERVALS;i++) assert(plugin_intervals[i].active);
    discard_failed_plugin_events(bad,false);
    discard_failed_plugin_intervals(bad,false);
    plugin_instances[1].disposing=true; /* failed-load close guard blocks __gc re-registration */
    lua_close(bad);
    plugin_instances[1].L=NULL;
    assert(finalizer_runs==1);
    assert(plugin_event_subscriber_count[PLUGIN_EVENT_PAUSED]==1);
    assert(plugin_intervals[0].active);
    for(int i=1;i<PLUGIN_MAX_INTERVALS;i++) assert(!plugin_intervals[i].active);
    assert(cleared_intervals==PLUGIN_MAX_INTERVALS-1);
    notify_event_no_args(PLUGIN_EVENT_PAUSED,"paused");
    plugin_manager_interval_fired(1); /* freed slot must be a no-op */
    plugin_manager_interval_fired(0);
    assert(hits==2); /* only the successful plugin's two callbacks ran */
    lua_State *next=luaL_newstate(); luaL_openlibs(next);
    plugin_instances[2].L=next;
    bind_guarded(next,"on",l_plugin_on); bind_guarded(next,"set_interval",l_plugin_set_interval);
    lua_pushcfunction(next,hit);lua_setglobal(next,"hit");
    assert(luaL_dostring(next,"local co=coroutine.create(function() on('paused', function() hit() end); set_interval(1, function() hit() end) end); assert(coroutine.resume(co))")==LUA_OK);
    assert(plugin_intervals[1].active); /* failed owner's slot was reusable */
    notify_event_no_args(PLUGIN_EVENT_PAUSED,"paused");
    plugin_manager_interval_fired(1);
    assert(hits==5);
    lua_close(next); lua_close(good);
    puts("failed plugin rollback passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='compas-failed-plugin-') as tmp:
    tmp = pathlib.Path(tmp)
    c_file = tmp / 'test.c'
    binary = tmp / 'test'
    c_file.write_text(c_source)
    lua_sources = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DLUA_USE_LINUX',
                    '-I' + str(root / 'lua/src'), str(c_file), *lua_sources, '-lm', '-ldl', '-o', str(binary)],
                   check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
