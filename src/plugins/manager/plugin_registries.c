/* Registries for plugin list rows, Home/stream tiles and quick toggles, with the accessors the native screens call. */
#include "../../ui/text/i18n/i18n.h"
#include "../../ui/application/gui.h"
#include "../lua/plugin_lua_runtime.h"
#include "plugin_internal.h"
#include <string.h>
#include <unistd.h>
#include "plugin_manager_internal.h"

typedef struct {
    const char * list_id;
    const char * kind;
    int max_items;
} plugin_list_target_desc_t;

static const plugin_list_target_desc_t plugin_list_targets[PLUGIN_LIST_TARGET_COUNT] = {
    [PLUGIN_LIST_TARGET_BOOKS] = { "books", "books list item", PLUGIN_MAX_BOOKS_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MUSIC] = { "music", "music list item", PLUGIN_MAX_MUSIC_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_SETTINGS] = { "settings", "settings list item", PLUGIN_MAX_SETTINGS_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_DISPLAY] = { "display", "display list item", PLUGIN_MAX_DISPLAY_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_PLAYBACK] = { "playback", "playback list item", PLUGIN_MAX_PLAYBACK_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MUSIC_AUDIO] = { "music_audio", "music_audio list item", PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MUSIC_CONTROLS] = { "music_controls", "music_controls list item", PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MUSIC_TIMERS] = { "music_timers", "music_timers list item", PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MUSIC_LIBRARY] = { "music_library", "music_library list item", PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_POWER] = { "power", "power list item", PLUGIN_MAX_POWER_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_SYSTEM] = { "system", "system list item", PLUGIN_MAX_SYSTEM_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_PLAYER] = { "player", "player menu item", PLUGIN_MAX_PLAYER_LIST_ITEMS },
    [PLUGIN_LIST_TARGET_MORE] = { "more", "more list item", PLUGIN_MAX_MORE_LIST_ITEMS },
};
_Static_assert(PLUGIN_MAX_BOOKS_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "books registry capacity");
_Static_assert(PLUGIN_MAX_MUSIC_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "music registry capacity");
_Static_assert(PLUGIN_MAX_SETTINGS_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "settings registry capacity");
_Static_assert(PLUGIN_MAX_DISPLAY_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "display registry capacity");
_Static_assert(PLUGIN_MAX_PLAYBACK_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "playback registry capacity");
_Static_assert(PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "music_audio registry capacity");
_Static_assert(PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "music_controls registry capacity");
_Static_assert(PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "music_timers registry capacity");
_Static_assert(PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "music_library registry capacity");
_Static_assert(PLUGIN_MAX_POWER_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "power registry capacity");
_Static_assert(PLUGIN_MAX_SYSTEM_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "system registry capacity");
_Static_assert(PLUGIN_MAX_PLAYER_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "player registry capacity");
_Static_assert(PLUGIN_MAX_MORE_LIST_ITEMS <= PLUGIN_LIST_REGISTRY_CAPACITY, "more registry capacity");
plugin_list_item_t plugin_list_items[PLUGIN_LIST_TARGET_COUNT][PLUGIN_LIST_REGISTRY_CAPACITY];
int plugin_list_item_counts[PLUGIN_LIST_TARGET_COUNT];
static uint32_t plugin_list_registry_generation = 1;

void advance_list_registry_generation(void) {
    ++plugin_list_registry_generation;
    if (plugin_list_registry_generation == 0) ++plugin_list_registry_generation;
}

/* Separate registry for plugin.register_stream_media_tile() -- same
 * plugin_tile_t shape, different surface (gui.c's Stream Media screen).
 * See PLUGIN_MAX_STREAM_TILES's own comment in plugin_manager.h for why
 * Stream Media gets real tiles when Home doesn't. */
plugin_tile_t plugin_stream_tiles[PLUGIN_MAX_STREAM_TILES];
int plugin_stream_tile_count = 0;

/* Separate registry for plugin.register_home_tile() -- same plugin_tile_t
 * shape (its id field is what's actually used here), different surface
 * (gui_settings.c's build_home_screen(), via home_layout_config.order[]/
 * tiles[] naming a tile by this id). */
plugin_tile_t plugin_home_tiles[PLUGIN_MAX_HOME_TILES];
int plugin_home_tile_count = 0;

