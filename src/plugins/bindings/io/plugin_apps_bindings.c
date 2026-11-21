#include "plugin_apps_bindings.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../manager/plugin_manager.h"

#include "lauxlib.h"
#include "../../../core/device/app_registry.h"
#include "../../../ui/plugins/apps/gui_apps.h"

#include <stdlib.h>

typedef struct {
    app_manifest_t * apps;
    int count;
} apps_ctx;

static void apps_cleanup(void * mem) { free(mem); }

static int apps_push_lua(lua_State * L) {
    apps_ctx * ctx = lua_touserdata(L, 1);
    app_manifest_t * apps = ctx->apps;
    int count = ctx->count;
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
    return 1;
}

int l_plugin_list_apps(lua_State * L) {
    apps_ctx ctx = { NULL, 0 };
    ctx.apps = app_registry_scan(APP_ROOT, app_board_id(), &ctx.count);
    return plugin_lua_push_then_cleanup(L, apps_push_lua, &ctx, 1, apps_cleanup, ctx.apps);
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
