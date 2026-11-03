/* plugin.show_list(), settings lists, order editor and lock screen, with their UI callbacks. */
#include "../../ui/application/gui.h"
#include "../../ui/plugins/plugin_control_dashboard.h"
#include "../lua/plugin_lua_runtime.h"
#include <string.h>
#include <unistd.h>
#include "plugin_manager_internal.h"

plugin_list_callback_t plugin_list_callbacks[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lua_Integer plugin_list_next_handle = 1;

plugin_settings_list_row_ref_t
    plugin_settings_list_rows[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_ROWS];
int plugin_settings_list_row_counts[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];
plugin_order_editor_callback_t plugin_order_editor_callback = { NULL, LUA_NOREF };

/* Each items[] entry is either a plain string (today's original behavior,
 * unchanged) or a table { label = "...", icon = "...", text_size = "...",
 * wrap = true } for a row that wants its own icon/text size or wrapped label
 * -- see PLUGINS.md. An optional trailing `options` table (4th arg) carries
 * `height` and `width`, applying to every row in this call (call-level,
 * setting -- a plain browsing list mixing wildly different row heights
 * would look broken in a way an occasional taller settings-submenu row
 * doesn't, see PLUGINS.md). */
static int l_plugin_show_list(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* A replacement is scoped to this Lua state and an exact live handle.
     * Closed, covered, foreign, or superseded lists cannot be reopened by a
     * late asynchronous callback. Each render gets a new handle below. */
    int replace_slot = -1;
    lua_State * owner = plugin_lua_main_state(L);
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "replace");
        if (!lua_isnil(L, -1)) {
            lua_Integer handle = luaL_checkinteger(L, -1);
            for (int i = 0; i < PLUGIN_LIST_SCREEN_POOL_SIZE; i++) {
                if (handle > 0 && plugin_list_callbacks[i].L == owner &&
                    plugin_list_callbacks[i].handle == handle && gui_plugin_list_is_top(i)) {
                    replace_slot = i;
                    break;
                }
            }
            if (replace_slot < 0) { lua_pushnil(L); return 1; }
        }
        lua_pop(L, 1);
    }

    lua_Unsigned raw_n = lua_rawlen(L, 2);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;

    size_t label_capacity = 160;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "wrap");
            if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) label_capacity = 512;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (label_capacity == 512) break;
    }
    size_t alloc_n = (n > 0) ? (size_t) n : 1;
    char * label_bufs = lua_newuserdata(L, alloc_n * label_capacity);
    bool * wrap_labels = lua_newuserdata(L, (size_t) alloc_n * sizeof(*wrap_labels));
    const char ** labels = lua_newuserdata(L, (size_t) alloc_n * sizeof(*labels));
    char (*icon_bufs)[256] = lua_newuserdata(L, (size_t) alloc_n * sizeof(*icon_bufs));
    const char ** icon_paths = lua_newuserdata(L, (size_t) alloc_n * sizeof(*icon_paths));
    char (*text_size_bufs)[8] = lua_newuserdata(L, (size_t) alloc_n * sizeof(*text_size_bufs));
    const char ** text_sizes = lua_newuserdata(L, (size_t) alloc_n * sizeof(*text_sizes));

    for (int i = 0; i < n; i++) {
        char * label_buf = label_bufs + (size_t)i * label_capacity;
        lua_rawgeti(L, 2, i + 1);
        icon_bufs[i][0] = '\0';
        icon_paths[i] = NULL;
        text_size_bufs[i][0] = '\0';
        text_sizes[i] = NULL;
        wrap_labels[i] = false;

        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "wrap");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TBOOLEAN);
                wrap_labels[i] = lua_toboolean(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "label");
            const char * s = lua_tostring(L, -1);
            snprintf(label_buf, wrap_labels[i] ? label_capacity : 160, "%s", s ? s : "");
            lua_pop(L, 1);

            lua_getfield(L, -1, "icon");
            const char * icon = lua_tostring(L, -1);
            if (icon) {
                snprintf(icon_bufs[i], sizeof(icon_bufs[i]), "%s", icon);
                icon_paths[i] = icon_bufs[i];
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "text_size");
            const char * text_size = lua_tostring(L, -1);
            if (text_size) {
                if (!is_valid_text_size(text_size)) {
                    return luaL_error(L, "plugin.show_list: row %d has an unknown text_size '%s' (expected \"small\", \"medium\", \"large\", or \"mono\")",
                                       i + 1, text_size);
                }
                snprintf(text_size_bufs[i], sizeof(text_size_bufs[i]), "%s", text_size);
                text_sizes[i] = text_size_bufs[i];
            }
            lua_pop(L, 1);
        } else {
            const char * s = lua_tostring(L, -1);
            snprintf(label_buf, 160, "%s", s ? s : "");
        }
        labels[i] = label_buf;
        lua_pop(L, 1);
    }

    int32_t height = 0;
    int32_t width = 0;
    int selected_index = -1;
    int columns = 0;
    bool reset_scroll = false;
    bool has_hold = false;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "on_hold");
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1)) return luaL_error(L, "plugin.show_list: on_hold must be a function");
            has_hold = true;
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "layout");
        if (!lua_isnil(L, -1)) {
            const char * layout = luaL_checkstring(L, -1);
            if (strcmp(layout, "grid") == 0) columns = 3;
            else if (strcmp(layout, "list") != 0)
                return luaL_error(L, "plugin.show_list: layout must be \"list\" or \"grid\"");
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "columns");
        if (!lua_isnil(L, -1)) {
            lua_Integer c = luaL_checkinteger(L, -1);
            if (c < 2 || c > 4) return luaL_error(L, "plugin.show_list: columns must be 2..4");
            if (columns > 0) columns = (int) c;
        }
        lua_pop(L, 1);
        height = (int32_t) plugin_lua_opt_integer_field(L, 4, "height", 0);
        width = (int32_t) plugin_lua_opt_integer_field(L, 4, "width", 0);
        lua_getfield(L, 4, "selected");
        if (!lua_isnil(L, -1)) {
            lua_Integer selected = luaL_checkinteger(L, -1);
            if (selected < 1 || selected > n)
                return luaL_error(L, "plugin.show_list: selected index %lld is outside 1..%d",
                                  (long long) selected, n);
            selected_index = (int) selected - 1;
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "reset_scroll");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            reset_scroll = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int new_hold_ref = LUA_NOREF;
    if (has_hold) {
        lua_getfield(L, 4, "on_hold");
        new_hold_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int slot = gui_plugin_show_list(title, labels, icon_paths, text_sizes, wrap_labels, height, width, selected_index, n, columns, replace_slot);
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) {
        luaL_unref(L, LUA_REGISTRYINDEX, new_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, new_hold_ref);
        lua_pushnil(L);
        return 1;
    }
    if (replace_slot >= 0 && reset_scroll) gui_plugin_list_reset_scroll(slot);
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    plugin_unref(cb->L, cb->select_ref);
    plugin_unref(cb->L, cb->hold_ref);
    cb->L = plugin_lua_main_state(L);
    cb->select_ref = new_ref;
    cb->hold_ref = new_hold_ref;
    cb->handle = plugin_list_next_handle++;
    lua_pushinteger(L, cb->handle);
    return 1;
}

