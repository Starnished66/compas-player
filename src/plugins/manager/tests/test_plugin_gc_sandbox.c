#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "../plugin_call.c"

#include <assert.h>
#include <stdio.h>

bool plugin_storage_path_is_reserved(const char * path) { (void) path; return false; }
bool plugin_path_in_program_dirs(const char * path) { (void) path; return false; }
int plugin_lua_push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

int main(void) {
    lua_State * L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    sandbox_plugin_lua_state(L);
    const char * script =
        "local ok,err=pcall(setmetatable, {}, {__gc=function() while true do end end})\n"
        "assert(not ok and string.find(err, '__gc'))\n"
        "local f=io.tmpfile(); assert(f); assert(getmetatable(f)==nil); f:close()\n";
    int rc = luaL_dostring(L, script);
    if (rc != LUA_OK) fprintf(stderr, "GC sandbox regression: %s\n", lua_tostring(L, -1));
    assert(rc == LUA_OK);
    lua_close(L);
    puts("PASS plugin finalizer sandbox guards");
    return 0;
}
