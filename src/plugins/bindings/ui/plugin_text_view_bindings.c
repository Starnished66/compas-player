#include "plugin_text_view_bindings.h"

#include "../../../ui/text/rendering/gui_text_view.h"
#include "../../lua/plugin_lua_runtime.h"

#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

/* One context per shown view. The view's on_close is delivered later (after
 * the back transition), so a closing view and a new one can briefly coexist.
 * L is the plugin's main state: a coroutine that opened the view may be
 * collected before the callbacks run. */
#define TEXT_VIEW_CTX_MAX 4
typedef struct {
    bool live;
    lua_State * L;
    int turn_ref;
    int close_ref;
    plugin_text_view_guarded_call_fn guarded_call;
    plugin_text_view_owner_is_aborted_fn owner_is_aborted;
} text_view_ctx_t;

static text_view_ctx_t text_view_ctxs[TEXT_VIEW_CTX_MAX];

/* A hard-aborted state is frozen: its registry is never touched again. */
static void text_view_ctx_release(text_view_ctx_t * ctx) {
    if (ctx->L && !ctx->owner_is_aborted(ctx->L)) {
        if (ctx->turn_ref != LUA_NOREF) luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->turn_ref);
        if (ctx->close_ref != LUA_NOREF) luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->close_ref);
    }
    ctx->live = false;
    ctx->L = NULL;
    ctx->turn_ref = LUA_NOREF;
    ctx->close_ref = LUA_NOREF;
    ctx->guarded_call = NULL;
    ctx->owner_is_aborted = NULL;
}

/* plugin_call()'s aborted-state return does not pop the function or its
 * arguments, and Lua pushes do not grow a frozen stack. Check before any
 * Lua API access. The slot stays live: the view and its queued close still
 * hold this pointer, and releasing it would let a new reader reuse the slot
 * before that close runs. */
static bool text_view_ctx_prepare(text_view_ctx_t * ctx) {
    if (!ctx || !ctx->live || !ctx->L) return false;
    return !ctx->owner_is_aborted(ctx->L);
}

