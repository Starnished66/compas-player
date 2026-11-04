/* plugin.soundstage_apply_state() / plugin.get_soundstage_state(): the
 * transient native Soundstage DSP stage, owned by the plugin that enabled it. */
#include "../../../audio/effects/soundstage.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../manager/plugin_manager_internal.h"

#include <math.h>
#include <stddef.h>

lua_State * soundstage_owner;

static double * field_ptr(soundstage_config_t * config, size_t i) {
    return (double *)((char *)config + soundstage_fields[i].offset);
}

/* Every field is required; the first missing or bad one is an argument error
 * and nothing has been published by then. */
static int l_plugin_soundstage_apply_state(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    soundstage_config_t config;
    soundstage_defaults(&config);
    lua_getfield(L, 1, "enabled");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 1, "enabled must be boolean");
    config.enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);
    for (size_t i = 0; i < soundstage_field_count; i++) {
        lua_getfield(L, 1, soundstage_fields[i].name);
        int is_number = lua_type(L, -1) == LUA_TNUMBER;
        double value = is_number ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
        if (!is_number || !isfinite(value) || value < soundstage_fields[i].min || value > soundstage_fields[i].max)
            return luaL_argerror(L, 1, lua_pushfstring(L, "%s must be a number between %f and %f",
                                                       soundstage_fields[i].name, soundstage_fields[i].min, soundstage_fields[i].max));
        *field_ptr(&config, i) = value;
    }
    if (!soundstage_set_config(&config)) { lua_pushboolean(L, false); return 1; }
    soundstage_owner = config.enabled ? plugin_lua_main_state(L) : NULL;
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_get_soundstage_state(lua_State * L) {
    soundstage_config_t config;
    soundstage_get_config(&config);
    lua_createtable(L, 0, (int)soundstage_field_count + 1);
    lua_pushboolean(L, config.enabled);
    lua_setfield(L, -2, "enabled");
    for (size_t i = 0; i < soundstage_field_count; i++) {
        lua_pushnumber(L, *field_ptr(&config, i));
        lua_setfield(L, -2, soundstage_fields[i].name);
    }
    return 1;
}

const luaL_Reg plugin_soundstage_funcs[] = {
    { "soundstage_apply_state", l_plugin_soundstage_apply_state },
    { "get_soundstage_state",   l_plugin_get_soundstage_state },
    { NULL, NULL }
};
