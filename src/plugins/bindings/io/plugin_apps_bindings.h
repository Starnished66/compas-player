#ifndef PLUGIN_APPS_BINDINGS_H
#define PLUGIN_APPS_BINDINGS_H

#include "lua.h"

/* plugin.list_apps() -> array of { id, name, version, opens } for the
 * external apps installed on the SD card that run on this board
 * (capability apps.list). */
int l_plugin_list_apps(lua_State * L);

/* plugin.launch_app(id [, path]) -> true | nil, reason (capability
 * apps.launch). Allowed only from a user-action callback (a list row or menu
 * tap, a confirmation callback); path must be a file on the SD card whose
 * extension the app opens. The launch happens after the call returns, and
 * the player asks the user first when the app is not yet approved. */
int l_plugin_launch_app(lua_State * L);

#endif /* PLUGIN_APPS_BINDINGS_H */
