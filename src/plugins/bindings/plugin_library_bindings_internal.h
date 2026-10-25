#ifndef PLUGIN_LIBRARY_BINDINGS_INTERNAL_H
#define PLUGIN_LIBRARY_BINDINGS_INTERNAL_H

#include "lua.h"

int plugin_library_push_string_array_result(lua_State * L, char ** items, int count);

#endif /* PLUGIN_LIBRARY_BINDINGS_INTERNAL_H */