plugin_quick_toggle_t plugin_quick_toggles[PLUGIN_MAX_QUICK_TOGGLES];
int plugin_quick_toggle_count = 0;

/* Copies icon and derives its selected-state sibling the way every real icon
 * pair in this app is named: extension stripped, "_s.png" appended. The strip
 * goes through an 80-byte scratch buffer, so callers that must not truncate
 * check strlen(icon) < 80 first. */
static void plugin_icon_pair(const char * icon, char * icon_dst, char * selected_dst, size_t n) {
    char base[80];
    snprintf(base, sizeof(base), "%s", icon);
    char * dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(icon_dst, n, "%s", icon);
    snprintf(selected_dst, n, "%s_s.png", base);
}

/* Rewrites a relative icon to <sd>/.plugins/<icon> when that file is readable,
 * so a plugin can ship images in its own folder (same base as settings row
 * icons). Otherwise, or if the absolute path does not fit, the theme-relative
 * path is kept. Absolute paths are left alone. */
static void plugin_resolve_icon_path(char * icon, size_t n) {
    if (icon[0] == '\0' || icon[0] == '/') return;
    char abs_path[sizeof(plugin_quick_toggles[0].icon)];
    int len = snprintf(abs_path, sizeof(abs_path), MUSIC_ROOT_DIR "/.plugins/%s", icon);
    if (len < 0 || (size_t) len >= sizeof(abs_path) || (size_t) len >= n) return;
    if (access(abs_path, R_OK) != 0) return;
    memcpy(icon, abs_path, (size_t) len + 1);
}

/* Used by l_plugin_register_stream_media_tile() -- fills t->icon/icon_selected
 * from an explicit icon string (deriving the "_s" selected-state variant
 * the same way every real icon pair in this app is named), or from
 * (default_icon, default_icon_selected) if icon is NULL. */
static void fill_tile_icon(plugin_tile_t * t, const char * icon, const char * default_icon,
                            const char * default_icon_selected) {
    if (icon) {
        plugin_icon_pair(icon, t->icon, t->icon_selected, sizeof(t->icon));
    } else {
        snprintf(t->icon, sizeof(t->icon), "%s", default_icon);
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s", default_icon_selected);
    }
}

/* Validates a text_size string against the four recognized tiers -- shared
 * by append_list_item() below, l_plugin_show_settings_list()/
 * l_plugin_show_list(), and l_plugin_set_home_layout() (fail loudly at the
 * Lua boundary, same convention every other unrecognized-string case in this
 * file already uses; the resolved font itself is picked later, in
 * screen_builders.c's pill_row_resolve_text_size(), which trusts this was
 * already validated). "mono" (lv_font_unscii_16, ASCII-only -- see
 * pill_row_resolve_text_size()'s own comment) is available to every plugin
 * row this validates, not only plugin.set_home_layout()'s tiles. */
bool is_valid_text_size(const char * text_size) {
    return strcmp(text_size, "small") == 0 || strcmp(text_size, "medium") == 0 || strcmp(text_size, "large") == 0 ||
           strcmp(text_size, "mono") == 0;
}

/* Shared by l_plugin_register_list_item() below, once its capacity check
 * for the target array has already passed -- pushes on_open (Lua stack
 * index 3 in the caller) into the registry and appends {L, ref, label}, plus
 * this call's optional 4th `options` table ({ icon, height, width, text_size },
 * PLUGINS.md) if present. icon_path/text_size are left as empty strings
 * (not touched at all here, matching the struct's already-zero-initialized
 * static-array storage) when `options` is absent or doesn't set that field
 * -- plugin_manager_get_*_list_item_options() below is what translates that
 * back to NULL for its own callers. */
