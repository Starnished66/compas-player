#ifndef PLUGIN_EQUALIZER_BINDINGS_H
#define PLUGIN_EQUALIZER_BINDINGS_H

#include "lua.h"

int l_plugin_eq_reset(lua_State * L);
int l_plugin_eq_set_bypass(lua_State * L);
int l_plugin_eq_set_preamp(lua_State * L);
int l_plugin_eq_get_stereo_width(lua_State * L);
int l_plugin_eq_set_stereo_width(lua_State * L);
int l_plugin_eq_get_crossfeed(lua_State * L);
int l_plugin_eq_set_crossfeed(lua_State * L);
int l_plugin_eq_set_band(lua_State * L);
int l_plugin_eq_set_band_type(lua_State * L);
int l_plugin_eq_set_band_enabled(lua_State * L);
int l_plugin_get_eq_state(lua_State * L);
int l_plugin_eq_apply_state(lua_State * L);

#endif /* PLUGIN_EQUALIZER_BINDINGS_H */
