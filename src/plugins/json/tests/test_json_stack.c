#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "../plugin_json.h"

#include <assert.h>
#include <stdio.h>

static void register_fn(lua_State * L, const char * name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setglobal(L, name);
}

int main(void) {
    lua_State * L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    register_fn(L, "json_encode", l_plugin_json_encode);
    register_fn(L, "json_decode", l_plugin_json_decode);
    const char * script =
        "local opts={max_nesting=64}\n"
        "local t=0; for i=1,60 do t={child=t} end\n"
        "local encoded,err=json_encode(t,opts); assert(encoded,err)\n"
        "local decoded,derr=json_decode(encoded,opts); assert(decoded,derr)\n"
        "for i=1,60 do assert(type(decoded)=='table'); decoded=decoded.child end\n"
        "assert(decoded==0)\n";
    int rc = luaL_dostring(L, script);
    if (rc != LUA_OK) fprintf(stderr, "JSON stack regression: %s\n", lua_tostring(L, -1));
    assert(rc == LUA_OK);
    lua_close(L);
    puts("PASS nested JSON encode/decode (60 levels)");
    return 0;
}