static void text_view_turn_trampoline(int page, int pages, size_t byte_offset, void * user) {
    text_view_ctx_t * ctx = user;
    if (!text_view_ctx_prepare(ctx) || ctx->turn_ref == LUA_NOREF) return;

    lua_State * L = ctx->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->turn_ref);
    lua_pushinteger(L, page);
    lua_pushinteger(L, pages);
    lua_pushinteger(L, (lua_Integer) byte_offset);
    if (ctx->guarded_call(L, 3, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_text_view on_turn error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

/* Always installed for a view with a context, so the context is released
 * even when the plugin passed no on_close. */
static void text_view_close_trampoline(int page, size_t byte_offset, void * user) {
    text_view_ctx_t * ctx = user;
    if (!ctx || !ctx->live) return;

    /* Retire the slot after this delivery even when the plugin was aborted.
     * The callback is skipped; the slot is not reusable until then. */
    if (text_view_ctx_prepare(ctx) && ctx->close_ref != LUA_NOREF) {
        lua_State * L = ctx->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->close_ref);
        lua_pushinteger(L, page);
        lua_pushinteger(L, (lua_Integer) byte_offset);
        if (ctx->guarded_call(L, 2, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(L, -1);
            fprintf(stderr, "[plugins] show_text_view on_close error: %s\n", err ? err : "unknown error");
            lua_pop(L, 1);
        }
    }
    text_view_ctx_release(ctx);
}

typedef struct {
    bool has_turn;
    bool has_close;
    int turn_ref;
    int close_ref;
} text_view_refs_t;

/* Stack: lightuserdata, on_turn or nil, on_close or nil. Refs the functions
 * already validated above; a later __index on the options table is not read. */
static int text_view_store_refs(lua_State * L) {
    text_view_refs_t * refs = lua_touserdata(L, 1);
    if (refs->has_turn) {
        lua_pushvalue(L, 2);
        refs->turn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (refs->has_close) {
        lua_pushvalue(L, 3);
        refs->close_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

int plugin_text_view_bindings_show(lua_State * L,
                                   plugin_text_view_guarded_call_fn guarded_call,
                                   plugin_text_view_owner_is_aborted_fn owner_is_aborted) {
    const char * title = luaL_checkstring(L, 1);
    size_t text_len = 0;
    const char * text = luaL_checklstring(L, 2, &text_len);

    gui_text_view_opts opts;
    memset(&opts, 0, sizeof(opts));
    const char * image_paths[GUI_TEXT_VIEW_MAX_IMAGES];

    bool has_turn = false;
    bool has_close = false;
    int turn_abs = 0;
    int close_abs = 0;

    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        if (!lua_istable(L, 3)) {
            return luaL_error(L, "plugin.show_text_view: options must be a table");
        }

        lua_getfield(L, 3, "page");
        if (!lua_isnil(L, -1)) {
            if (lua_isnumber(L, -1)) {
                opts.has_page = true;
                opts.page = (int) lua_tointeger(L, -1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "offset");
        if (!lua_isnil(L, -1)) {
            if (lua_isnumber(L, -1)) {
                opts.has_offset = true;
                lua_Integer off = lua_tointeger(L, -1);
                opts.offset = (off < 0) ? 0 : (size_t) off;
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "font_px");
        lua_pop(L, 1);

        /* options.images: picture files for "<ESC>n<ESC>" markers (see
         * gui_text_view.h). The table stays on the stack until the view has
         * copied the paths, keeping these strings alive. */
        lua_getfield(L, 3, "images");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) return luaL_error(L, "plugin.show_text_view: images must be a list of paths");
            lua_Integer count = luaL_len(L, -1);
            if (count < 0 || count > GUI_TEXT_VIEW_MAX_IMAGES)
                return luaL_error(L, "plugin.show_text_view: at most %d images", GUI_TEXT_VIEW_MAX_IMAGES);
            for (lua_Integer i = 1; i <= count; i++) {
                lua_rawgeti(L, -1, i);
                size_t len = 0;
                const char * path = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
                if (!path || len == 0 || len >= 512 || strlen(path) != len)
                    return luaL_error(L, "plugin.show_text_view: images[%d] must be a path", (int) i);
                image_paths[i - 1] = path;
                lua_pop(L, 1);
            }
            opts.images = image_paths;
            opts.image_count = (int) count;
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, 3, "on_turn");
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) {
                return luaL_error(L, "plugin.show_text_view: on_turn must be a function");
            }
            has_turn = true;
            turn_abs = lua_gettop(L);
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, 3, "on_close");
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) {
                return luaL_error(L, "plugin.show_text_view: on_close must be a function");
            }
            has_close = true;
            close_abs = lua_gettop(L);
        } else {
            lua_pop(L, 1);
        }
    }

    if (gui_text_view_is_open()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "busy");
        return 2;
    }

    text_view_ctx_t * ctx = NULL;
    if (has_turn || has_close) {
        for (int i = 0; i < TEXT_VIEW_CTX_MAX; i++) {
            if (!text_view_ctxs[i].live) {
                ctx = &text_view_ctxs[i];
                break;
            }
        }
        if (!ctx) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "busy");
            return 2;
        }

        /* Slot stays free until both refs exist. A Lua error while storing
         * them must not consume a context. */
        if (!lua_checkstack(L, 4)) return luaL_error(L, "stack overflow");
        lua_State * main_L = plugin_lua_main_state(L);
        text_view_refs_t refs = { has_turn, has_close, LUA_NOREF, LUA_NOREF };
        lua_pushcfunction(L, text_view_store_refs);
        lua_pushlightuserdata(L, &refs);
        if (has_turn) lua_pushvalue(L, turn_abs);
        else lua_pushnil(L);
        if (has_close) lua_pushvalue(L, close_abs);
        else lua_pushnil(L);
        if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
            if (refs.turn_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, refs.turn_ref);
            if (refs.close_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, refs.close_ref);
            return lua_error(L);
        }
        ctx->L = main_L;
        ctx->guarded_call = guarded_call;
        ctx->owner_is_aborted = owner_is_aborted;
        ctx->turn_ref = refs.turn_ref;
        ctx->close_ref = refs.close_ref;
        ctx->live = true;
    }

    bool ok = gui_text_view_show(title, text, text_len, &opts,
                                 (ctx && has_turn) ? text_view_turn_trampoline : NULL,
                                 ctx ? text_view_close_trampoline : NULL, ctx);
    if (!ok) {
        if (ctx) text_view_ctx_release(ctx);
        lua_pushboolean(L, false);
        lua_pushstring(L, "unavailable");
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

void plugin_text_view_bindings_clear_owner(lua_State * main_owner, bool aborted) {
    for (int i = 0; i < TEXT_VIEW_CTX_MAX; i++) {
        text_view_ctx_t * ctx = &text_view_ctxs[i];
        if (!ctx->live || !ctx->L || ctx->L != main_owner) continue;
        if (!aborted) {
            if (ctx->turn_ref != LUA_NOREF) luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->turn_ref);
            if (ctx->close_ref != LUA_NOREF) luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->close_ref);
        }
        /* The GUI still owns user until its queued close trampoline runs. */
        ctx->L = NULL;
        ctx->turn_ref = LUA_NOREF;
        ctx->close_ref = LUA_NOREF;
    }
}

void plugin_text_view_bindings_clear_all(void) {
    for (int i = 0; i < TEXT_VIEW_CTX_MAX; i++) {
        if (text_view_ctxs[i].live) text_view_ctx_release(&text_view_ctxs[i]);
    }
}
