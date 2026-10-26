#ifndef PLUGIN_LUA_RUNTIME_H
#define PLUGIN_LUA_RUNTIME_H

#include "lua.h"

/* All helpers require a valid Lua state and must not run on a hard-aborted
 * state. The external-path result borrows the Lua string on the stack.
 * pcall_then_cleanup borrows ud/mem for its synchronous call, runs cleanup
 * exactly once (also after a failed stack check), then re-raises Lua errors. */
lua_State * plugin_lua_main_state(lua_State * L);
int plugin_lua_push_error(lua_State * L, const char * message);
const char * plugin_lua_check_external_path(lua_State * L, int index, const char * api);
int plugin_lua_pcall_then_cleanup(lua_State * L, void * ud, int nresults,
                                  void (*cleanup)(void *), void * mem);

#endif /* PLUGIN_LUA_RUNTIME_H */