static void append_list_item(plugin_list_item_t * array, int * count, lua_State * L, const char * label) {
    char group_copy[33] = { 0 };
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "group");
        if (!lua_isnil(L, -1)) {
            size_t group_len = 0;
            bool is_string = lua_type(L, -1) == LUA_TSTRING;
            const char * group = lua_tolstring(L, -1, &group_len);
            if (!is_string || !group || group_len > 32 || memchr(group, '\0', group_len)) {
                lua_pop(L, 1);
                luaL_error(L, "plugin.register_list_item: group must be a string of at most 32 bytes without NUL");
            }
            if (group_len) memcpy(group_copy, group, group_len);
        }
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_list_item_t * item = &array[(*count)++];
    item->L = plugin_lua_main_state(L);
    item->open_ref = ref;
    plugin_copy_label(item->label, sizeof(item->label), label);
    item->icon_path[0] = '\0';
    item->row_height = 0;
    item->row_width = 0;
    item->text_size[0] = '\0';
    snprintf(item->group, sizeof(item->group), "%s", group_copy);

    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "icon");
        const char * icon = lua_tostring(L, -1);
        if (icon) snprintf(item->icon_path, sizeof(item->icon_path), "%s", icon);
        lua_pop(L, 1);

        item->row_height = (int32_t) plugin_lua_opt_integer_field(L, 4, "height", 0);

        item->row_width = (int32_t) plugin_lua_opt_integer_field(L, 4, "width", 0);

        lua_getfield(L, 4, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                lua_pop(L, 1);
                luaL_error(L, "plugin.register_list_item: unknown text_size '%s' (expected \"small\", \"medium\", \"large\", or \"mono\")",
                           text_size);
            }
            snprintf(item->text_size, sizeof(item->text_size), "%s", text_size);
        }
        lua_pop(L, 1);

    }
}

/* Adds a row to an existing native list screen -- see PLUGIN_MAX_BOOKS_
 * LIST_ITEMS/PLUGIN_MAX_SETTINGS_LIST_ITEMS's own comments in plugin_
 * manager.h. list_id is checked against plugin_list_targets[]'s own fixed
 * set of recognized strings ("books", "settings", ...) rather than accepted
 * as-is: a typo'd or unsupported list_id should fail loudly at plugin load
 * time, not silently register into nothing. */
static int l_plugin_register_list_item(lua_State * L) {
    const char * list_id = luaL_checkstring(L, 1);
    const char * label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    for (int i = 0; i < PLUGIN_LIST_TARGET_COUNT; i++) {
        if (strcmp(list_id, plugin_list_targets[i].list_id) == 0) {
            if (plugin_list_item_counts[i] >= plugin_list_targets[i].max_items) {
                return luaL_error(L, "plugin.register_list_item: too many items registered for \"%s\" (max %d)",
                                  plugin_list_targets[i].list_id, plugin_list_targets[i].max_items);
            }
            append_list_item(plugin_list_items[i], &plugin_list_item_counts[i], L, label);
            return 0;
        }
    }

    return luaL_error(L, "plugin.register_list_item: unknown list_id '%s' (expected \"books\", \"music\", \"settings\", \"display\", \"playback\", \"music_audio\", \"music_controls\", \"music_timers\", \"music_library\", \"power\", \"system\", or \"player\")", list_id);
}

/* Registers a Stream Media tile, appended after the built-in Subsonic tile.
 * Defaults icon to "stream_media/radio.png" if not specified. */
