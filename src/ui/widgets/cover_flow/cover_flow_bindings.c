/* plugin.show_cover_flow{}: the Lua side of the cover flow screen. Callbacks are
 * delivered the way plugin.show_list() delivers on_select and on_hold. */
#include "cover_flow.h"
#include "../../../plugins/manager/plugin_manager_internal.h"
#include "../../../plugins/lua/plugin_lua_runtime.h"
#include <string.h>

static lua_State * owner;
static int select_ref = LUA_NOREF;
static int hold_ref = LUA_NOREF;

static void dispatch(int ref, int index, const char * what) {
    if (!owner || ref == LUA_NOREF || !plugin_push_callback(owner, ref)) return;
    lua_pushinteger(owner, index + 1);
    plugin_call_report(owner, 1, true, "show_cover_flow %s error", what);
}

static void on_select(int index) { dispatch(select_ref, index, "on_select"); }
static void on_hold(int index) { dispatch(hold_ref, index, "on_hold"); }

static void drop_refs(bool aborted) {
    if (owner && !aborted) {
        plugin_unref(owner, select_ref);
        plugin_unref(owner, hold_ref);
    }
    owner = NULL;
    select_ref = hold_ref = LUA_NOREF;
}

static void on_closed(void) { drop_refs(false); }

static int fail(lua_State * L, const char * message) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, message);
    return 2;
}

/* The strings stay owned by the items table, which the caller keeps alive; the
 * screen copies them. */
static bool read_items(lua_State * L, int index, cover_flow_item_t * out, int n) {
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, index, i + 1);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        lua_pushstring(L, "title");
        lua_rawget(L, -2);
        out[i].title = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pushstring(L, "subtitle");
        lua_rawget(L, -3);
        out[i].subtitle = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_getfield(L, -3, "song_id");
        out[i].song_id = lua_isnumber(L, -1) ? (int64_t) lua_tointeger(L, -1) : 0;
        lua_pop(L, 4); /* the strings live on in the item table */
    }
    return true;
}

/* Optional function field; false if present with another type. */
static bool function_field(lua_State * L, const char * name, bool * present) {
    lua_getfield(L, 1, name);
    *present = !lua_isnil(L, -1);
    bool ok = !*present || lua_isfunction(L, -1);
    if (!*present) lua_pop(L, 1);
    return ok;
}

/* What the options table asked for. The stack holds the values `*_index` point at. */
typedef struct {
    int items_index, select_index, hold_index, n, selected;
    bool has_hold, az_index;
    const char * title;
} show_opts_t;

/* Reads the options table at 1 onto the stack; the error message, or NULL when all is well. */
static const char * read_options(lua_State * L, show_opts_t * o) {
    if (!lua_istable(L, 1)) return "options table expected";
    lua_getfield(L, 1, "items");
    if (!lua_istable(L, -1)) return "items must be a table";
    o->items_index = lua_gettop(L);
    lua_Unsigned raw = lua_rawlen(L, o->items_index);
    o->n = raw > COVER_FLOW_MAX_ITEMS ? COVER_FLOW_MAX_ITEMS : (int) raw;
    if (o->n == 0) return "items is empty";

    bool has_select;
    if (!function_field(L, "on_select", &has_select) || !has_select) return "on_select must be a function";
    o->select_index = lua_gettop(L);
    if (!function_field(L, "on_hold", &o->has_hold)) return "on_hold must be a function";
    o->hold_index = lua_gettop(L);

    lua_getfield(L, 1, "selected");
    int is_int = 1;
    lua_Integer selected = lua_isnil(L, -1) ? 1 : lua_tointegerx(L, -1, &is_int);
    if (!is_int) return "selected must be an integer";
    if (selected < 1 || selected > o->n) return "selected is outside the items";
    o->selected = (int) selected - 1;
    lua_getfield(L, 1, "title");
    o->title = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_getfield(L, 1, "az_index");
    o->az_index = lua_toboolean(L, -1);
    return NULL;
}

static int l_plugin_show_cover_flow(lua_State * L) {
    show_opts_t o = { 0 };
    const char * error = read_options(L, &o);
    if (error) return fail(L, error);

    cover_flow_item_t * items = lua_newuserdata(L, (size_t) o.n * sizeof(*items));
    if (!read_items(L, o.items_index, items, o.n)) return fail(L, "every item must be a table");
    cover_flow_params_t params = { o.title, items, o.n, o.selected,
                                 on_select, o.has_hold ? on_hold : NULL, on_closed, o.az_index };
    if (!cover_flow_show(&params)) return fail(L, "unavailable");

    /* Shown: keep the new callbacks, release the old ones. */
    lua_pushvalue(L, o.select_index);
    int new_select_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int new_hold_ref = LUA_NOREF;
    if (o.has_hold) {
        lua_pushvalue(L, o.hold_index);
        new_hold_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    drop_refs(false);
    owner = plugin_lua_main_state(L);
    select_ref = new_select_ref;
    hold_ref = new_hold_ref;
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg plugin_cover_flow_funcs[] = {
    { "show_cover_flow", l_plugin_show_cover_flow },
    { NULL, NULL }
};

/* A plugin unloads: its callbacks go; the screen stays until Back. */
void plugin_cover_flow_clear_owner(lua_State * main_owner, bool aborted) {
    if (owner && owner == main_owner) drop_refs(aborted);
}

/* Deinit drops refs without calling Lua; the screen goes too. */
void plugin_cover_flow_clear_all(void) {
    drop_refs(true);
    cover_flow_shutdown();
}
