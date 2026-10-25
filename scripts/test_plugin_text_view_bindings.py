#!/usr/bin/env python3
"""Exercise production text-view Lua binding and lifecycle code with real Lua."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
manager = (root / "src/plugins/plugin_manager.c").read_text()
binding_wrapper = re.search(
    r"static int l_plugin_show_text_view\(lua_State \* L\) \{.*?^\}",
    manager, re.M | re.S)
assert binding_wrapper and "plugin_text_view_bindings_show(L, plugin_call, plugin_text_view_owner_is_aborted)" in binding_wrapper.group()
assert '{ "show_text_view",            l_plugin_show_text_view }' in manager
rollback_start = manager.index("static void discard_failed_plugin_resources(lua_State * L, bool aborted) {")
rollback_end = manager.index("/* Discards failed plugin instance", rollback_start)
assert "plugin_text_view_bindings_clear_owner(L, aborted);" in manager[rollback_start:rollback_end]
deinit_start = manager.index("void plugin_manager_deinit(void)")
assert manager.index("plugin_text_view_bindings_clear_all();", deinit_start) < manager.index(
    "lua_close(plugin_instances[i].L)", deinit_start)
register_start = manager.index("static void register_plugin_lib(")
assert "lua_pushcclosure(L, l_plugin_api_guard, 1);" in manager[register_start:]

c_source = r'''
#define _GNU_SOURCE
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "gui_text_view.h"
#include "plugin_lua_runtime.h"
#include <stdbool.h>
bool plugin_storage_path_is_reserved(const char *path) { (void)path; return false; }
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define LUA_NOREF (-2)
typedef struct { long allocations; long fail_at; bool fail_enabled; } alloc_ctl_t;
static void *test_alloc(void *ud,void *ptr,size_t oldsz,size_t newsz) {
    (void)oldsz; alloc_ctl_t *ctl=ud;
    if(!newsz) { free(ptr); return NULL; }
    ctl->allocations++;
    if(ctl->fail_enabled && ctl->allocations>=ctl->fail_at) return NULL;
    return realloc(ptr,newsz);
}
typedef struct { lua_State *L; bool aborted; } plugin_instance_t;
static plugin_instance_t instances[4];
static plugin_instance_t *plugin_instance_for_state(lua_State *L) {
    for (int i=0;i<4;i++) if (instances[i].L==L) return &instances[i];
    return NULL;
}
static bool test_owner_is_aborted(lua_State *L) {
    plugin_instance_t *inst=plugin_instance_for_state(L); return inst && inst->aborted;
}
static int guarded_calls;
static int plugin_call(lua_State *L,int nargs,int nresults,int errfunc) {
    guarded_calls++; return lua_pcall(L,nargs,nresults,errfunc);
}
static bool ui_open, ui_available=true, synchronous_callback_seen;
static int shown_count;
static char saved_title[128], saved_text[4096];
static gui_text_view_opts saved_opts;
static char saved_images[GUI_TEXT_VIEW_MAX_IMAGES][512];
static gui_text_view_turn_fn saved_turn[16];
static gui_text_view_close_fn saved_close[16];
static void *saved_user[16];
static int active_view;
bool gui_text_view_is_open(void) { return ui_open; }
bool gui_text_view_show(const char *title,const void *text,size_t text_len,
    const gui_text_view_opts *opts,gui_text_view_turn_fn turn,gui_text_view_close_fn close,void *user) {
    assert(!ui_open);
    assert(!turn || !synchronous_callback_seen);
    snprintf(saved_title,sizeof(saved_title),"%s",title);
    size_t n=text_len<sizeof(saved_text)-1?text_len:sizeof(saved_text)-1;
    memcpy(saved_text,text,n); saved_text[n]='\0';
    saved_opts=opts?*opts:(gui_text_view_opts){0};
    if (opts && opts->image_count>0) {
        for (int i=0;i<opts->image_count;i++) snprintf(saved_images[i],sizeof(saved_images[i]),"%s",opts->images[i]);
        saved_opts.images=(const char * const *)saved_images;
    }
    active_view=shown_count++; saved_turn[active_view]=turn; saved_close[active_view]=close; saved_user[active_view]=user;
    if (!ui_available) return false;
    ui_open=true; return true;
}
static void fire_turn(int page,int pages,size_t offset) {
    assert(saved_turn[active_view]); saved_turn[active_view](page,pages,offset,saved_user[active_view]);
}
static void fire_close_at(int index,int page,size_t offset) {
    gui_text_view_close_fn fn=saved_close[index]; void *user=saved_user[index];
    ui_open=false; if(fn) fn(page,offset,user);
}
static void fire_close(int page,size_t offset) { fire_close_at(active_view,page,offset); }
static int defer_close(void) { ui_open=false; return active_view; }

static int monitored_luaL_ref(lua_State *, int);
#define luaL_ref monitored_luaL_ref
#include "bindings/plugin_text_view_bindings.c"
#undef luaL_ref
static int l_plugin_show_text_view(lua_State *L) {
    return plugin_text_view_bindings_show(L, plugin_call, test_owner_is_aborted);
}
static int ref_attempts, ref_successes;
static int monitored_luaL_ref(lua_State *L,int t) {
    ref_attempts++; int ref=luaL_ref(L,t); ref_successes++; return ref;
}
static int registry_function_refs(lua_State *L) {
    int n=0; lua_pushnil(L);
    while(lua_next(L,LUA_REGISTRYINDEX)) {
        if(lua_isinteger(L,-2) && lua_tointeger(L,-2)>2 && lua_isfunction(L,-1)) n++;
        lua_pop(L,1);
    }
    return n;
}
static void bind(lua_State *L) {
    lua_pushcfunction(L,l_plugin_show_text_view); lua_setglobal(L,"show_text_view");
}
static void run(lua_State *L,const char *code) {
    int rc=luaL_dostring(L,code);
    if(rc!=LUA_OK) fprintf(stderr,"lua failure: %s\n",lua_tostring(L,-1));
    assert(rc==LUA_OK);
}
static int live_slots(void) { int n=0; for(int i=0;i<TEXT_VIEW_CTX_MAX;i++) n+=text_view_ctxs[i].live; return n; }
static void assert_ref_dead(lua_State *L,int ref) { lua_rawgeti(L,LUA_REGISTRYINDEX,ref); assert(!lua_isfunction(L,-1)); lua_pop(L,1); }
static void test_oom_partial_ref_cleanup(void) {
    bool observed_partial_ref_failure=false;
    for(int fill=0;fill<=512 && !observed_partial_ref_failure;fill++) {
      for(long budget=0;budget<8 && !observed_partial_ref_failure;budget++) {
        alloc_ctl_t ctl={0}; lua_State *L=lua_newstate(test_alloc,&ctl,0); assert(L); luaL_openlibs(L);
        instances[3]=(plugin_instance_t){L,false}; bind(L);
        run(L,"turn_cb=function() end; close_cb=function() end");
        for(int i=0;i<fill;i++) { lua_pushinteger(L,i); (void)luaL_ref(L,LUA_REGISTRYINDEX); }
        int before=registry_function_refs(L);
        lua_getglobal(L,"show_text_view"); lua_pushstring(L,"oom"); lua_pushstring(L,"payload");
        lua_createtable(L,0,2); lua_getglobal(L,"turn_cb"); lua_setfield(L,-2,"on_turn");
        lua_getglobal(L,"close_cb"); lua_setfield(L,-2,"on_close");
        ctl.fail_at=ctl.allocations+budget+1; ctl.fail_enabled=true; ref_attempts=ref_successes=0;
        int rc=lua_pcall(L,3,2,0); ctl.fail_enabled=false;
        if(rc!=LUA_OK && ref_attempts==2 && ref_successes==1) {
            assert(rc==LUA_ERRMEM); assert(live_slots()==0); assert(!ui_open);
            assert(registry_function_refs(L)==before);
            observed_partial_ref_failure=true;
            fprintf(stderr,"partial ref OOM characterized (filler=%d, alloc budget=%ld)\n",fill,budget);
        }
        lua_settop(L,0); plugin_text_view_bindings_clear_all(); ui_open=false;
        lua_close(L); instances[3].L=NULL;
      }
    }
    assert(observed_partial_ref_failure && "allocator sweep must hit second callback ref after first ref succeeds");
}
int main(void) {
    lua_State *a=luaL_newstate(),*b=luaL_newstate(); assert(a&&b);
    luaL_openlibs(a); luaL_openlibs(b); instances[0]=(plugin_instance_t){a,false}; instances[1]=(plugin_instance_t){b,false}; bind(a); bind(b);
    run(a,"assert(not pcall(show_text_view,true,'x')); assert(not pcall(show_text_view,'bad','x',5)); assert(not pcall(show_text_view,'bad','x',{images='not a list'})); assert(not pcall(show_text_view,'bad','x',{on_turn=true})); assert(not pcall(show_text_view,'bad','x',{on_close=true}))");
    assert(live_slots()==0&&!ui_open);
    run(a,"turns=0; closes=0; show_calls=0; co=coroutine.create(function() assert(show_text_view('reader','abc',{page=3,offset=42,font_px=80,images={'cover.bin'},on_turn=function(p,ps,o) turns=turns+1; assert(p==4 and ps==8 and o==123) end,on_close=function(p,o) closes=closes+1; assert(p==4 and o==123) end})) end); assert(coroutine.resume(co))");
    assert(ui_open&&shown_count==1&&live_slots()==1&&!strcmp(saved_title,"reader")&&!strcmp(saved_text,"abc"));
    assert(saved_opts.has_page&&saved_opts.page==3&&saved_opts.has_offset&&saved_opts.offset==42&&saved_opts.image_count==1&&!strcmp(saved_images[0],"cover.bin"));
    int slot=-1;for(int i=0;i<TEXT_VIEW_CTX_MAX;i++)if(text_view_ctxs[i].live)slot=i;assert(slot>=0&&text_view_ctxs[slot].L==a);
    int turn_ref=text_view_ctxs[slot].turn_ref, close_ref=text_view_ctxs[slot].close_ref; assert(turn_ref!=LUA_NOREF&&close_ref!=LUA_NOREF);
    fire_turn(4,8,123);run(a,"assert(turns==1)");assert(guarded_calls==1);
    fire_close(4,123);run(a,"assert(closes==1)");assert(live_slots()==0&&guarded_calls==2);assert_ref_dead(a,turn_ref);assert_ref_dead(a,close_ref);
    /* Duplicate delivery while the released slot remains unoccupied is stale-safe. */
    text_view_close_trampoline(9,999,&text_view_ctxs[slot]); assert(live_slots()==0&&guarded_calls==2);

    /* Callback failures are protected/logged and still allow normal close cleanup. */
    run(a,"assert(show_text_view('callback-error','x',{on_turn=function() error('expected turn failure') end,on_close=function() error('expected close failure') end}))");
    fire_turn(1,2,3); assert(live_slots()==1); fire_close(1,2); assert(live_slots()==0);

    /* UI construction failure releases both refs and makes this slot reusable. */
    int refs_before_ctor=registry_function_refs(a);
    ui_available=false;run(a,"assert(not show_text_view('fail','x',{on_turn=function()end,on_close=function()end}))");
    assert(registry_function_refs(a)==refs_before_ctor);
    assert(live_slots()==0&&!ui_open);ui_available=true;
    run(a,"assert(show_text_view('reuse','x',{on_close=function() end}))");assert(live_slots()==1);
    fire_close(1,0);assert(live_slots()==0);

    /* During the GUI slide-out, a replacement view can open before the old
     * queued close trampoline arrives. The old slot must remain occupied. */
    run(a,"assert(show_text_view('closing','x',{on_close=function() closes=closes+1 end}))");
    int old_queued_view=defer_close(); assert(live_slots()==1);
    run(a,"assert(show_text_view('replacement','x',{on_close=function() closes=closes+1 end}))");
    assert(live_slots()==2); int replacement_view=active_view;
    fire_close_at(old_queued_view,5,6); assert(live_slots()==1);
    int closes_before_stale=0; lua_getglobal(a,"closes"); closes_before_stale=(int)lua_tointeger(a,-1); lua_pop(a,1);
    fire_close_at(old_queued_view,5,6); assert(live_slots()==1);
    lua_getglobal(a,"closes"); assert(lua_tointeger(a,-1)==closes_before_stale); lua_pop(a,1);
    fire_close_at(replacement_view,1,2); assert(live_slots()==0);

    /* A busy UI doesn't allocate callback refs or consume slots. */
    ui_open=true;run(a,"local ok,err=show_text_view('busy','x',{on_close=function()end}); assert(ok==false and err=='busy')");assert(live_slots()==0);ui_open=false;

    /* Runtime-hard-aborted state is checked before stack/registry access. */
    run(b,"assert(show_text_view('abort','x',{on_turn=function() error('must not run') end,on_close=function() error('must not run') end}))");
    int aborted_slot=-1;for(int i=0;i<TEXT_VIEW_CTX_MAX;i++)if(text_view_ctxs[i].live)aborted_slot=i;assert(aborted_slot>=0);
    int aborted_turn=text_view_ctxs[aborted_slot].turn_ref; instances[1].aborted=true; int top=lua_gettop(b), calls_before_abort=guarded_calls;
    fire_turn(2,3,4);assert(lua_gettop(b)==top&&guarded_calls==calls_before_abort);
    plugin_text_view_bindings_clear_owner(b,true);assert(text_view_ctxs[aborted_slot].live&&text_view_ctxs[aborted_slot].L==NULL);
    lua_rawgeti(b,LUA_REGISTRYINDEX,aborted_turn);assert(lua_isfunction(b,-1));lua_pop(b,1);
    fire_close(2,4);assert(live_slots()==0); /* tombstone retires without touching aborted L */

    /* Normal failed-load cleanup unreferences callbacks but keeps the slot live until close. */
    instances[1].aborted=false;
    run(b,"assert(show_text_view('rollback','x',{on_turn=function()end,on_close=function()end}))");
    int owner_slot=-1;for(int i=0;i<TEXT_VIEW_CTX_MAX;i++)if(text_view_ctxs[i].live)owner_slot=i;assert(owner_slot>=0);
    int owner_turn=text_view_ctxs[owner_slot].turn_ref, owner_close=text_view_ctxs[owner_slot].close_ref;
    plugin_text_view_bindings_clear_owner(b,false);assert(text_view_ctxs[owner_slot].live&&text_view_ctxs[owner_slot].L==NULL);
    assert_ref_dead(b,owner_turn);assert_ref_dead(b,owner_close);fire_close(2,4);assert(live_slots()==0);

    /* Clear-all (deinit) unreferences all remaining callbacks while L is valid. */
    run(a,"assert(show_text_view('deinit','x',{on_turn=function()end,on_close=function()end}))");
    int deinit_slot=-1;for(int i=0;i<TEXT_VIEW_CTX_MAX;i++)if(text_view_ctxs[i].live)deinit_slot=i;assert(deinit_slot>=0);
    int deinit_turn=text_view_ctxs[deinit_slot].turn_ref;plugin_text_view_bindings_clear_all();assert(live_slots()==0);assert_ref_dead(a,deinit_turn);
    test_oom_partial_ref_cleanup();lua_close(a);lua_close(b);puts("plugin text-view binding lifecycle passed");return 0;
}

'''

with tempfile.TemporaryDirectory(prefix="compas-plugin-text-view-") as directory:
    directory = Path(directory)
    c_file, binary = directory / "test.c", directory / "test"
    c_file.write_text(c_source)
    lua_sources = [str(path) for path in (root / "lua/src").glob("*.c")
                   if path.name not in ("lua.c", "luac.c")]
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-DLUA_USE_LINUX",
                    "-I" + str(root), "-I" + str(root / "lua/src"),
                    "-I" + str(root / "src/ui"), "-I" + str(root / "src/plugins"),
                    "-I" + str(root / "src/plugins/bindings"),
                    "-I" + str(root / "lvgl"), "-I" + str(root / "lvgl/src"),
                    str(c_file), str(root / "src/plugins/plugin_lua_runtime.c"), *lua_sources,
                    "-lm", "-ldl", "-pthread", "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
