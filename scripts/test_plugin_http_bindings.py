#!/usr/bin/env python3
"""Characterize async plugin HTTP bindings with real Lua and deterministic transports."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]

c_source = r'''#define _GNU_SOURCE
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "http_client.h"
#include "bindings/plugin_http_bindings.c"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;
static bool fake_started, fake_release, fake_error, fake_download_failure;
static atomic_int guarded_calls;
static bool fail_next_create;

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    if (fail_next_create) { fail_next_create = false; return EAGAIN; }
    return __real_pthread_create(thread, attr, start_routine, arg);
}

static int test_guarded_call(lua_State *L, int nargs, int nresults, int errfunc) {
    atomic_fetch_add(&guarded_calls, 1);
    return lua_pcall(L, nargs, nresults, errfunc);
}

bool plugin_storage_path_is_reserved(const char *path) { (void)path; return false; }
void http_cancel_token_init(http_cancel_token_t *tok) {
    pthread_mutex_init(&tok->mutex, NULL); tok->fd = -1; tok->cancel_requested = false;
}
void http_cancel_token_destroy(http_cancel_token_t *tok) { pthread_mutex_destroy(&tok->mutex); }
void http_cancel_token_cancel(http_cancel_token_t *tok) {
    pthread_mutex_lock(&tok->mutex); tok->cancel_requested = true; pthread_mutex_unlock(&tok->mutex);
    pthread_mutex_lock(&fake_mutex); pthread_cond_broadcast(&fake_cond); pthread_mutex_unlock(&fake_mutex);
}
bool http_cancel_token_is_cancelled(http_cancel_token_t *tok) {
    pthread_mutex_lock(&tok->mutex); bool cancelled = tok->cancel_requested; pthread_mutex_unlock(&tok->mutex);
    return cancelled;
}
static void fake_wait(http_cancel_token_t *cancel) {
    pthread_mutex_lock(&fake_mutex);
    fake_started = true; pthread_cond_broadcast(&fake_cond);
    while (!fake_release && !http_cancel_token_is_cancelled(cancel)) pthread_cond_wait(&fake_cond, &fake_mutex);
    pthread_mutex_unlock(&fake_mutex);
}
static void reset_fake(void) {
    pthread_mutex_lock(&fake_mutex); fake_started = false; fake_release = false; fake_error = false;
    pthread_mutex_unlock(&fake_mutex);
}
static void wait_started(void) {
    pthread_mutex_lock(&fake_mutex); while (!fake_started) pthread_cond_wait(&fake_cond, &fake_mutex);
    pthread_mutex_unlock(&fake_mutex);
}
static void release_request(void) {
    pthread_mutex_lock(&fake_mutex); fake_release = true; pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_mutex);
}
static void wait_done(int slot) {
    while (!atomic_load(&plugin_async_http[slot].done)) sched_yield();
}
bool http_request_ex(const http_request_t *request, http_cancel_token_t *cancel, http_response_t *response) {
    (void)request; memset(response, 0, sizeof(*response)); fake_wait(cancel);
    if (http_cancel_token_is_cancelled(cancel)) { response->error = HTTP_ERR_CANCELLED; return false; }
    if (fake_error) { response->error = HTTP_ERR_TIMEOUT; return false; }
    response->status = 202; response->body = (uint8_t *)strdup("response"); response->body_len = 8;
    response->header_count = 3;
    strcpy(response->headers[0].name, "X-Test"); strcpy(response->headers[0].value, "old");
    strcpy(response->headers[1].name, "x-test"); strcpy(response->headers[1].value, "new");
    strcpy(response->headers[2].name, "Content-Type"); strcpy(response->headers[2].value, "text/plain");
    response->error = HTTP_ERR_NONE; return true;
}
bool http_get_to_file_bounded(const char *url, bool verify_tls, const char *dest_path, size_t max_body_size,
                              http_progress_cb_t progress_cb, void *progress_user_data) {
    (void)url; (void)verify_tls; (void)max_body_size;
    progress_cb(5, 10, progress_user_data);
    FILE *file = fopen(dest_path, "wb"); if (!file) return false;
    fputs(fake_download_failure ? "partial" : "complete", file); fclose(file);
    return !fake_download_failure;
}
static void run(lua_State *L, const char *code) {
    int status = luaL_dostring(L, code);
    if (status != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
}
static int request_handle(lua_State *L, const char *global) {
    lua_getglobal(L, global); assert(lua_isinteger(L, -1)); int handle=(int)lua_tointeger(L,-1); lua_pop(L,1); return handle;
}
static void bind(lua_State *L) {
    luaL_openlibs(L);
    lua_pushcfunction(L, l_plugin_http_request); lua_setglobal(L, "request");
    lua_pushcfunction(L, l_plugin_download_file_async); lua_setglobal(L, "download");
    lua_pushcfunction(L, l_plugin_get_download_progress); lua_setglobal(L, "progress");
    lua_pushcfunction(L, l_plugin_cancel); lua_setglobal(L, "cancel");
}
static void assert_no_partial(const char *dest) {
    char pattern[PATH_MAX]; snprintf(pattern,sizeof(pattern),"%s.part.*",dest);
    glob_t matches; memset(&matches,0,sizeof(matches));
    int rc=glob(pattern,0,NULL,&matches); assert(rc==GLOB_NOMATCH); globfree(&matches);
}
int main(int argc, char **argv) {
    assert(argc==3); const char *dest_ok=argv[1], *dest_fail=argv[2];
    unlink(dest_ok); unlink(dest_fail);
    lua_State *a=luaL_newstate(), *b=luaL_newstate(); assert(a&&b); bind(a); bind(b);

    reset_fake(); run(a,"request_calls=0; handle=request({url='https://fixture.test/good',headers={A='b'}},function(status,body,err,headers) request_calls=request_calls+1; assert(status==202 and body=='response' and err==nil and headers['x-test']=='new' and headers['Content-Type']=='text/plain') end)");
    wait_started(); release_request(); wait_done(0); plugin_http_bindings_poll(test_guarded_call);
    run(a,"assert(request_calls==1)"); assert(!plugin_async_http[0].active); assert(atomic_load(&guarded_calls)==1);

    reset_fake(); run(a,"co_called=false; co=coroutine.create(function() request({url='https://fixture.test/co'},function(status,body,err) co_called=true; assert(status==202 and body=='response' and err==nil) end) end); assert(coroutine.resume(co))");
    wait_started(); release_request(); wait_done(0); plugin_http_bindings_poll(test_guarded_call); run(a,"assert(co_called)");

    reset_fake(); fake_error=true; run(a,"error_called=false; request({url='https://fixture.test/error'},function(status,body,err) error_called=true; assert(status==nil and body==nil and err=='timeout') end)");
    wait_started(); release_request(); wait_done(0); plugin_http_bindings_poll(test_guarded_call); run(a,"assert(error_called)");

    reset_fake(); run(a,"cancel_called=false; cancel_handle=request({url='https://fixture.test/cancel'},function() cancel_called=true end)");
    wait_started(); int handle=request_handle(a,"cancel_handle"); lua_pushinteger(b,handle); lua_setglobal(b,"foreign_handle");
    run(b,"assert(cancel(foreign_handle)==true)"); wait_done(0); plugin_http_bindings_poll(test_guarded_call); run(a,"assert(cancel_called==false)");
    assert(!plugin_async_http[0].active);

    fake_download_failure=false;
    char lua_download[2048]; snprintf(lua_download,sizeof(lua_download),"download_path=nil; download_error='unset'; d=download('https://fixture.test/file','%s',function(path,err) download_path=path; download_error=err end)",dest_ok);
    run(a,lua_download); wait_done(0); run(a,"p=progress(d); assert(p.downloaded==5 and p.total==10)");
    plugin_http_bindings_poll(test_guarded_call);
    char lua_check[2048]; snprintf(lua_check,sizeof(lua_check),"assert(download_path=='%s' and download_error==nil)",dest_ok); run(a,lua_check);
    struct stat st; assert(stat(dest_ok,&st)==0 && st.st_size==8); unlink(dest_ok);

    fake_download_failure=true;
    snprintf(lua_download,sizeof(lua_download),"download_path='unset'; download_error=nil; df=download('https://fixture.test/file','%s',function(path,err) download_path=path; download_error=err end)",dest_fail);
    run(a,lua_download); wait_done(0); plugin_http_bindings_poll(test_guarded_call);
    run(a,"assert(download_path==nil and download_error=='download failed')"); assert(access(dest_fail,F_OK)!=0); assert_no_partial(dest_fail);

    /* The Lua API returns nil,error for pthread failure; inspect the actual native slot too. */
    reset_fake(); fail_next_create=true;
    run(a,"request_start_error=nil; local h,e=request({url='https://fixture.test/start',body='body',headers={Authorization=string.rep('x',800)}},function() end); request_start_error=e; assert(h==nil and e=='could not start HTTP worker')");
    assert(!plugin_async_http[0].active && plugin_async_http[0].L==plugin_lua_main_state(a));
    assert(plugin_async_http[0].request_body==NULL && plugin_async_http[0].long_header_value==NULL);
    lua_rawgeti(a,LUA_REGISTRYINDEX,plugin_async_http[0].callback_ref); assert(!lua_isfunction(a,-1)); lua_pop(a,1);

    fail_next_create=true;
    run(a,"co=coroutine.create(function() local h,e=request({url='https://fixture.test/co-start'},function() end); assert(h==nil and e=='could not start HTTP worker') end); assert(coroutine.resume(co))");
    assert(!plugin_async_http[0].active && plugin_async_http[0].L==plugin_lua_main_state(a));
    lua_rawgeti(a,LUA_REGISTRYINDEX,plugin_async_http[0].callback_ref); assert(!lua_isfunction(a,-1)); lua_pop(a,1);

    char start_failure_lua[2048];
    snprintf(start_failure_lua,sizeof(start_failure_lua),
             "local h,e=download('https://fixture.test/start','%s',function() end); assert(h==nil and e=='could not start HTTP worker')",
             dest_fail);
    fail_next_create=true; run(a,start_failure_lua);
    assert(!plugin_async_http[0].active && plugin_async_http[0].L==NULL);
    assert(plugin_async_http[0].callback_ref==LUA_NOREF);

    reset_fake(); run(a,"owner_handle=request({url='https://fixture.test/owner'},function() error('not dispatched') end)"); wait_started();
    int normal_ref=plugin_async_http[0].callback_ref; plugin_http_bindings_clear_owner(plugin_lua_main_state(a),false);
    assert(!plugin_async_http[0].active); lua_rawgeti(a,LUA_REGISTRYINDEX,normal_ref); assert(!lua_isfunction(a,-1)); lua_pop(a,1);

    reset_fake(); run(b,"aborted_handle=request({url='https://fixture.test/aborted'},function() end)"); wait_started();
    int aborted_ref=plugin_async_http[0].callback_ref; plugin_http_bindings_clear_owner(plugin_lua_main_state(b),true);
    assert(!plugin_async_http[0].active); lua_rawgeti(b,LUA_REGISTRYINDEX,aborted_ref); assert(lua_isfunction(b,-1)); lua_pop(b,1);

    reset_fake(); run(a,"deinit_handle=request({url='https://fixture.test/deinit'},function() end)"); wait_started();
    int deinit_ref=plugin_async_http[0].callback_ref; plugin_http_bindings_cancel_all(); assert(!plugin_async_http[0].active);
    lua_rawgeti(a,LUA_REGISTRYINDEX,deinit_ref); assert(!lua_isfunction(a,-1)); lua_pop(a,1);
    assert(!plugin_http_bindings_has_work()); lua_close(a); lua_close(b);
    puts("plugin HTTP bindings passed"); return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='compas-plugin-http-') as folder:
    folder = pathlib.Path(folder)
    c_file = folder / 'test.c'
    binary = folder / 'test'
    c_file.write_text(c_source)
    lua_sources = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-DLUA_USE_LINUX',
                    '-Wl,--wrap=pthread_create', '-I' + str(root / 'lua/src'),
                    '-I' + str(root / 'src/network'), '-I' + str(root / 'src/plugins'),
                    '-I' + str(root / 'src/plugins/bindings'),
                    str(c_file), str(root / 'src/plugins/plugin_lua_runtime.c'), *lua_sources,
                    '-lm', '-ldl', '-pthread', '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary), str(folder / 'download-ok.bin'), str(folder / 'download-failed.bin')],
                   check=True, cwd=root)
