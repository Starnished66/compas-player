#ifndef PLUGIN_ARCHIVE_BINDINGS_H
#define PLUGIN_ARCHIVE_BINDINGS_H

#include "lua.h"
#include <stdbool.h>

typedef int (*plugin_archive_guarded_call_fn)(lua_State * L, int nargs, int nresults, int errfunc);
typedef bool (*plugin_archive_owner_aborted_fn)(lua_State * L);

/* Lua/UI-thread API only. Worker threads never touch Lua. The poll callback
 * must be the manager's guarded plugin_call; the abort predicate must use
 * normalized main states and report false when the instance is missing.
 * clear_owner expects the normalized owner state and skips registry access
 * when aborted. cancel_all joins before Lua close and checks abort afterward. */
int l_plugin_zip_read(lua_State * L);
int l_plugin_zip_list(lua_State * L);
int l_plugin_set_startup_image_async(lua_State * L);
int l_plugin_has_startup_image(lua_State * L);
int l_plugin_zip_image_async(lua_State * L);
int l_plugin_image_thumbnail_async(lua_State * L);
int l_plugin_image_wallpaper_async(lua_State * L);
int l_plugin_image_startup_async(lua_State * L);
void plugin_archive_bindings_poll(plugin_archive_guarded_call_fn guarded_call,
                                  plugin_archive_owner_aborted_fn owner_is_aborted);
void plugin_archive_bindings_clear_owner(lua_State * normalized_owner, bool aborted);
void plugin_archive_bindings_cancel_all(plugin_archive_owner_aborted_fn owner_is_aborted);
bool plugin_archive_bindings_has_work(void);

#endif /* PLUGIN_ARCHIVE_BINDINGS_H */