/* plugin.is_list_showing(handle): true while the list show_list() returned
 * handle for is the screen in front (not closed, covered, or replaced). */
static int l_plugin_is_list_showing(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    bool showing = false;
    for (int i = 0; i < PLUGIN_LIST_SCREEN_POOL_SIZE; i++) {
        if (plugin_list_callbacks[i].handle == handle) {
            showing = handle > 0 && gui_plugin_list_is_top(i);
            break;
        }
    }
    lua_pushboolean(L, showing);
    return 1;
}

/* Parse the shared shape used by plugin.show_lock_screen() and the optional
 * inline settings preview. Keep validation and legacy defaults identical. */
static bool plugin_parse_lock_screen_options(lua_State * L, int options_index,
                                            gui_lock_screen_options_t * opts,
                                            const char ** error_out) {
    options_index = lua_absindex(L, options_index);
    memset(opts, 0, sizeof(*opts));
    if (!lua_istable(L, options_index)) {
        *error_out = "options must be a table";
        return false;
    }

    lua_getfield(L, options_index, "mode");
    const char * mode_str = lua_tostring(L, -1);
    if (!mode_str) {
        lua_pop(L, 1);
        *error_out = "missing mode";
        return false;
    }
    if (strcmp(mode_str, "album_art") == 0) {
        opts->mode = LOCK_SCREEN_MODE_ALBUM_ART;
    } else if (strcmp(mode_str, "image") == 0) {
        opts->mode = LOCK_SCREEN_MODE_IMAGE;
    } else if (strcmp(mode_str, "clock") == 0) {
        opts->mode = LOCK_SCREEN_MODE_CLOCK;
    } else {
        lua_pop(L, 1);
        *error_out = "invalid mode (expected 'album_art', 'image', or 'clock')";
        return false;
    }
    lua_pop(L, 1);

    if (opts->mode == LOCK_SCREEN_MODE_IMAGE) {
        lua_getfield(L, options_index, "image_path");
        const char * path = lua_tostring(L, -1);
        /* Validate before access(), then copy to the bounded core buffer. */
        if (!path || strlen(path) >= sizeof(opts->image_path)) {
            lua_pop(L, 1);
            *error_out = "image_path missing or too long";
            return false;
        }
        if (access(path, R_OK) != 0) {
            lua_pop(L, 1);
            *error_out = "image_path inaccessible or not readable";
            return false;
        }
        snprintf(opts->image_path, sizeof(opts->image_path), "%s", path);
        lua_pop(L, 1);
    }

    if (opts->mode == LOCK_SCREEN_MODE_IMAGE || opts->mode == LOCK_SCREEN_MODE_ALBUM_ART) {
        lua_getfield(L, options_index, "image_fit");
        if (lua_isnil(L, -1)) {
            /* Older plugins that omit this field retain historical behavior. */
            opts->image_fit = LOCK_SCREEN_IMAGE_FIT_NATURAL;
        } else {
            const char * fit = lua_tostring(L, -1);
            if (fit && strcmp(fit, "contain") == 0) {
                opts->image_fit = LOCK_SCREEN_IMAGE_FIT_CONTAIN;
            } else if (fit && strcmp(fit, "cover") == 0) {
                opts->image_fit = LOCK_SCREEN_IMAGE_FIT_COVER;
            } else {
                lua_pop(L, 1);
                *error_out = "image_fit must be 'contain' or 'cover'";
                return false;
            }
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, options_index, "clock_24h");
    opts->clock_24h = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : current_settings.clock_24h;
    lua_pop(L, 1);
    return true;
}

/* plugin.show_settings_list(title, items) -- a nested settings submenu with
 * real toggle/slider rows, see compas-plugins/PLUGINS.md.
 * Each Lua item is a table: { type = "row"|"toggle"|"slider", label = ...,
 * on_select = fn }  (type == "row") or  { ..., value = bool, on_change = fn }
 * (type == "toggle")  or  { ..., min = n, max = n, value = n, on_change = fn }
 * (type == "slider"). A "row" whose on_select calls show_settings_list()
 * again is how a plugin nests a submenu inside a submenu -- same chaining
 * convention plugin.show_list() already supports.
 *
 * Rows past PLUGIN_SETTINGS_LIST_MAX_ROWS, and "slider" rows past
 * PLUGIN_SETTINGS_LIST_MAX_SLIDERS within one call, are silently dropped --
 * same truncate-don't-error convention as l_plugin_show_list()'s own
 * PLUGIN_MAX_LIST_ITEMS cap. An item with an unknown/missing `type`, or
 * missing its required callback, raises a Lua error (fail loudly at call
 * time, same convention l_plugin_register_list_item() uses for a bad
 * list_id). */
static int l_plugin_show_settings_list(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    bool update_existing = false;
    bool has_preview = false;
    bool has_dashboard = false;
    plugin_control_dashboard_options_t dashboard = {0};
    dashboard.accent_first = true;
    gui_lock_screen_options_t preview_options;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "update");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            update_existing = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "layout");
        if (!lua_isnil(L, -1)) {
            const char * layout = luaL_checkstring(L, -1);
            if (strcmp(layout, "dashboard") == 0) has_dashboard = true;
            else if (strcmp(layout, "list") != 0)
                return luaL_error(L, "plugin.show_settings_list: unknown layout");
        }
        lua_pop(L, 1);
        if (has_dashboard) {
            lua_getfield(L, 3, "markers");
            lua_Integer markers = luaL_optinteger(L, -1, 0);
            if (markers < 0 || markers > 12)
                return luaL_error(L, "plugin.show_settings_list: markers must be between 0 and 12");
            dashboard.markers = (int) markers;
            lua_pop(L, 1);
            lua_getfield(L, 3, "accent_first");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TBOOLEAN);
                dashboard.accent_first = lua_toboolean(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "active");
            if (!lua_isnil(L, -1)) luaL_checktype(L, -1, LUA_TBOOLEAN);
            dashboard.active = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "detail");
            const char * detail = luaL_optstring(L, -1, "");
            plugin_copy_label(dashboard.detail, sizeof(dashboard.detail), detail);
            lua_pop(L, 1);
        }

        lua_getfield(L, 3, "preview");
        if (!lua_isnil(L, -1)) {
            const char * preview_error = NULL;
            if (!plugin_parse_lock_screen_options(L, -1, &preview_options, &preview_error)) {
                return luaL_error(L, "plugin.show_settings_list: invalid preview options: %s",
                                  preview_error ? preview_error : "invalid options");
            }
            has_preview = true;
        }
        lua_pop(L, 1);
    }

    lua_Unsigned raw_n = lua_rawlen(L, 2);
    /* Validate the bounded dashboard shape before taking any Lua callback
     * references, so rejected input cannot leak registry ownership. */
    if (has_dashboard) {
        if (has_preview || raw_n != 7)
            return luaL_error(L, "plugin.show_settings_list: dashboard requires seven rows and no preview");
        for (int i = 1; i <= 7; i++) {
            lua_rawgeti(L, 2, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            lua_getfield(L, -1, "type");
            const char * type = luaL_checkstring(L, -1);
            if (strcmp(type, i == 2 ? "slider" : "row") != 0)
                return luaL_error(L, "plugin.show_settings_list: dashboard row 2 must be a slider; other rows must be actions");
            lua_pop(L, 1);
            if (i == 2) {
                lua_getfield(L, -1, "min");
                lua_Integer min = luaL_checkinteger(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "max");
                lua_Integer max = luaL_checkinteger(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "value");
                lua_Integer value = luaL_checkinteger(L, -1);
                lua_pop(L, 1);
                if (min < INT16_MIN || max > INT16_MAX || min >= max || value < min || value > max)
                    return luaL_error(L, "plugin.show_settings_list: dashboard slider requires an ordered int16 range and an in-range value");
            }
            lua_pop(L, 1);
        }
    }
    int n = (raw_n > (lua_Unsigned) PLUGIN_SETTINGS_LIST_MAX_ROWS) ? PLUGIN_SETTINGS_LIST_MAX_ROWS : (int) raw_n;

    static char label_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][512];
    static bool wrap_labels[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static const char * labels[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int row_types[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static bool toggle_initial[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_min[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_max[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_value[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static char icon_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][256];
    static const char * icon_paths[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int32_t heights[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int32_t widths[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static char text_size_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][8];
    static const char * text_sizes[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    int new_refs[PLUGIN_SETTINGS_LIST_MAX_ROWS];

    int count = 0;
    int slider_count = 0;
    /* Callbacks wait in this table (collected by the GC if a later row
     * raises) and only become registry refs once every row has validated. */
    lua_newtable(L);
    const int callbacks = lua_gettop(L);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1); /* row table -> stack top */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "type");
        const char * type = lua_tostring(L, -1);
        int row_type = -1;
        if (type) {
            if (strcmp(type, "row") == 0) row_type = PLUGIN_SETTINGS_ROW_TAP;
            else if (strcmp(type, "toggle") == 0) row_type = PLUGIN_SETTINGS_ROW_TOGGLE;
            else if (strcmp(type, "slider") == 0) row_type = PLUGIN_SETTINGS_ROW_SLIDER;
        }
        if (row_type < 0) {
            return luaL_error(L, "plugin.show_settings_list: row %d has an unknown or missing type (expected \"row\", \"toggle\", or \"slider\")", i + 1);
        }
        lua_pop(L, 1); /* type string */

        if (row_type == PLUGIN_SETTINGS_ROW_SLIDER && slider_count >= PLUGIN_SETTINGS_LIST_MAX_SLIDERS) {
            lua_pop(L, 1); /* row table */
            continue;
        }

        lua_getfield(L, -1, "wrap");
        if (!lua_isnil(L, -1)) luaL_checktype(L, -1, LUA_TBOOLEAN);
        wrap_labels[count] = row_type != PLUGIN_SETTINGS_ROW_SLIDER && lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "label");
        const char * label = lua_tostring(L, -1);
        snprintf(label_bufs[count], wrap_labels[count] ? sizeof(label_bufs[count]) : 96,
                 "%s", label ? label : "");
        lua_pop(L, 1); /* label string */

        const char * cb_field = (row_type == PLUGIN_SETTINGS_ROW_TAP) ? "on_select" : "on_change";
        lua_getfield(L, -1, cb_field);
        if (!lua_isfunction(L, -1)) {
            return luaL_error(L, "plugin.show_settings_list: row %d ('%s') missing %s function", i + 1, label ? label : "", cb_field);
        }
        lua_rawseti(L, callbacks, count + 1); /* pops the function */

        toggle_initial[count] = false;
        slider_min[count] = 0;
        slider_max[count] = 100;
        slider_value[count] = 0;
        if (row_type == PLUGIN_SETTINGS_ROW_TOGGLE) {
            lua_getfield(L, -1, "value");
            toggle_initial[count] = lua_toboolean(L, -1);
            lua_pop(L, 1);
        } else if (row_type == PLUGIN_SETTINGS_ROW_SLIDER) {
            slider_min[count] = (int) plugin_lua_opt_integer_field(L, -1, "min", 0);
            slider_max[count] = (int) plugin_lua_opt_integer_field(L, -1, "max", 100);
            slider_value[count] = (int) plugin_lua_opt_integer_field(L, -1, "value", slider_min[count]);
        }

        icon_bufs[count][0] = '\0';
        icon_paths[count] = NULL;
        lua_getfield(L, -1, "icon");
        const char * icon = lua_tostring(L, -1);
        if (icon) {
            snprintf(icon_bufs[count], sizeof(icon_bufs[count]), "%s", icon);
            icon_paths[count] = icon_bufs[count];
        }
        lua_pop(L, 1);

        heights[count] = (int32_t) plugin_lua_opt_integer_field(L, -1, "height", 0);

        widths[count] = (int32_t) plugin_lua_opt_integer_field(L, -1, "width", 0);

        text_size_bufs[count][0] = '\0';
        text_sizes[count] = NULL;
        lua_getfield(L, -1, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                return luaL_error(L, "plugin.show_settings_list: row %d has an unknown text_size '%s' (expected \"small\", \"medium\", \"large\", or \"mono\")",
                                   i + 1, text_size);
            }
            snprintf(text_size_bufs[count], sizeof(text_size_bufs[count]), "%s", text_size);
            text_sizes[count] = text_size_bufs[count];
        }
        lua_pop(L, 1);

        labels[count] = label_bufs[count];
        row_types[count] = row_type;
        if (row_type == PLUGIN_SETTINGS_ROW_SLIDER) slider_count++;
        count++;

        lua_pop(L, 1); /* row table */
    }
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, callbacks, i + 1);
        new_refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    int slot = gui_plugin_show_settings_list(title, row_types, labels, toggle_initial, slider_min, slider_max,
                                              slider_value, icon_paths, heights, widths, text_sizes, wrap_labels, count,
                                              update_existing, has_preview ? &preview_options : NULL,
                                              has_dashboard ? &dashboard : NULL);
    if (slot < 0 || slot >= PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE) {
        /* A failed native allocation did not take ownership of callbacks. */
        for (int i = 0; i < count; i++) luaL_unref(L, LUA_REGISTRYINDEX, new_refs[i]);
        return 0;
    }

    /* Release whatever the reused slot held from its previous population
     * before overwriting -- see plugin_settings_list_rows[]'s own comment on
     * why this is per-slot storage rather than one shared "current" ref. */
    for (int i = 0; i < plugin_settings_list_row_counts[slot]; i++) {
        plugin_unref(plugin_settings_list_rows[slot][i].L, plugin_settings_list_rows[slot][i].callback_ref);
    }
    for (int i = 0; i < count; i++) {
        plugin_settings_list_rows[slot][i].L = plugin_lua_main_state(L);
        plugin_settings_list_rows[slot][i].callback_ref = new_refs[i];
    }
    plugin_settings_list_row_counts[slot] = count;

    return 0;
}

/* Opens the native drag-and-toggle order editor for plugin-owned settings.
 * The UI owns copies of all strings/state; Lua only retains the callback. */
static int l_plugin_show_order_editor(lua_State * L) {
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "plugin.show_order_editor: title must be a string");
    size_t title_len = 0;
    const char * title = lua_tolstring(L, 1, &title_len);
    if (title_len > 127 || memchr(title, '\0', title_len))
        return luaL_error(L, "plugin.show_order_editor: title must be at most 127 bytes and contain no NUL");
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_Unsigned raw_n = lua_rawlen(L, 2);
    if (raw_n < 1 || raw_n > PLUGIN_ORDER_EDITOR_MAX_ITEMS)
        return luaL_error(L, "plugin.show_order_editor: items must contain 1 to %d entries",
                          PLUGIN_ORDER_EDITOR_MAX_ITEMS);

    int seen_count = 0;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (!lua_isinteger(L, -2))
            return luaL_error(L, "plugin.show_order_editor: items must be a dense array");
        lua_Integer index = lua_tointeger(L, -2);
        if (index < 1 || index > (lua_Integer)raw_n)
            return luaL_error(L, "plugin.show_order_editor: items must be a dense array");
        ++seen_count;
        lua_pop(L, 1);
    }
    if (seen_count != (int)raw_n)
        return luaL_error(L, "plugin.show_order_editor: items must be a dense array");

    char key_bufs[PLUGIN_ORDER_EDITOR_MAX_ITEMS][64];
    char label_bufs[PLUGIN_ORDER_EDITOR_MAX_ITEMS][128];
    const char * keys[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    const char * labels[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    bool visible[PLUGIN_ORDER_EDITOR_MAX_ITEMS];
    int count = (int)raw_n;
    for (int i = 0; i < count; ++i) {
        lua_rawgeti(L, 2, i + 1);
        if (!lua_istable(L, -1))
            return luaL_error(L, "plugin.show_order_editor: item %d must be a table", i + 1);
        lua_getfield(L, -1, "key");
        size_t key_len = 0;
        const char * key = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &key_len) : NULL;
        if (!key || key_len == 0 || key_len >= sizeof(key_bufs[i]) || memchr(key, '\0', key_len))
            return luaL_error(L, "plugin.show_order_editor: item %d key must be 1 to %zu bytes",
                              i + 1, sizeof(key_bufs[i]) - 1);
        memcpy(key_bufs[i], key, key_len);
        key_bufs[i][key_len] = '\0';
        lua_pop(L, 1);
        for (int j = 0; j < i; ++j)
            if (strcmp(key_bufs[i], key_bufs[j]) == 0)
                return luaL_error(L, "plugin.show_order_editor: duplicate key '%s'", key_bufs[i]);

        lua_getfield(L, -1, "label");
        size_t label_len = 0;
        const char * label = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &label_len) : NULL;
        if (!label || label_len >= sizeof(label_bufs[i]) || memchr(label, '\0', label_len))
            return luaL_error(L, "plugin.show_order_editor: item %d label must be at most %zu bytes",
                              i + 1, sizeof(label_bufs[i]) - 1);
        memcpy(label_bufs[i], label, label_len);
        label_bufs[i][label_len] = '\0';
        lua_pop(L, 1);

        lua_getfield(L, -1, "visible");
        if (!lua_isboolean(L, -1))
            return luaL_error(L, "plugin.show_order_editor: item %d visible must be a boolean", i + 1);
        visible[i] = lua_toboolean(L, -1);
        lua_pop(L, 2); /* visible, item table */
        keys[i] = key_bufs[i];
        labels[i] = label_bufs[i];
    }

    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int slot = gui_plugin_show_order_editor(title, keys, labels, visible, count);
    if (slot != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, new_ref);
        return luaL_error(L, "plugin.show_order_editor: editor is already open or unavailable");
    }
    plugin_unref(plugin_order_editor_callback.L, plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = plugin_lua_main_state(L);
    plugin_order_editor_callback.callback_ref = new_ref;
    return 0;
}

