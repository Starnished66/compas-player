#ifndef PLUGIN_LUA_RUNTIME_H
#define PLUGIN_LUA_RUNTIME_H

#include "lua.h"

/* All helpers require a valid Lua state and must not run on a hard-aborted
 * state. The external-path result borrows the Lua string on the stack.
 * pcall_then_cleanup borrows ud/mem for its synchronous call, runs cleanup
 * exactly once (also after a failed stack check), then re-raises Lua errors. */
lua_State * plugin_lua_main_state(lua_State * L);
/* Call once on a freshly created plugin state, before any coroutine exists. */
void plugin_lua_register_main_state(lua_State * main);
int plugin_lua_push_error(lua_State * L, const char * message);
/* Reserved-tree check for native plugin.* entry points that take a
 * caller-controlled path and reach the filesystem outside the sandboxed
 * io/os wrappers. Raises a Lua error on a reserved path; returns the path. */
const char * plugin_lua_check_external_path(lua_State * L, int index, const char * api);
/* t[name] as an integer (luaL_optinteger: nil/absent gives def, a non-integer
 * raises a Lua error), leaving the stack unchanged. idx is evaluated before
 * the field is pushed, so a relative index names the table as at the call. */
lua_Integer plugin_lua_opt_integer_field(lua_State * L, int idx, const char * name, lua_Integer def);
int plugin_lua_pcall_then_cleanup(lua_State * L, void * ud, int nresults,
                                  void (*cleanup)(void *), void * mem);

#endif /* PLUGIN_LUA_RUNTIME_H */