static int l_plugin_register_stream_media_tile(lua_State * L) {
    const char * label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char * icon = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? luaL_checkstring(L, 3) : NULL;

    if (plugin_stream_tile_count >= PLUGIN_MAX_STREAM_TILES) {
        return luaL_error(L, "too many stream media tiles registered (max %d)", PLUGIN_MAX_STREAM_TILES);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_tile_t * t = &plugin_stream_tiles[plugin_stream_tile_count++];
    t->L = plugin_lua_main_state(L);
    t->open_ref = ref;
    plugin_copy_label(t->label, sizeof(t->label), label);
    fill_tile_icon(t, icon, "stream_media/radio.png", "stream_media/radio_s.png");
    return 0;
}

/* Home tile ids share the plugin id charset (plugin_id_charset_ok): a tile id
 * rides inside a .theme file's comma-separated home_order= line, so it must
 * not contain a comma or whitespace. */

/* Registers a tile a theme can place on Home via set_home_layout()'s
 * `options.order`. The `icon` parameter is required.
 * Gated on id not colliding with any of the fixed native tile keys
 * (home_layout_tile_keys[]). */
static int l_plugin_register_home_tile(lua_State * L) {
    const char * id = luaL_checkstring(L, 1);
    const char * label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    const char * icon = luaL_checkstring(L, 4);

    /* Validate ID format and maximum length before storage. */
    if (!plugin_id_charset_ok(id) || strlen(id) >= sizeof(plugin_home_tiles[0].id)) {
        return luaL_error(L, "plugin.register_home_tile: id must be 1-%zu characters using letters, digits, '.', '_' or '-'",
                           sizeof(plugin_home_tiles[0].id) - 1);
    }
    for (int k = 0; k < HOME_LAYOUT_TILE_COUNT; k++) {
        if (strcmp(id, home_layout_tile_keys[k]) == 0) {
            return luaL_error(L, "plugin.register_home_tile: id '%s' is reserved for the native Home tile of "
                               "the same name -- pick a different id", id);
        }
    }
    if (plugin_manager_find_home_tile_by_id(id) >= 0) {
        return luaL_error(L, "plugin.register_home_tile: id '%s' is already registered", id);
    }
    if (plugin_home_tile_count >= PLUGIN_MAX_HOME_TILES) {
        return luaL_error(L, "too many home tiles registered (max %d)", PLUGIN_MAX_HOME_TILES);
    }
    if (icon[0] == '\0') {
        return luaL_error(L, "plugin.register_home_tile: icon must not be empty");
    }
    /* fill_tile_icon()'s own internal scratch buffer for stripping the
     * extension before deriving "_s.png" is a 80-byte `char base[80]` --
     * smaller than t->icon's real 96-byte destination. An icon at or past
     * that length would silently truncate inside fill_tile_icon() BEFORE
     * the extension strip runs, so t->icon (never truncated, holds the
     * full icon string up to 95 chars) and t->icon_selected (derived from
     * the truncated, shorter base) would end up describing two unrelated
     * files, not just a shorter pair. Reject here, before that mismatch
     * can happen, rather than silently store two diverging paths. */
    if (strlen(icon) >= 80) {
        return luaL_error(L, "plugin.register_home_tile: icon path '%s' is too long (max 79 characters)", icon);
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_tile_t * t = &plugin_home_tiles[plugin_home_tile_count++];
    t->L = plugin_lua_main_state(L);
    t->open_ref = ref;
    snprintf(t->id, sizeof(t->id), "%s", id);
    plugin_copy_label(t->label, sizeof(t->label), label);
    fill_tile_icon(t, icon, NULL, NULL);
    return 0;
}

static int plugin_find_quick_toggle_by_id(const char * id) {
    if (!id) return -1;
    for (int i = 0; i < plugin_quick_toggle_count; i++) {
        if (strcmp(plugin_quick_toggles[i].id, id) == 0) return i;
    }
    return -1;
}

/* plugin.register_quick_toggle(id, label, on_change, options) -- adds an
 * on/off tile to the quick drawer's expanded third row. `options` carries
 * icon (required), plus the optional icon_selected / value / on_text /
 * off_text. Same id rules as register_home_tile(); the "_s.png" sibling is
 * derived from `icon` the same way unless icon_selected says otherwise, so
 * a plugin shipping the usual pair only names one path. */
static int l_plugin_register_quick_toggle(lua_State * L) {
    const char * id = luaL_checkstring(L, 1);
    const char * label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (!plugin_id_charset_ok(id) || strlen(id) >= sizeof(plugin_quick_toggles[0].id)) {
        return luaL_error(L, "plugin.register_quick_toggle: id must be 1-%zu characters using letters, digits, '.', '_' or '-'",
                          sizeof(plugin_quick_toggles[0].id) - 1);
    }
    if (plugin_find_quick_toggle_by_id(id) >= 0) {
        return luaL_error(L, "plugin.register_quick_toggle: id '%s' is already registered", id);
    }
    if (plugin_quick_toggle_count >= PLUGIN_MAX_QUICK_TOGGLES) {
        return luaL_error(L, "too many quick toggles registered (max %d)", PLUGIN_MAX_QUICK_TOGGLES);
    }
    if (lua_gettop(L) < 4 || !lua_istable(L, 4)) {
        return luaL_error(L, "plugin.register_quick_toggle: options table with an `icon` is required");
    }

    lua_getfield(L, 4, "icon");
    const char * icon = lua_tostring(L, -1);
    /* Same 80-byte ceiling and same reason as register_home_tile()'s own
     * check -- the extension strip below runs through a shorter scratch
     * buffer than the stored path. */
    if (!icon || icon[0] == '\0' || strlen(icon) >= 80) {
        lua_pop(L, 1);
        return luaL_error(L, "plugin.register_quick_toggle: icon must be a non-empty path under 80 characters");
    }
    /* 80, matching the length check above, so the extension-strip copy below
     * is same-size and cannot truncate. */
    char icon_copy[80];
    snprintf(icon_copy, sizeof(icon_copy), "%s", icon);
    lua_pop(L, 1);

    plugin_quick_toggle_t * t = &plugin_quick_toggles[plugin_quick_toggle_count];

    plugin_icon_pair(icon_copy, t->icon, t->icon_selected, sizeof(t->icon));

    lua_getfield(L, 4, "icon_selected");
    const char * icon_sel = lua_tostring(L, -1);
    if (icon_sel && icon_sel[0] != '\0')
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s", icon_sel);
    lua_pop(L, 1);
    plugin_resolve_icon_path(t->icon, sizeof(t->icon));
    plugin_resolve_icon_path(t->icon_selected, sizeof(t->icon_selected));

    lua_getfield(L, 4, "on_text");
    const char * on_text = lua_tostring(L, -1);
    snprintf(t->on_text, sizeof(t->on_text), "%s", on_text && on_text[0] ? on_text : TR("On"));
    utf8_sanitize(t->on_text);
    lua_pop(L, 1);

    lua_getfield(L, 4, "off_text");
    const char * off_text = lua_tostring(L, -1);
    snprintf(t->off_text, sizeof(t->off_text), "%s", off_text && off_text[0] ? off_text : TR("Off"));
    utf8_sanitize(t->off_text);
    lua_pop(L, 1);

    lua_getfield(L, 4, "value");
    t->value = lua_toboolean(L, -1);
    lua_pop(L, 1);

    t->on_hold_ref = LUA_NOREF;
    lua_getfield(L, 4, "on_hold");
    if (!lua_isnil(L, -1)) {
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "plugin.register_quick_toggle: on_hold must be a function");
        }
        t->on_hold_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    /* Ref taken last: every rejection above must leave the registry and the
     * Lua registry untouched, not leak a ref for a toggle that never landed. */
    lua_pushvalue(L, 3);
    t->change_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    t->L = plugin_lua_main_state(L);
    snprintf(t->id, sizeof(t->id), "%s", id);
    plugin_copy_label(t->label, sizeof(t->label), label);
    plugin_quick_toggle_count++;
    return 0;
}

/* plugin.set_quick_toggle(id, value) -- publishes a state change the plugin
 * made on its own (from its settings screen, say) so the drawer shows it the
 * next time it opens. Deliberately does NOT fire on_change: the plugin is the
 * one telling us, so calling back into it would just loop. */
static int l_plugin_set_quick_toggle(lua_State * L) {
    const char * id = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    int index = plugin_find_quick_toggle_by_id(id);
    if (index < 0) {
        return luaL_error(L, "plugin.set_quick_toggle: no quick toggle registered with id '%s'", id);
    }
    plugin_quick_toggles[index].value = lua_toboolean(L, 2);
    return 0;
}

/* Shared by plugin_manager_books_list_item_clicked()/_settings_list_item_
 * clicked() below -- kind is just for the stderr message ("books list
 * item"/"settings list item"), so a load-time error in one plugin's row is
 * distinguishable from another's in the log. */
static void dispatch_list_item_open(plugin_list_item_t * item, const char * kind) {
    if (!plugin_push_callback(item->L, item->open_ref)) return;
    plugin_call_report(item->L, 0, true, "%s '%s' on_open error", kind, item->label);
}

/* Shared by every plugin_manager_get_*_list_item_options() below --
 * translates a plugin_list_item_t's empty-string "unset" convention back to
 * NULL, matching what screen_builders.c's pill_row_apply_icon()/
 * pill_row_resolve_text_size() already expect. Out-of-range index leaves
 * every out param at its "unset" value, same tolerance every other
 * out-of-range accessor in this family already has. */
static void get_list_item_options(const plugin_list_item_t * array, int count, int index, const char ** out_icon,
                                   int32_t * out_height, int32_t * out_width, const char ** out_text_size) {
    *out_icon = NULL;
    *out_height = 0;
    *out_width = 0;
    *out_text_size = NULL;
    if (index < 0 || index >= count) return;

    const plugin_list_item_t * item = &array[index];
    if (item->icon_path[0]) *out_icon = item->icon_path;
    *out_height = item->row_height;
    *out_width = item->row_width;
    if (item->text_size[0]) *out_text_size = item->text_size;
}

static int generic_list_item_count(int target) {
    if (target < 0 || target >= PLUGIN_LIST_TARGET_COUNT) return 0;
    return plugin_list_item_counts[target];
}

static const char * generic_list_item_label(int target, int index) {
    if (target < 0 || target >= PLUGIN_LIST_TARGET_COUNT) return "";
    if (index < 0 || index >= plugin_list_item_counts[target] || index >= plugin_list_targets[target].max_items) return "";
    return plugin_list_items[target][index].label;
}

const char * plugin_manager_get_list_item_group(const char * list_id, int index) {
    if (!list_id) return NULL;
    for (int target = 0; target < PLUGIN_LIST_TARGET_COUNT; target++) {
        if (strcmp(list_id, plugin_list_targets[target].list_id) != 0) continue;
        if (index < 0 || index >= plugin_list_item_counts[target] || index >= plugin_list_targets[target].max_items) return NULL;
        const char * group = plugin_list_items[target][index].group;
        return group[0] ? group : NULL;
    }
    return NULL;
}

static void generic_list_item_clicked(int target, int index) {
    if (target < 0 || target >= PLUGIN_LIST_TARGET_COUNT) return;
    if (index < 0 || index >= plugin_list_item_counts[target] || index >= plugin_list_targets[target].max_items) return;
    dispatch_list_item_open(&plugin_list_items[target][index], plugin_list_targets[target].kind);
}

static void generic_list_item_options(int target, int index, const char ** out_icon, int32_t * out_height,
                                      int32_t * out_width, const char ** out_text_size) {
    if (target < 0 || target >= PLUGIN_LIST_TARGET_COUNT) {
        *out_icon = NULL;
        *out_height = 0;
        *out_width = 0;
        *out_text_size = NULL;
        return;
    }
    int count = plugin_list_item_counts[target];
    if (count > plugin_list_targets[target].max_items) {
        count = plugin_list_targets[target].max_items;
    }
    get_list_item_options(plugin_list_items[target], count, index, out_icon, out_height, out_width, out_text_size);
}

/* The public per-screen accessors (see plugin_manager.h): count, label,
 * clicked and options for one list target, e.g. PLUGIN_LIST_TARGET_ACCESSORS(
 * books, BOOKS) defines plugin_manager_get_books_list_item_count() and its
 * three siblings. The player menu has no options accessor, so it is spelled
 * out below. */
#define PLUGIN_LIST_TARGET_ACCESSORS(name, TARGET) \
    int plugin_manager_get_##name##_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_##TARGET); } \
    const char * plugin_manager_get_##name##_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_##TARGET, index); } \
    void plugin_manager_##name##_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_##TARGET, index); } \
    void plugin_manager_get_##name##_list_item_options(int index, const char ** out_icon, int32_t * out_height, \
                                                      int32_t * out_width, const char ** out_text_size) { \
        generic_list_item_options(PLUGIN_LIST_TARGET_##TARGET, index, out_icon, out_height, out_width, out_text_size); \
    }

PLUGIN_LIST_TARGET_ACCESSORS(books, BOOKS)

PLUGIN_LIST_TARGET_ACCESSORS(music, MUSIC)

/* Stable order key "plugin:<plugin id>:<n>"; n is the entry's ordinal among
 * the same plugin's entries in registration order. */
static bool plugin_order_key(lua_State * L, int ordinal, char * out, size_t cap) {
    const plugin_instance_t * inst = plugin_instance_for_state(L);
    return inst && snprintf(out, cap, "plugin:%s:%d", inst->id, ordinal) < (int) cap;
}

bool plugin_manager_get_music_list_item_key(int index, char * out, size_t cap) {
    const plugin_list_item_t * items = plugin_list_items[PLUGIN_LIST_TARGET_MUSIC];
    if (index < 0 || index >= plugin_list_item_counts[PLUGIN_LIST_TARGET_MUSIC] ||
        index >= plugin_list_targets[PLUGIN_LIST_TARGET_MUSIC].max_items) return false;
    int ordinal = 0;
    for (int i = 0; i < index; i++)
        if (items[i].L == items[index].L) ordinal++;
    return plugin_order_key(items[index].L, ordinal, out, cap);
}

bool plugin_manager_get_quick_toggle_key(int index, char * out, size_t cap) {
    if (index < 0 || index >= plugin_quick_toggle_count) return false;
    int ordinal = 0;
    for (int i = 0; i < index; i++)
        if (plugin_quick_toggles[i].L == plugin_quick_toggles[index].L) ordinal++;
    return plugin_order_key(plugin_quick_toggles[index].L, ordinal, out, cap);
}

PLUGIN_LIST_TARGET_ACCESSORS(settings, SETTINGS)

static bool settings_item_matches_file(const plugin_list_item_t * item, const char * filename) {
    if (!item || !filename || !filename[0] || !item->L || item->open_ref == LUA_NOREF || item->open_ref == LUA_REFNIL)
        return false;
    plugin_instance_t * owner = plugin_instance_for_state(item->L);
    return owner && owner->defined && !owner->disposing && !owner->aborted &&
           strcmp(owner->filename, filename) == 0;
}

/* Every list that is a page under Settings. Books, Music, the player menu and
 * More are not settings pages, so their rows never count. Order decides which
 * row is the primary one: Additional Tools first, then the sections. */
static const int plugin_settings_targets[] = {
    PLUGIN_LIST_TARGET_SETTINGS,       PLUGIN_LIST_TARGET_DISPLAY,        PLUGIN_LIST_TARGET_PLAYBACK,
    PLUGIN_LIST_TARGET_MUSIC_AUDIO,    PLUGIN_LIST_TARGET_MUSIC_CONTROLS, PLUGIN_LIST_TARGET_MUSIC_TIMERS,
    PLUGIN_LIST_TARGET_MUSIC_LIBRARY,  PLUGIN_LIST_TARGET_POWER,          PLUGIN_LIST_TARGET_SYSTEM,
};

/* First registered settings row of this exact file, across all settings lists. */
static bool find_file_settings_row(const char * filename, int * out_target, int * out_index) {
    for (size_t t = 0; t < sizeof(plugin_settings_targets) / sizeof(plugin_settings_targets[0]); t++) {
        int target = plugin_settings_targets[t];
        int count = generic_list_item_count(target);
        for (int i = 0; i < count; i++) {
            if (!settings_item_matches_file(&plugin_list_items[target][i], filename)) continue;
            *out_target = target;
            *out_index = i;
            return true;
        }
    }
    return false;
}

bool plugin_manager_file_has_settings(const char * filename) {
    int target, index;
    return find_file_settings_row(filename, &target, &index);
}

bool plugin_manager_open_file_settings(const char * filename) {
    int target, index;
    if (!find_file_settings_row(filename, &target, &index)) return false;
    /* Keep the normal settings-row dispatch, which applies the user-action
     * call budget and Lua error handling. Registration order selects the
     * plugin's primary settings entry when it registered several. */
    generic_list_item_clicked(target, index);
    return true;
}

PLUGIN_LIST_TARGET_ACCESSORS(display, DISPLAY)

PLUGIN_LIST_TARGET_ACCESSORS(playback, PLAYBACK)

PLUGIN_LIST_TARGET_ACCESSORS(music_audio, MUSIC_AUDIO)

PLUGIN_LIST_TARGET_ACCESSORS(music_controls, MUSIC_CONTROLS)

PLUGIN_LIST_TARGET_ACCESSORS(music_timers, MUSIC_TIMERS)

PLUGIN_LIST_TARGET_ACCESSORS(music_library, MUSIC_LIBRARY)

PLUGIN_LIST_TARGET_ACCESSORS(power, POWER)

PLUGIN_LIST_TARGET_ACCESSORS(system, SYSTEM)

PLUGIN_LIST_TARGET_ACCESSORS(more, MORE)

int plugin_manager_get_player_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_PLAYER); }
const char * plugin_manager_get_player_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_PLAYER, index); }
void plugin_manager_player_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_PLAYER, index); }
uint32_t plugin_manager_get_list_registry_generation(void) { return plugin_list_registry_generation; }

