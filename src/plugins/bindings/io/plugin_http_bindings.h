#ifndef PLUGIN_HTTP_BINDINGS_H
#define PLUGIN_HTTP_BINDINGS_H

#include <stdbool.h>
#include "lua.h"

typedef int (*plugin_http_guarded_call_fn)(lua_State * L, int nargs, int nresults, int errfunc);
/* True for a hard-aborted owner: its state is frozen and never touched again. */
typedef bool (*plugin_http_owner_aborted_fn)(lua_State * L);

/* Lua bindings and lifecycle services are called on the manager's UI/Lua
 * thread. Worker threads must not access Lua. Poll must receive the manager's
 * guarded callback so timeout and aborted-plugin policy remains centralized.
 * clear_owner takes a normalized main state; when aborted is true it skips
 * Lua registry access. cancel_all joins workers and unreferences callbacks (not of aborted owners),
 * so it must run before any owned Lua state is closed. */
int l_plugin_http_request(lua_State * L);
int l_plugin_download_file_async(lua_State * L);
int l_plugin_get_download_progress(lua_State * L);
int l_plugin_cancel(lua_State * L);

void plugin_http_bindings_poll(plugin_http_guarded_call_fn guarded_call,
                               plugin_http_owner_aborted_fn owner_is_aborted);
void plugin_http_bindings_clear_owner(lua_State * owner, bool aborted);
void plugin_http_bindings_cancel_all(plugin_http_owner_aborted_fn owner_is_aborted);
bool plugin_http_bindings_has_work(void);

#endif /* PLUGIN_HTTP_BINDINGS_H */
