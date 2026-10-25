#ifndef PLUGIN_TEXT_VIEW_BINDINGS_H
#define PLUGIN_TEXT_VIEW_BINDINGS_H

#include <stdbool.h>
#include "lua.h"

typedef int (*plugin_text_view_guarded_call_fn)(lua_State * L, int nargs, int nresults, int errfunc);
typedef bool (*plugin_text_view_owner_is_aborted_fn)(lua_State * L);

/* All functions run on the manager's UI/Lua thread. The callbacks supplied
 * to show must remain valid for the view lifetime. owner_is_aborted receives
 * the normalized main state and must not touch Lua when that state is frozen. */
int plugin_text_view_bindings_show(lua_State * L,
                                   plugin_text_view_guarded_call_fn guarded_call,
                                   plugin_text_view_owner_is_aborted_fn owner_is_aborted);

/* main_owner must be normalized. Failed-load cleanup leaves a tombstone until
 * the GUI's deferred close callback releases its user pointer. clear_all is
 * for deinit only, after gui_text_view_teardown() and before lua_close(). */
void plugin_text_view_bindings_clear_owner(lua_State * main_owner, bool aborted);
void plugin_text_view_bindings_clear_all(void);

#endif /* PLUGIN_TEXT_VIEW_BINDINGS_H */