static int l_plugin_show_lock_screen(lua_State * L) {
    gui_lock_screen_options_t opts;
    const char * error = NULL;
    if (!plugin_parse_lock_screen_options(L, 1, &opts, &error)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, error ? error : "invalid options");
        return 2;
    }

    bool ok = gui_lock_screen_show(&opts);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushliteral(L, "failed to show lock screen");
        return 2;
    }
    return 1;
}

void plugin_manager_list_item_selected(int slot, int index) {
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) return;
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (!cb->L || cb->select_ref == LUA_NOREF) return;

    if (!plugin_push_callback(cb->L, cb->select_ref)) return;
    lua_pushinteger(cb->L, index + 1);
    plugin_call_report(cb->L, 1, true, "show_list on_select error");
}

/* Long press on a row: true when the plugin's on_hold ran, so the caller
 * can swallow the release that would otherwise select the row. */
bool plugin_manager_list_item_held(int slot, int index) {
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) return false;
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (!cb->L || cb->hold_ref == LUA_NOREF) return false;

    if (!plugin_push_callback(cb->L, cb->hold_ref)) return false;
    lua_pushinteger(cb->L, index + 1);
    plugin_call_report(cb->L, 1, true, "show_list on_hold error");
    return true;
}

/* Shared bounds check for the three plugin_manager_settings_list_*() below --
 * out-of-range or never-populated is a silent no-op, same tolerance
 * plugin_manager_list_item_selected() above has for "no show_list() call is
 * current yet". */