/* Runs a stream or home tile's on_open callback. */
static void dispatch_tile_open(plugin_tile_t * t) {
    if (!plugin_push_callback(t->L, t->open_ref)) return;
    plugin_call_report(t->L, 0, true, "tile '%s' on_open error", t->label);
}

int plugin_manager_get_stream_tile_count(void) {
    return plugin_stream_tile_count;
}

const char * plugin_manager_get_stream_tile_label(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "";
    return plugin_stream_tiles[index].label;
}

const char * plugin_manager_get_stream_tile_icon(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "stream_media/radio.png";
    return plugin_stream_tiles[index].icon;
}

const char * plugin_manager_get_stream_tile_icon_selected(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "stream_media/radio_s.png";
    return plugin_stream_tiles[index].icon_selected;
}

void plugin_manager_stream_tile_clicked(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return;
    dispatch_tile_open(&plugin_stream_tiles[index]);
}

int plugin_manager_find_home_tile_by_id(const char * id) {
    if (!id) return -1;
    for (int i = 0; i < plugin_home_tile_count; i++) {
        if (strcmp(plugin_home_tiles[i].id, id) == 0) return i;
    }
    return -1;
}

const char * plugin_manager_get_home_tile_label(int index) {
    if (index < 0 || index >= plugin_home_tile_count) return "";
    return plugin_home_tiles[index].label;
}

