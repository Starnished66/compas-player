#include "plugin_equalizer_bindings.h"

#include "peq.h"

#include "lua.h"
#include "lauxlib.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

int l_plugin_eq_reset(lua_State * L) {
    (void) L;
    peq_reset_to_defaults();
    peq_save();
    return 0;
}

int l_plugin_eq_set_bypass(lua_State * L) {
    bool enabled = lua_toboolean(L, 1);
    peq_set_bypass(enabled);
    peq_save();
    return 0;
}

int l_plugin_eq_set_preamp(lua_State * L) {
    double db = luaL_checknumber(L, 1);
    peq_set_preamp_db(db);
    peq_save();
    return 0;
}

int l_plugin_eq_get_stereo_width(lua_State * L) {
    lua_pushnumber(L, peq_get_stereo_width());
    return 1;
}

int l_plugin_eq_set_stereo_width(lua_State * L) {
    double width = luaL_checknumber(L, 1);
    luaL_argcheck(L, isfinite(width) && width >= 0.0 && width <= 2.0, 1,
                  "stereo width must be finite and between 0 and 2");
    peq_set_stereo_width(width);
    peq_save();
    return 0;
}

int l_plugin_eq_get_crossfeed(lua_State * L) {
    lua_pushnumber(L, peq_get_crossfeed());
    return 1;
}

int l_plugin_eq_set_crossfeed(lua_State * L) {
    double strength = luaL_checknumber(L, 1);
    luaL_argcheck(L, isfinite(strength) && strength >= 0.0 && strength <= 1.0, 1,
                  "crossfeed must be finite and between 0 and 1");
    peq_set_crossfeed(strength);
    peq_save();
    return 0;
}

/* Converts a 1-based Lua band index to peq.c's 0-based one, luaL_error()ing
 * if out of range -- same "fail loudly at call time" convention
 * l_plugin_register_list_item() already uses for a bad list_id. */
static int check_eq_band_index(lua_State * L, int arg) {
    lua_Integer index = luaL_checkinteger(L, arg);
    if (index < 1 || index > PEQ_NUM_BANDS) {
        luaL_error(L, "band index %d out of range (expected 1..%d)", (int) index, PEQ_NUM_BANDS);
    }
    return (int) index - 1;
}

int l_plugin_eq_set_band(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    double freq_hz = luaL_checknumber(L, 2);
    double gain_db = luaL_checknumber(L, 3);
    double q = luaL_checknumber(L, 4);
    peq_set_band(index, freq_hz, gain_db, q);
    peq_save();
    return 0;
}

int l_plugin_eq_set_band_type(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    const char * type = luaL_checkstring(L, 2);

    peq_band_type_t t;
    if (strcmp(type, "peaking") == 0) t = PEQ_TYPE_PEAKING;
    else if (strcmp(type, "low_shelf") == 0) t = PEQ_TYPE_LOW_SHELF;
    else if (strcmp(type, "high_shelf") == 0) t = PEQ_TYPE_HIGH_SHELF;
    else return luaL_error(L, "plugin.eq_set_band_type: unknown type '%s' (expected \"peaking\", \"low_shelf\", or \"high_shelf\")", type);

    peq_set_band_type(index, t);
    peq_save();
    return 0;
}

int l_plugin_eq_set_band_enabled(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    bool enabled = lua_toboolean(L, 2);
    peq_set_band_enabled(index, enabled);
    peq_save();
    return 0;
}

/* Return an explicit, detached snapshot so plugins can keep or serialize it
 * without observing subsequent changes to peq.c's live band structs. */
int l_plugin_get_eq_state(lua_State * L) {
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, peq_get_bypass()); lua_setfield(L, -2, "bypass");
    lua_pushnumber(L, peq_get_preamp_db()); lua_setfield(L, -2, "preamp_db");
    lua_pushnumber(L, peq_get_stereo_width()); lua_setfield(L, -2, "stereo_width");
    lua_pushnumber(L, peq_get_crossfeed()); lua_setfield(L, -2, "crossfeed");
    lua_pushinteger(L, peq_get_crossfeed_angle()); lua_setfield(L, -2, "crossfeed_angle");
    lua_createtable(L, PEQ_NUM_BANDS, 0);
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        const peq_band_t * b = peq_get_band(i);
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, i + 1); lua_setfield(L, -2, "index");
        lua_pushnumber(L, b->freq_hz); lua_setfield(L, -2, "freq_hz");
        lua_pushnumber(L, b->gain_db); lua_setfield(L, -2, "gain_db");
        lua_pushnumber(L, b->q); lua_setfield(L, -2, "q");
        const char * type = b->type == PEQ_TYPE_LOW_SHELF ? "low_shelf" :
                            b->type == PEQ_TYPE_HIGH_SHELF ? "high_shelf" : "peaking";
        lua_pushstring(L, type); lua_setfield(L, -2, "type");
        lua_pushboolean(L, b->enabled); lua_setfield(L, -2, "enabled");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "bands");
    return 1;
}

/* Validate the complete table before touching live DSP state. Bounds follow
 * the PEQ's stable operating domain: positive audible frequencies, finite
 * gain/Q, and the stereo-width clamp implemented by peq.c. `crossfeed` is
 * optional so a snapshot captured before that field existed still applies:
 * a missing or nil field means off, and does not leave the previous strength
 * in place. A present field must be a finite number from 0 to 1. The
 * optional crossfeed_angle selects the delay; missing means legacy/no delay. */
