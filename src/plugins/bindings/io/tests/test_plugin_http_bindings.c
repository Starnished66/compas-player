#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "http/client/http_client.h"
#include "download/download.h"
#include "plugins/lua/plugin_lua_runtime.h"
#include "plugins/storage/plugin_storage.h"
#include "plugins/storage/plugin_reserved_dirs.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned tracked_allocations;
static void * tracked_malloc(size_t size) {
    tracked_allocations++;
    return malloc(size);
}
#define malloc tracked_malloc
#include "../plugin_http_bindings.c"
#undef malloc

static struct {
    char url[2048];
    char content_type[128];
    size_t body_len;
    unsigned char body_first;
} captured[PLUGIN_MAX_ASYNC_HTTP];
static atomic_uint captured_count;

lua_State * plugin_lua_main_state(lua_State * L) { return L; }
int plugin_lua_push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}
bool plugin_storage_path_is_reserved(const char * path) { (void) path; return false; }
bool plugin_path_in_program_dirs(const char * path) { (void) path; return false; }

void http_cancel_token_init(http_cancel_token_t * token) {
    pthread_mutex_init(&token->mutex, NULL);
    token->fd = -1;
    token->secondary_fd = -1;
    token->cancel_requested = false;
}
void http_cancel_token_destroy(http_cancel_token_t * token) { pthread_mutex_destroy(&token->mutex); }
bool http_cancel_token_is_cancelled(http_cancel_token_t * token) { (void) token; return false; }
void http_cancel_token_cancel(http_cancel_token_t * token) { (void) token; }
bool http_request_ex(const http_request_t * request, http_cancel_token_t * cancel, http_response_t * response) {
    (void) cancel;
    unsigned i = atomic_fetch_add(&captured_count, 1);
    assert(i < PLUGIN_MAX_ASYNC_HTTP);
    snprintf(captured[i].url, sizeof(captured[i].url), "%s", request->url);
    snprintf(captured[i].content_type, sizeof(captured[i].content_type), "%s", request->content_type);
    captured[i].body_len = request->body_len;
    captured[i].body_first = request->body_len ? request->body[0] : 0;
    memset(response, 0, sizeof(*response));
    response->status = 200;
    response->error = HTTP_ERR_NONE;
    return true;
}
download_error_t download_to_file(const download_request_t * request, http_cancel_token_t * cancel,
                                  download_progress_fn progress, void * user, download_result_t * result) {
    (void) request; (void) cancel; (void) progress; (void) user; (void) result;
    return DOWNLOAD_ERR_ARGUMENT;
}

int main(void) {
    lua_State * L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    lua_pushcfunction(L, l_plugin_http_request);
    lua_setglobal(L, "plugin_http_request");
    const char * lifetime_script =
        "local options=setmetatable({}, {__index=function(_,key)\n"
        "  if key=='url' then return 'http://example.invalid/plugin' end\n"
        "  if key=='method' then\n"
        "    local nested,err=plugin_http_request({url='http://example.invalid/nested'},function() end)\n"
        "    assert(nested,err); collectgarbage(); return 'POST'\n"
        "  end\n"
        "  if key=='body' then return string.rep('B',131072) end\n"
        "  if key=='content_type' then return 'application/x-plugin-test' end\n"
        "  if key=='headers' then collectgarbage(); return {} end\n"
        "  return nil\n"
        "end})\n"
        "local handle,err=plugin_http_request(options,function() end); assert(handle,err)\n";
    assert(luaL_dostring(L, lifetime_script) == LUA_OK);
    assert(plugin_async_http[0].active && plugin_async_http[1].active);
    for (int i = 0; i < 2; i++) {
        assert(pthread_join(plugin_async_http[i].thread, NULL) == 0);
        free(plugin_async_http[i].response_body);
        plugin_async_http[i].response_body = NULL;
        http_cancel_token_destroy(&plugin_async_http[i].cancel);
        plugin_async_http[i].active = false;
        luaL_unref(L, LUA_REGISTRYINDEX, plugin_async_http[i].callback_ref);
    }
    assert(atomic_load(&captured_count) == 2);
    bool saw_outer = false, saw_nested = false;
    for (int i = 0; i < 2; i++) {
        if (strcmp(captured[i].url, "http://example.invalid/plugin") == 0) {
            saw_outer = true;
            assert(strcmp(captured[i].content_type, "application/x-plugin-test") == 0);
            assert(captured[i].body_len == 131072 && captured[i].body_first == 'B');
        }
        if (strcmp(captured[i].url, "http://example.invalid/nested") == 0) saw_nested = true;
    }
    assert(saw_outer && saw_nested);

    tracked_allocations = 0;
    const char * invalid_options_script =
        "for i=1,24 do\n"
        "  local options=setmetatable({}, {__index=function(_,key)\n"
        "    if key=='url' then return 'http://example.invalid/' end\n"
        "    if key=='body' then return string.rep('X',1048576) end\n"
        "    if key=='content_type' then return {} end\n"
        "    return nil\n"
        "  end})\n"
        "  assert(not pcall(plugin_http_request,options,function() end)); collectgarbage()\n"
        "end\n";
    assert(luaL_dostring(L, invalid_options_script) == LUA_OK);
    assert(tracked_allocations == 0);
    lua_close(L);
    puts("PASS HTTP option lifetime and invalid-body allocation regressions");
    return 0;
}