const char * plugin_manager_get_home_tile_icon(int index) {
    if (index < 0 || index >= plugin_home_tile_count) return "";
    return plugin_home_tiles[index].icon;
}

const char * plugin_manager_get_home_tile_icon_selected(int index) {
    if (index < 0 || index >= plugin_home_tile_count) return "";
    return plugin_home_tiles[index].icon_selected;
}

void plugin_manager_home_tile_clicked(int index) {
    if (index < 0 || index >= plugin_home_tile_count) return;
    dispatch_tile_open(&plugin_home_tiles[index]);
}

int plugin_manager_get_quick_toggle_count(void) {
    return plugin_quick_toggle_count;
}

int plugin_manager_find_quick_toggle_by_id(const char * id) {
    return plugin_find_quick_toggle_by_id(id);
}

const char * plugin_manager_get_quick_toggle_label(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return "";
    return plugin_quick_toggles[index].label;
}

const char * plugin_manager_get_quick_toggle_icon(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return "";
    return plugin_quick_toggles[index].icon;
}

const char * plugin_manager_get_quick_toggle_icon_selected(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return "";
    return plugin_quick_toggles[index].icon_selected;
}

const char * plugin_manager_get_quick_toggle_state_text(int index, bool on) {
    if (index < 0 || index >= plugin_quick_toggle_count) return on ? TR("On") : TR("Off");
    return on ? plugin_quick_toggles[index].on_text : plugin_quick_toggles[index].off_text;
}