int l_plugin_eq_apply_state(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    bool bypass, enabled[PEQ_NUM_BANDS];
    int crossfeed_angle = 0;
    double preamp, width, crossfeed, freq[PEQ_NUM_BANDS], gain[PEQ_NUM_BANDS], q[PEQ_NUM_BANDS];
    peq_band_type_t types[PEQ_NUM_BANDS];
    lua_getfield(L, 1, "bypass");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 1, "bypass must be boolean");
    bypass = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "preamp_db"); luaL_checktype(L, -1, LUA_TNUMBER); preamp = lua_tonumber(L, -1); lua_pop(L, 1);
    luaL_argcheck(L, isfinite(preamp) && preamp >= -300.0 && preamp <= 300.0, 1, "preamp_db must be finite and between -300 and 300");
    lua_getfield(L, 1, "stereo_width"); luaL_checktype(L, -1, LUA_TNUMBER); width = lua_tonumber(L, -1); lua_pop(L, 1);
    luaL_argcheck(L, isfinite(width) && width >= 0.0 && width <= 2.0, 1, "stereo_width must be finite and between 0 and 2");
    lua_getfield(L, 1, "crossfeed");
    if (lua_isnil(L, -1)) crossfeed = 0.0;
    else {
        luaL_argcheck(L, lua_type(L, -1) == LUA_TNUMBER, 1, "crossfeed must be a number when present");
        crossfeed = lua_tonumber(L, -1);
        luaL_argcheck(L, isfinite(crossfeed) && crossfeed >= 0.0 && crossfeed <= 1.0, 1, "crossfeed must be finite and between 0 and 1");
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "crossfeed_angle");
    if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_type(L, -1) == LUA_TNUMBER, 1, "crossfeed_angle must be a number when present");
        double angle = lua_tonumber(L, -1);
        luaL_argcheck(L, angle == 0.0 || angle == 30.0 || angle == 45.0 || angle == 60.0 || angle == 90.0,
                      1, "crossfeed_angle must be 0, 30, 45, 60, or 90");
        crossfeed_angle = (int) angle;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "bands");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TTABLE && lua_rawlen(L, -1) == PEQ_NUM_BANDS, 1, "bands must contain exactly 10 entries");
    int bands_table = lua_gettop(L);
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        lua_rawgeti(L, bands_table, i + 1);
        luaL_argcheck(L, lua_type(L, -1) == LUA_TTABLE, 1, "each band must be a table");
        int band_table = lua_gettop(L);
        lua_getfield(L, band_table, "index");
        luaL_argcheck(L, lua_isinteger(L, -1) && lua_tointeger(L, -1) == i + 1, 1, "band indices must be ordered 1..10"); lua_pop(L, 1);
        lua_getfield(L, band_table, "freq_hz"); luaL_checktype(L, -1, LUA_TNUMBER); freq[i] = lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, band_table, "gain_db"); luaL_checktype(L, -1, LUA_TNUMBER); gain[i] = lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, band_table, "q"); luaL_checktype(L, -1, LUA_TNUMBER); q[i] = lua_tonumber(L, -1); lua_pop(L, 1);
        luaL_argcheck(L, isfinite(freq[i]) && freq[i] >= 1.0 && freq[i] <= 24000.0, 1, "freq_hz must be finite and between 1 and 24000");
        luaL_argcheck(L, isfinite(gain[i]) && gain[i] >= -300.0 && gain[i] <= 300.0, 1, "gain_db must be finite and between -300 and 300");
        luaL_argcheck(L, isfinite(q[i]) && q[i] >= 0.01 && q[i] <= 100.0, 1, "q must be finite and between 0.01 and 100");
        lua_getfield(L, band_table, "type");
        luaL_checktype(L, -1, LUA_TSTRING);
        const char * type = lua_tostring(L, -1);
        if (strcmp(type, "peaking") == 0) types[i] = PEQ_TYPE_PEAKING;
        else if (strcmp(type, "low_shelf") == 0) types[i] = PEQ_TYPE_LOW_SHELF;
        else if (strcmp(type, "high_shelf") == 0) types[i] = PEQ_TYPE_HIGH_SHELF;
        else return luaL_error(L, "band type must be peaking, low_shelf, or high_shelf");
        lua_pop(L, 1);
        lua_getfield(L, band_table, "enabled");
        luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 1, "enabled must be boolean");
        enabled[i] = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    bool persist = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE); lua_getfield(L, 2, "persist");
        if (!lua_isnil(L, -1)) { luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 2, "persist must be boolean"); persist = lua_toboolean(L, -1); }
        lua_pop(L, 1);
    }
    peq_set_bypass(bypass); peq_set_preamp_db(preamp); peq_set_stereo_width(width); peq_set_crossfeed(crossfeed); peq_set_crossfeed_angle(crossfeed_angle);
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        peq_set_band(i, freq[i], gain[i], q[i]); peq_set_band_type(i, types[i]); peq_set_band_enabled(i, enabled[i]);
    }
    if (persist) peq_save();
    lua_pushboolean(L, true);
    return 1;
}
