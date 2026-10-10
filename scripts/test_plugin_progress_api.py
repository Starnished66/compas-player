#!/usr/bin/env python3
"""Run production progress bindings with real Lua, including worker snapshots."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'src/plugins/plugin_manager.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?(?:int|void|bool|const char \*|lua_State \*) ' + name + r'\([^\n]*\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()

# Pointer-returning helpers have their '*' adjacent to the name.
def pointer_function(name):
    match = re.search(r'^static (?:const char|lua_State) \* ' + name + r'\([^\n]*\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()

instance_lookup = re.search(r'^static plugin_instance_t \* plugin_instance_for_state\([^\n]*\) \{.*?^\}', src, re.M | re.S).group()
registry = re.search(r'^static lua_State \* plugin_progress_owner = NULL;.*?^static lua_Integer plugin_progress_next_handle = 1;', src, re.M | re.S).group()
body = r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#define PLUGIN_MAX_ASYNC_HTTP 2
typedef struct { bool cancelled; } http_cancel_token_t;
static bool http_cancel_token_is_cancelled(http_cancel_token_t *c) {return c->cancelled;}
typedef struct {
    bool active, is_download;
    uint16_t generation;
    lua_State *L;
    uint64_t downloaded_bytes, total_bytes;
    http_cancel_token_t cancel;
} plugin_async_http_t;
static plugin_async_http_t plugin_async_http[PLUGIN_MAX_ASYNC_HTTP];
static pthread_mutex_t plugin_download_progress_mutex = PTHREAD_MUTEX_INITIALIZER;
#define PLUGIN_MAX_FILES 2
typedef struct {lua_State *L;bool disposing;} plugin_instance_t;
static plugin_instance_t plugin_instances[PLUGIN_MAX_FILES];
static int plugin_instance_count = PLUGIN_MAX_FILES;
static int plugin_call_native(lua_State *L,lua_CFunction fn) {
    int nargs=lua_gettop(L);lua_pushcfunction(L,fn);lua_insert(L,1);
    int rc=lua_pcall(L,nargs,LUA_MULTRET,0);if(rc!=LUA_OK)return lua_error(L);return lua_gettop(L);
}
static bool visible, unavailable;
static int percent, shows, updates;
static bool gui_plugin_show_progress(const char *t,const char *m,int p) {
    assert(t && m); if(unavailable)return false; visible=true; percent=p; shows++; return true;
}
static bool gui_plugin_update_progress(const char *m,int p) {
    assert(m);if(!visible)return false;percent=p;updates++;return true;
}
static bool gui_plugin_progress_is_visible(void) {return visible;}
static void gui_plugin_close_progress(void) {visible=false;}
static int push_plugin_error(lua_State *L,const char *msg) {lua_pushnil(L);lua_pushstring(L,msg);return 2;}
''' + registry + '\n' + instance_lookup + '\n' + pointer_function('plugin_main_state') + '\n' + pointer_function('plugin_progress_text') + '\n' + '\n'.join(function(n) for n in (
    'plugin_progress_percent', 'plugin_progress_reset', 'l_plugin_show_progress',
    'l_plugin_update_progress', 'l_plugin_close_progress', 'l_plugin_get_download_progress',
    'plugin_async_download_progress', 'failed_load_owner_matches', 'l_plugin_api_guard',
    'plugin_manager_mark_disposing')) + r'''
static void guarded(lua_State *L,const char *name,lua_CFunction fn) {
    lua_pushcfunction(L,fn);lua_pushcclosure(L,l_plugin_api_guard,1);lua_setglobal(L,name);
}
static int finalizers;
static int finalizer_hit(lua_State *L){(void)L;finalizers++;return 0;}
static void bind(lua_State *L) {
    luaL_openlibs(L);
    guarded(L,"show",l_plugin_show_progress);
    guarded(L,"update",l_plugin_update_progress);
    guarded(L,"close",l_plugin_close_progress);
    guarded(L,"bytes",l_plugin_get_download_progress);
}
static void run(lua_State *L,const char *code) {
    int rc=luaL_dostring(L,code);if(rc!=LUA_OK)fprintf(stderr,"%s\n",lua_tostring(L,-1));assert(rc==LUA_OK);
}
static atomic_bool writing;
static void *writer(void *data) {
    for(uint64_t i=1;i<=100000;i++) assert(plugin_async_download_progress(i,i*2,data));
    atomic_store(&writing,false);return NULL;
}
int main(void) {
    lua_State *a=luaL_newstate(), *b=luaL_newstate();plugin_instances[0].L=a;plugin_instances[1].L=b;bind(a);bind(b);
    unavailable=true;run(a,"local h,e=show('t','m');assert(h==nil and e)");unavailable=false;
    run(a,"h=assert(show('Title','Looking up'));assert(update(h,'Download',0.25))");
    assert(visible&&percent==25&&shows==1&&updates==1);
    lua_pushinteger(b,plugin_progress_handle);lua_setglobal(b,"foreign");
    run(b,"assert(not update(foreign,'foreign',1));assert(not close(foreign))");assert(visible&&percent==25);
    run(a,"local co=coroutine.create(function() assert(update(h,'from coroutine',0.5)) end);assert(coroutine.resume(co))");assert(percent==50);
    run(a,"assert(not pcall(show,'title','message',0/0));assert(not pcall(show,'t','m',math.huge));assert(not pcall(show,'t','m',-0.1));assert(not pcall(update,h,'m',1.1));assert(not pcall(show,string.rep('a',129),'m'));assert(not pcall(show,'t',string.rep('m',513)));assert(not pcall(show,'t','a'..string.char(0)..'b'))");assert(percent==50&&visible);
    visible=false;run(a,"assert(not update(h,'dismissed',1));assert(not close(h));h=assert(show('new','message'))");assert(visible);
    lua_Integer old=plugin_progress_handle;run(b,"h=assert(show('other','message',0))");assert(plugin_progress_handle>old);
    run(a,"assert(not update(h,'replaced',1));assert(not close(h))");assert(visible&&percent==0);
    assert(!failed_load_owner_matches(plugin_progress_owner,a,false));
    assert(failed_load_owner_matches(plugin_progress_owner,b,true));plugin_progress_reset();assert(!visible&&!plugin_progress_owner);
    run(b,"assert(not update(h,'unloaded',1));h=assert(show('after reload','m'));assert(close(h));assert(not close(h))");
    plugin_async_http_t *req=&plugin_async_http[0];req->active=true;req->is_download=true;req->generation=1;req->L=a;
    run(a,"local p=assert(bytes(257));assert(p.downloaded==0 and p.total==0);assert(bytes(-1)==nil);assert(bytes(0x1000000)==nil);assert(bytes(256)==nil);assert(bytes(513)==nil)");
    run(b,"assert(bytes(257)==nil)");
    assert(plugin_async_download_progress(10,100,req));run(a,"local p=assert(bytes(257));assert(p.downloaded==10 and p.total==100)");
    req->cancel.cancelled=true;assert(!plugin_async_download_progress(20,100,req));run(a,"assert(bytes(257)==nil)");req->cancel.cancelled=false;
    req->is_download=false;run(a,"assert(bytes(257)==nil)");req->is_download=true;
    assert(plugin_async_download_progress(0,0,req));
    pthread_t thread;atomic_init(&writing,true);assert(!pthread_create(&thread,NULL,writer,req));
    do {run(a,"local p=assert(bytes(257));assert(p.total==p.downloaded*2)");}while(atomic_load(&writing));
    assert(!pthread_join(thread,NULL));req->active=false;run(a,"assert(bytes(257)==nil)");
    lua_pushcfunction(a,finalizer_hit);lua_setglobal(a,"finalizer_hit");
    run(a,"gc_obj=setmetatable({}, {__gc=function() finalizer_hit(); assert(not pcall(show,'finalizer','must not reopen')) end})");
    plugin_manager_mark_disposing();plugin_progress_reset();lua_close(a);lua_close(b);
    assert(finalizers==1&&!visible&&plugin_progress_owner==NULL);
    puts("plugin progress bindings passed");return 0;
}
'''
# Exercise the real load/reload integration, not just the standalone reset helper.
assert 'failed_load_owner_matches(plugin_progress_owner, L, aborted)' in function('discard_failed_plugin_resources')
assert 'plugin_manager_mark_disposing();' in function('plugin_manager_deinit')
assert 'plugin_progress_reset();' in function('plugin_manager_deinit')
for name in ('show_progress', 'update_progress', 'close_progress', 'get_download_progress'):
    assert re.search(r'\{ "' + name + r'",\s+l_plugin_' + name + r' \}', src)
with tempfile.TemporaryDirectory(prefix='compas-progress-api-') as directory:
    directory = pathlib.Path(directory)
    c = directory / 'test.c'; binary = directory / 'test'; c.write_text(body)
    lua_sources = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DLUA_USE_LINUX',
                    '-I' + str(root / 'lua/src'), str(c), *lua_sources, '-lm', '-ldl', '-pthread', '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