bool plugin_manager_get_quick_toggle_value(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return false;
    return plugin_quick_toggles[index].value;
}

bool plugin_manager_quick_toggle_has_on_hold(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return false;
    const plugin_quick_toggle_t * t = &plugin_quick_toggles[index];
    return t->L != NULL && t->on_hold_ref != LUA_NOREF;
}

void plugin_manager_quick_toggle_set(int index, bool new_value) {
    if (index < 0 || index >= plugin_quick_toggle_count) return;
    plugin_quick_toggle_t * t = &plugin_quick_toggles[index];
    /* Stored before the call so a plugin that reads its own state back from
     * inside on_change (or calls plugin.set_quick_toggle() there) sees the
     * new value rather than the one it is replacing. */
    t->value = new_value;
    if (!t->L || t->change_ref == LUA_NOREF) return;
    if (!plugin_push_callback(t->L, t->change_ref)) return;
    lua_pushboolean(t->L, new_value);
    plugin_call_report(t->L, 1, false, "quick toggle '%s' on_change error", t->label);
}

bool plugin_manager_quick_toggle_open_settings(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return false;
    plugin_quick_toggle_t * t = &plugin_quick_toggles[index];
    if (!t->L || t->on_hold_ref == LUA_NOREF) return false;
    if (!plugin_push_callback(t->L, t->on_hold_ref)) return false;
    plugin_call_report(t->L, 0, false, "quick toggle '%s' on_hold error", t->label);
    return true;
}

const luaL_Reg plugin_registries_funcs[] = {
    { "register_list_item",        l_plugin_register_list_item },
    { "register_stream_media_tile", l_plugin_register_stream_media_tile },
    { "register_home_tile",        l_plugin_register_home_tile },
    { "register_quick_toggle",     l_plugin_register_quick_toggle },
    { "set_quick_toggle",          l_plugin_set_quick_toggle },
    { NULL, NULL }
};