static bool settings_list_row_ref(int slot, int row, lua_State ** out_L, int * out_ref) {
    if (slot < 0 || slot >= PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE) return false;
    if (row < 0 || row >= plugin_settings_list_row_counts[slot]) return false;
    plugin_settings_list_row_ref_t * r = &plugin_settings_list_rows[slot][row];
    if (!r->L) return false;
    *out_L = r->L;
    *out_ref = r->callback_ref;
    return true;
}

void plugin_manager_settings_list_row_selected(int slot, int row) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    if (!plugin_push_callback(L, ref)) return;
    plugin_call_report(L, 0, true, "show_settings_list on_select error");
}

void plugin_manager_settings_list_toggled(int slot, int row, bool new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    if (!plugin_push_callback(L, ref)) return;
    lua_pushboolean(L, new_value);
    plugin_call_report(L, 1, false, "show_settings_list on_change error");
}

void plugin_manager_settings_list_slid(int slot, int row, int new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    if (!plugin_push_callback(L, ref)) return;
    lua_pushinteger(L, new_value);
    plugin_call_report(L, 1, false, "show_settings_list on_change error");
}

void plugin_manager_order_editor_changed(int slot, const char * const * keys,
                                         const char * const * labels, const bool * visible, int count) {
    if (slot != 0 || !plugin_order_editor_callback.L ||
        plugin_order_editor_callback.callback_ref == LUA_NOREF ||
        !keys || !labels || !visible || count < 1 || count > PLUGIN_ORDER_EDITOR_MAX_ITEMS) return;
    lua_State * L = plugin_order_editor_callback.L;
    if (!plugin_push_callback(L, plugin_order_editor_callback.callback_ref)) return;
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, keys[i] ? keys[i] : "");
        lua_setfield(L, -2, "key");
        lua_pushstring(L, labels[i] ? labels[i] : "");
        lua_setfield(L, -2, "label");
        lua_pushboolean(L, visible[i]);
        lua_setfield(L, -2, "visible");
        lua_rawseti(L, -2, i + 1);
    }
    plugin_call_report(L, 1, false, "show_order_editor on_change error");
}

void plugin_manager_order_editor_closed(int slot) {
    if (slot != 0) return;
    plugin_unref(plugin_order_editor_callback.L, plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = NULL;
    plugin_order_editor_callback.callback_ref = LUA_NOREF;
}

const luaL_Reg plugin_list_screens_funcs[] = {
    { "show_list",                 l_plugin_show_list },
    { "is_list_showing",           l_plugin_is_list_showing },
    { "show_settings_list",        l_plugin_show_settings_list },
    { "show_order_editor",         l_plugin_show_order_editor },
    { "show_lock_screen",          l_plugin_show_lock_screen },
    { NULL, NULL }
};
