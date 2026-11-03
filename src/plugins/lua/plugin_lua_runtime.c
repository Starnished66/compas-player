#include "plugin_lua_runtime.h"
#include "lauxlib.h"
#include "../storage/plugin_storage.h"

/* A new thread copies the main thread's extra space (lua_newthread), so the
 * main state recorded there is readable from any coroutine without touching
 * the registry of a possibly frozen state. */
void plugin_lua_register_main_state(lua_State * main) {
    *(lua_State **) lua_getextraspace(main) = main;
}

lua_State * plugin_lua_main_state(lua_State * L) {
    return *(lua_State **) lua_getextraspace(L);
}

int plugin_lua_push_error(lua_State * L, const char * message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

/* `fn` is already on the stack. Lua allocation inside the protected call can
 * raise; `cleanup` still runs, then the error is re-raised. A failed stack
 * check takes the same path so the native buffer is not kept. */
lua_Integer plugin_lua_opt_integer_field(lua_State * L, int idx, const char * name, lua_Integer def) {
    lua_getfield(L, idx, name);
    lua_Integer v = luaL_optinteger(L, -1, def);
    lua_pop(L, 1);
    return v;
}

int plugin_lua_pcall_then_cleanup(lua_State * L, void * ud, int nresults,
                              void (*cleanup)(void *), void * mem) {
    if (!lua_checkstack(L, 1)) {
        if (cleanup) cleanup(mem);
        return luaL_error(L, "stack overflow");
    }
    lua_pushlightuserdata(L, ud);
    int rc = lua_pcall(L, 1, nresults, 0);
    if (cleanup) cleanup(mem);
    if (rc != LUA_OK) return lua_error(L);
    return nresults;
}


const char * plugin_lua_check_external_path(lua_State * L, int index, const char * api) {
    const char * path = luaL_checkstring(L, index);
    if (plugin_storage_path_is_reserved(path)) {
        luaL_error(L, "%s: path is reserved for plugin.storage/plugin.secrets", api);
        return NULL; /* unreachable -- luaL_error() longjmps */
    }
    return path;
}
