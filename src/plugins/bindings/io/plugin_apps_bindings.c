#include "plugin_apps_bindings.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../manager/plugin_manager.h"

#include "lauxlib.h"
#include "../../../core/device/app_registry.h"
#include "../../../ui/plugins/apps/gui_apps.h"

#include <stdlib.h>

int l_plugin_list_apps(lua_State * L) {
    int count = 0;
    app_manifest_t * apps = app_registry_scan(APP_ROOT, app_board_id(), &count);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, apps[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, apps[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, apps[i].version);
        lua_setfield(L, -2, "version");
        lua_createtable(L, apps[i].opens_count, 0);
        for (int j = 0; j < apps[i].opens_count; j++) {
            lua_pushstring(L, apps[i].opens[j]);
            lua_rawseti(L, -2, j + 1);
        }
        lua_setfield(L, -2, "opens");
        lua_rawseti(L, -2, i + 1);
    }
    free(apps);
    return 1;
}

int l_plugin_launch_app(lua_State * L) {
    const char * id = luaL_checkstring(L, 1);
    const char * path = luaL_optstring(L, 2, NULL);
    if (!plugin_manager_user_action_take(L))
        return plugin_lua_push_error(L, "plugin.launch_app only works once from a user action of the calling plugin");
    const char * reason;
    if (!gui_apps_request_launch(id, path, &reason)) return plugin_lua_push_error(L, reason);
    lua_pushboolean(L, 1);
    return 1;
}
