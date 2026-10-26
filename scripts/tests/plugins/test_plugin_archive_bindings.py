#!/usr/bin/env python3
"""Run the production ZIP/image plugin bindings with real Lua and native doubles."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]

fixture = r'''#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "plugin_lua_runtime.h"
#include "bindings/plugin_archive_bindings.h"
#include "zip_reader.h"
#include "image_thumb.h"
#include "startup_splash.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fail_worker, block_writer, fail_image, callback_throws;
static atomic_bool writer_entered;
static int zip_name_frees, callbacks, cleanup_runs;
typedef struct { lua_State * L; bool aborted; } test_instance_t;
static test_instance_t instances[2];
static test_instance_t * instance_for_state(lua_State * L) {
    for (int i=0;i<2;i++) if (instances[i].L == L) return &instances[i];
    return NULL;
}

static int test_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
#define pthread_create test_pthread_create
#include "bindings/plugin_archive_bindings.c"
#undef pthread_create
static int test_pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                               void *(*start)(void *), void * arg) {
    if (fail_worker) { fail_worker = false; return EAGAIN; }
    return pthread_create(thread, attr, start, arg);
}

bool plugin_storage_path_is_reserved(const char * path) {
    return path && strncmp(path, "RESERVED", 8) == 0;
}
static zip_status zip_result = ZIP_OK;
zip_status zip_read_entry(const char * path, const char * entry, unsigned char ** out,
                          size_t * len, zip_entry_info * info) {
    (void)path; (void)entry;
    if (zip_result != ZIP_OK) return zip_result;
    *out=malloc(7); assert(*out); memcpy(*out,"payload",7); *len=7;
    info->method=8; info->compressed=3; info->uncompressed=7; return ZIP_OK;
}
zip_status zip_read_entry_limited(const char * path, const char * entry, uint32_t max_compressed,
                                  uint32_t max_uncompressed, unsigned char ** out, size_t * len,
                                  zip_entry_info * info) {
    (void)max_compressed; (void)max_uncompressed;
    return zip_read_entry(path,entry,out,len,info);
}
zip_status zip_list_entries(const char * path, char *** names, size_t * count) {
    (void)path; if(zip_result!=ZIP_OK) return zip_result;
    *names=calloc(2,sizeof(char*)); assert(*names);
    (*names)[0]=strdup("cover.jpg"); (*names)[1]=strdup("chapter.txt"); *count=2; return ZIP_OK;
}
void zip_free_names(char ** names,size_t count) {
    zip_name_frees+=(int)count; for(size_t i=0;i<count;i++) free(names[i]); free(names);
}
const char * zip_status_reason(zip_status status) { return status==ZIP_ERR_IO?"io":"missing"; }
static bool image_write(const uint8_t * data,size_t len,int width,int height,const char * dest,
                        artwork_cancel_fn cancel,void * user,const char ** reason) {
    (void)width; (void)height; (void)dest; atomic_store(&writer_entered,true);
    for(int i=0;i<100 && block_writer;i++) {
        if(cancel && cancel(user)) { *reason="cancelled"; return false; }
        usleep(1000);
    }
    if(fail_image || (data && len && data[0]=='!')) { *reason="decode failed"; return false; }
    return true;
}
bool image_thumb_write_bin(const uint8_t *d,size_t n,int w,int h,const char *p,artwork_priority_t pr,
                           artwork_cancel_fn c,void*u,const char**r) {
    (void)pr; return image_write(d,n,w,h,p,c,u,r);
}
bool image_wallpaper_write_bin(const uint8_t *d,size_t n,int w,int h,const char *p,artwork_priority_t pr,
                               artwork_cancel_fn c,void*u,const char**r) {
    (void)pr; return image_write(d,n,w,h,p,c,u,r);
}
bool startup_splash_install(const char * path,int width,int height,const char ** reason) {
    (void)path; (void)width; (void)height; (void)reason; return true;
}
bool startup_splash_configured(void) { return true; }
lv_display_t * lv_display_get_default(void) { return NULL; }
int32_t lv_display_get_horizontal_resolution(const lv_display_t * display) { (void)display; return 240; }
int32_t lv_display_get_vertical_resolution(const lv_display_t * display) { (void)display; return 240; }

static int guarded_call(lua_State * L,int nargs,int nresults,int errfunc) {
    return lua_pcall(L,nargs,nresults,errfunc);
}
static bool owner_aborted(lua_State * L) {
    test_instance_t * inst=instance_for_state(L); return inst && inst->aborted;
}
static void bind(lua_State * L,const char * name,lua_CFunction fn) {
    lua_pushcfunction(L,fn); lua_setglobal(L,name);
}
static void run(lua_State * L,const char * code) {
    int rc=luaL_dostring(L,code);
    if(rc!=LUA_OK) fprintf(stderr,"Lua error: %s\n",lua_tostring(L,-1));
    assert(rc==LUA_OK);
}
static int callback(lua_State * L) {
    callbacks++;
    if(callback_throws) return luaL_error(L,"callback marker");
    return 0;
}
static int raise_marshalling(lua_State * L) { return luaL_error(L,"marshal marker"); }
static void cleanup_count(void * p) { (*(int *)p)++; }
static int call_cleanup_helper(lua_State * L) {
    int * count=lua_touserdata(L,lua_upvalueindex(1));
    lua_pushcfunction(L,raise_marshalling);
    return plugin_lua_pcall_then_cleanup(L,count,1,cleanup_count,count);
}
static void poll_until_done(void) {
    for(int i=0;i<3000 && plugin_archive_bindings_has_work();i++) {
        plugin_archive_bindings_poll(guarded_call,owner_aborted); usleep(1000);
    }
    plugin_archive_bindings_poll(guarded_call,owner_aborted);
    assert(!plugin_archive_bindings_has_work());
}
int main(void) {
    lua_State * L=luaL_newstate(); assert(L); luaL_openlibs(L); instances[0].L=L;
    bind(L,"zip_read",l_plugin_zip_read); bind(L,"zip_list",l_plugin_zip_list);
    bind(L,"zip_image",l_plugin_zip_image_async); bind(L,"thumb",l_plugin_image_thumbnail_async);
    bind(L,"wallpaper",l_plugin_image_wallpaper_async); bind(L,"startup",l_plugin_image_startup_async);
    bind(L,"set_startup",l_plugin_set_startup_image_async); bind(L,"has_startup",l_plugin_has_startup_image);
    bind(L,"callback",callback);
    lua_pushstring(L,TEST_IMAGE); lua_setglobal(L,"image_path");
    lua_pushstring(L,TEST_DIR); lua_setglobal(L,"out_dir");

    run(L,"local d,m=zip_read('a.zip','cover.jpg'); assert(d=='payload' and m.method==8 and m.compressed==3 and m.uncompressed==7)");
    run(L,"local a=zip_list('a.zip'); assert(#a==2 and a[1]=='cover.jpg' and a[2]=='chapter.txt')");
    assert(zip_name_frees==2);
    run(L,"local ok,e=pcall(zip_read,'RESERVED/a.zip','x'); assert(not ok and e:find('path is reserved'))");
    zip_result=ZIP_ERR_IO;
    run(L,"local d,e=zip_read('a.zip','bad'); assert(d==nil and e=='io'); local n,why=zip_list('a.zip'); assert(n==nil and why=='io')");
    zip_result=ZIP_OK;

    run(L,"assert(has_startup()); assert(zip_image('a.zip','cover.jpg',out_dir..'/zip.bin',64,64,callback))");
    poll_until_done(); assert(callbacks==1);
    run(L,"assert(thumb(image_path,out_dir..'/thumb.bin',64,64,callback))"); poll_until_done(); assert(callbacks==2);
    run(L,"assert(wallpaper(image_path,out_dir..'/wall.bin',callback))"); poll_until_done(); assert(callbacks==3);
    run(L,"assert(startup(image_path,out_dir..'/start.bin',callback))"); poll_until_done(); assert(callbacks==4);
    run(L,"assert(set_startup(image_path,callback))"); poll_until_done(); assert(callbacks==5);

    /* An absent plugin instance is not treated as aborted by poll. */
    instances[0].L=NULL;
    run(L,"assert(thumb(image_path,out_dir..'/missing-instance.bin',64,64,callback))");
    poll_until_done(); assert(callbacks==6); instances[0].L=L;

    run(L,"assert(not pcall(zip_image,'a.zip','x',out_dir..'/bad.png',64,64,callback)); assert(not pcall(thumb,'RESERVED/a',out_dir..'/ok.bin',64,64,callback)); assert(not pcall(wallpaper,image_path,out_dir..'/bad.png',callback)); assert(not pcall(startup,image_path,out_dir..'/bad.png',callback))");
    run(L,"assert(set_startup(nil,callback))"); poll_until_done(); assert(callbacks==7);

    fail_image=true;
    run(L,"assert(zip_image('a.zip','cover.jpg',out_dir..'/fail.bin',64,64,callback))");
    poll_until_done(); fail_image=false; assert(callbacks==8);

    callback_throws=true;
    run(L,"assert(thumb(image_path,out_dir..'/throw.bin',64,64,callback))"); poll_until_done(); callback_throws=false;
    assert(callbacks==9 && lua_gettop(L)==0);

    /* Worker creation failure unrefs on the calling state and leaves no job. */
    fail_worker=true;
    run(L,"local ok,e=thumb(image_path,out_dir..'/thread-fail.bin',64,64,callback); assert(ok==nil and e=='could not start image worker')");
    assert(!plugin_archive_bindings_has_work() && plugin_zip_image.L==NULL && plugin_zip_image.callback_ref==LUA_NOREF);

    /* Raised marshalling errors still run the borrowed cleanup callback. */
    lua_pushlightuserdata(L,&cleanup_runs); lua_pushcclosure(L,call_cleanup_helper,1);
    assert(lua_pcall(L,0,0,0)!=LUA_OK && cleanup_runs==1); lua_pop(L,1);

    /* Owner cancellation joins, suppresses callback, and releases a valid ref. */
    block_writer=true; atomic_store(&writer_entered,false);
    run(L,"assert(thumb(image_path,out_dir..'/cancel.bin',64,64,callback))");
    for(int i=0;i<3000 && !atomic_load(&writer_entered);i++) usleep(1000);
    assert(atomic_load(&writer_entered));
    int cancelled_ref=plugin_zip_image.callback_ref;
    plugin_archive_bindings_clear_owner(plugin_lua_main_state(L),false);
    assert(!plugin_archive_bindings_has_work() && atomic_load(&plugin_zip_image.done));
    lua_rawgeti(L,LUA_REGISTRYINDEX,cancelled_ref); assert(!lua_isfunction(L,-1)); lua_pop(L,1);
    assert(callbacks==9); block_writer=false;

    /* Hard-aborted owner cancellation skips every Lua registry operation. */
    instances[0].aborted=true;
    run(L,"assert(thumb(image_path,out_dir..'/aborted-cancel.bin',64,64,callback))");
    plugin_archive_bindings_clear_owner(plugin_lua_main_state(L),true);
    assert(!plugin_archive_bindings_has_work());
    instances[0].aborted=false;

    /* Poll also skips the registry for a hard-aborted owner. */
    run(L,"assert(thumb(image_path,out_dir..'/aborted-poll.bin',64,64,callback))");
    for(int i=0;i<3000 && !atomic_load(&plugin_zip_image.done);i++) usleep(1000);
    assert(atomic_load(&plugin_zip_image.done)); instances[0].aborted=true;
    plugin_archive_bindings_poll(guarded_call,owner_aborted);
    assert(!plugin_archive_bindings_has_work() && callbacks==9); instances[0].aborted=false;

    /* Deinit cancellation joins before clearing the final active request. */
    block_writer=true; atomic_store(&writer_entered,false);
    run(L,"assert(thumb(image_path,out_dir..'/deinit.bin',64,64,callback))");
    for(int i=0;i<3000 && !atomic_load(&writer_entered);i++) usleep(1000);
    assert(atomic_load(&writer_entered));
    plugin_archive_bindings_cancel_all(owner_aborted);
    assert(!plugin_archive_bindings_has_work() && atomic_load(&plugin_zip_image.done));
    assert(callbacks==9);

    lua_close(L);
    puts("production archive/image Lua bindings passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="plugin-archive-bindings-") as folder_name:
    folder = Path(folder_name)
    image_path = folder / "input image.jpg"
    image_path.write_bytes(b"payload")
    c_file = folder / "archive_bindings_test.c"
    binary = folder / "archive_bindings_test"
    c_file.write_text(f'#define TEST_DIR {json.dumps(str(folder))}\n#define TEST_IMAGE {json.dumps(str(image_path))}\n' + fixture)
    lua_sources = sorted(p for p in (root / "lua/src").glob("*.c") if p.name not in ("lua.c", "luac.c"))
    includes = [root, root / "lua/src", root / "src/plugins", root / "src/plugins/bindings",
                root / "src/ui", root / "src/core",
                root / "src/library", root / "src/audio", root / "src/hardware", root / "lvgl", root / "lvgl/src"]
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-DLUA_USE_LINUX", "-DHOST_BUILD=1"]
    command.extend(shlex.split(os.environ.get("PLUGIN_ARCHIVE_BINDING_CFLAGS", "")))
    command.extend("-I" + str(p) for p in includes)
    command.extend([str(c_file), str(root / "src/plugins/lua/plugin_lua_runtime.c"), *map(str, lua_sources),
                    "-lm", "-ldl", "-pthread", "-o", str(binary)])
    subprocess.run(command, check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=folder)
