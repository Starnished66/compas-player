#ifndef PLUGIN_INFO_BINDINGS_H
#define PLUGIN_INFO_BINDINGS_H

#include "lua.h"

int l_plugin_api_version(lua_State * L);
int l_plugin_get_app_info(lua_State * L);
int l_plugin_has_capability(lua_State * L);
int l_plugin_media_capabilities(lua_State * L);

#endif /* PLUGIN_INFO_BINDINGS_H */
