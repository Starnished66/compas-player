/* UI layout overrides, player actions and layouts, reload and theme refresh. */
#include "../../ui/application/gui.h"
#include "../../ui/shell/gui_reload.h"
#include "../../ui/player/layout/player_layouts.h"
#include "../lua/plugin_lua_runtime.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "plugin_manager_internal.h"
#define PLUGIN_MAX_PLAYER_ACTIONS 32
uint64_t ui_layout_sequence;

typedef struct {
    lua_State * L;
    int callback_ref;
    char owner[64];
    char action[PLAYER_LAYOUT_ACTION_MAX];
} plugin_player_action_t;

static plugin_player_action_t plugin_player_actions[PLUGIN_MAX_PLAYER_ACTIONS];

static void plugin_player_action_dispatch(const char * owner, const char * action) {
    if (!owner || !action) return;
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        if (!entry->L || strcmp(entry->owner, owner) != 0 || strcmp(entry->action, action) != 0) continue;
        plugin_instance_t * inst = plugin_instance_for_state(entry->L);
        if (!inst || !inst->defined || inst->disposing) return;
        if (inst->aborted) {
            plugin_player_actions_clear_owner(entry->L, inst->id, true);
            return;
        }
        lua_State * L = entry->L;
        char action_copy[PLAYER_LAYOUT_ACTION_MAX];
        snprintf(action_copy, sizeof(action_copy), "%s", entry->action);
        int top = lua_gettop(L);
        if (!plugin_push_callback(L, entry->callback_ref)) return;
        lua_pushstring(L, action_copy);
        plugin_call_report(L, 1, false, "player action '%s' callback error", action_copy);
        if (inst->aborted) plugin_player_actions_clear_owner(L, inst->id, true);
        else lua_settop(L, top);
        return;
    }
}

static bool plugin_player_action_name_valid(const char * action) {
    size_t len = action ? strlen(action) : 0;
    if (len == 0 || len >= PLAYER_LAYOUT_ACTION_MAX ||
        strcmp(action, "library") == 0 || strcmp(action, "none") == 0) return false;
    for (const unsigned char * p = (const unsigned char *) action; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')) return false;
    return true;
}

void plugin_player_actions_clear_owner(lua_State * L, const char * owner, bool aborted) {
    if (owner && owner[0]) player_layouts_clear_plugin_owner(owner);
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        if (entry->L != L) continue;
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, entry->callback_ref);
        memset(entry, 0, sizeof(*entry));
        entry->callback_ref = LUA_NOREF;
    }
}

void plugin_player_actions_clear_all(void) {
    player_layouts_set_action_handler(NULL);
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        plugin_unref(entry->L, entry->callback_ref);
        memset(entry, 0, sizeof(*entry));
        entry->callback_ref = LUA_NOREF;
    }
}

/* Registers or selects the XML layout requested by plugin.set_player_layout.
 * Select performs an upsert too, which clears per-entry owner metadata; save
 * the owner only after that final upsert succeeds, including when selection
 * leaves the already-effective ID unchanged. */
static bool plugin_set_player_layout_xml(lua_State * L, const char * id, const char * name,
                                         const char * path, bool loading, bool * layout_changed) {
    bool registered = player_layouts_session_register_xml(id, name, path, !loading);
    if (!registered) return false;
    bool changed = false;
    if (!loading) changed = player_layouts_session_select_xml(id, name, path, true);
    plugin_instance_t * owner_inst = plugin_instance_for_state(plugin_lua_main_state(L));
    if (owner_inst && owner_inst->defined && !owner_inst->disposing && !owner_inst->aborted)
        (void) player_layouts_set_plugin_owner(id, owner_inst->id);
    if (layout_changed) *layout_changed = changed;
    return true;
}

static int l_plugin_register_player_action(lua_State * L) {
    const char * action = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!plugin_player_action_name_valid(action))
        return luaL_error(L, "plugin.register_player_action: action must be 1-63 letters, digits, '_', '.' or '-', and not 'library' or 'none'");
    lua_State * main_L = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(main_L);
    if (!inst || !inst->defined || !inst->id[0])
        return luaL_error(L, "plugin.register_player_action requires plugin.define({id=...}) to run first");
    if (inst->disposing || inst->aborted)
        return luaL_error(L, "plugin.register_player_action: plugin is no longer active");

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        if (entry->L && strcmp(entry->owner, inst->id) == 0 && strcmp(entry->action, action) == 0) {
            slot = i;
            break;
        }
        if (!entry->L && slot < 0) slot = i;
    }
    if (slot < 0) return luaL_error(L, "plugin.register_player_action: action registry is full");

    plugin_player_action_t * entry = &plugin_player_actions[slot];
    if (entry->L && !inst->aborted)
        luaL_unref(main_L, LUA_REGISTRYINDEX, entry->callback_ref);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    memset(entry, 0, sizeof(*entry));
    entry->L = main_L;
    entry->callback_ref = ref;
    snprintf(entry->owner, sizeof(entry->owner), "%s", inst->id);
    snprintf(entry->action, sizeof(entry->action), "%s", action);
    player_layouts_set_action_handler(plugin_player_action_dispatch);
    return 0;
}

/* Structural declarations run during startup, before native screens exist.
 * Keep each owner's contribution so a failed/disabled plugin cannot leave
 * a restriction behind or erase another plugin's earlier declaration. */
static unsigned ui_order_mask(const ui_item_order_t *order) {
    unsigned mask = 0;
    for (unsigned i = 0; i < order->count && i < UI_ORDER_MAX; ++i)
        if (order->ids[i] < UI_ORDER_MAX) mask |= 1u << order->ids[i];
    return mask;
}

static void restrict_ui_order(ui_item_order_t *order, unsigned allowed) {
    unsigned count = 0;
    for (unsigned i = 0; i < order->count && i < UI_ORDER_MAX; ++i)
        if (order->ids[i] < UI_ORDER_MAX && (allowed & (1u << order->ids[i])))
            order->ids[count++] = order->ids[i];
    order->count = (uint8_t)count;
}

void refresh_plugin_ui_customization(void) {
    ui_customization_override_t merged = {0};
    uint64_t sequence[6] = {0};
    unsigned allowed[3] = {UINT_MAX, UINT_MAX, UINT_MAX};
    bool drawer_disabled = false, eq_hidden = false, volume_hidden = false;
    for (int i = 0; i < PLUGIN_MAX_FILES; ++i) {
        const plugin_instance_t *inst = &plugin_instances[i];
        if (!inst->L || inst->disposing || inst->aborted) continue;
        const ui_customization_override_t *o = &inst->ui_override;
        if (o->has_music) {
            allowed[0] &= ui_order_mask(&o->value.music);
            if (inst->music_layout_sequence > sequence[0]) {
                sequence[0] = inst->music_layout_sequence;
                merged.has_music = true;
                merged.value.music = o->value.music;
            }
        }
        if (o->has_music_toolbar) {
            allowed[1] &= ui_order_mask(&o->value.music_toolbar);
            if (inst->music_layout_sequence > sequence[1]) {
                sequence[1] = inst->music_layout_sequence;
                merged.has_music_toolbar = true;
                merged.value.music_toolbar = o->value.music_toolbar;
            }
        }
        if (o->has_drawer) {
            allowed[2] &= ui_order_mask(&o->value.drawer);
            if (inst->drawer_layout_sequence > sequence[2]) {
                sequence[2] = inst->drawer_layout_sequence;
                merged.has_drawer = true;
                merged.value.drawer = o->value.drawer;
            }
        }
        if (o->has_drawer_enabled) {
            drawer_disabled |= !o->value.drawer_enabled;
            if (inst->drawer_layout_sequence > sequence[3]) {
                sequence[3] = inst->drawer_layout_sequence;
                merged.has_drawer_enabled = true;
                merged.value.drawer_enabled = o->value.drawer_enabled;
            }
        }
        if (o->has_drawer_eq_visible) {
            eq_hidden |= !o->value.drawer_eq_visible;
            if (inst->drawer_layout_sequence > sequence[4]) {
                sequence[4] = inst->drawer_layout_sequence;
                merged.has_drawer_eq_visible = true;
                merged.value.drawer_eq_visible = o->value.drawer_eq_visible;
            }
        }
        if (o->has_drawer_volume_visible) {
            volume_hidden |= !o->drawer_volume_visible;
            if (inst->drawer_layout_sequence > sequence[5]) {
                sequence[5] = inst->drawer_layout_sequence;
                merged.has_drawer_volume_visible = true;
                merged.drawer_volume_visible = o->drawer_volume_visible;
            }
        }
    }
    restrict_ui_order(&merged.value.music, allowed[0]);
    restrict_ui_order(&merged.value.music_toolbar, allowed[1]);
    restrict_ui_order(&merged.value.drawer, allowed[2]);
    if (drawer_disabled) merged.value.drawer_enabled = false;
    if (eq_hidden) merged.value.drawer_eq_visible = false;
    if (volume_hidden) merged.drawer_volume_visible = false;
    ui_customization_set_override(&merged);
}

static plugin_instance_t *require_ui_layout_load(lua_State *L, const char *api) {
    lua_State *main = plugin_lua_main_state(L);
    plugin_instance_t *inst = plugin_instance_for_state(main);
    if (!inst || !inst->defined || inst->disposing || inst->aborted ||
        loading_plugin_slot < 0 || loading_plugin_slot >= PLUGIN_MAX_FILES ||
        &plugin_instances[loading_plugin_slot] != inst) {
        luaL_error(L, "%s must run after plugin.define during plugin startup; use plugin.reload_ui to apply changes", api);
        return NULL;
    }
    return inst;
}

static bool parse_ui_order_field(lua_State *L, const char *field, ui_order_kind_t kind,
                                 ui_item_order_t *out, const char *api) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return false; }
    if (!lua_istable(L, -1)) luaL_error(L, "%s: %s must be an array", api, field);
    lua_Unsigned count = lua_rawlen(L, -1);
    if (count > (lua_Unsigned)ui_order_size(kind))
        luaL_error(L, "%s: too many %s entries", api, field);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 ||
            (lua_Unsigned)lua_tointeger(L, -2) > count)
            luaL_error(L, "%s: %s must be a contiguous array", api, field);
        lua_pop(L, 1);
    }
    ui_item_order_t order = { .count = (uint8_t)count };
    unsigned seen = 0;
    for (lua_Unsigned i = 0; i < count; ++i) {
        lua_rawgeti(L, -1, (lua_Integer)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "%s: %s entries must be strings", api, field);
        size_t len;
        const char *key = lua_tolstring(L, -1, &len);
        int id = strlen(key) == len ? ui_order_find_key(kind, key) : -1;
        if (id < 0) luaL_error(L, "%s: unknown %s entry", api, field);
        if (seen & (1u << id)) luaL_error(L, "%s: duplicate %s entry", api, field);
        seen |= 1u << id;
        order.ids[i] = (uint8_t)id;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    *out = order;
    return true;
}

static bool parse_ui_bool_field(lua_State *L, const char *field, bool *out, const char *api) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return false; }
    if (!lua_isboolean(L, -1)) luaL_error(L, "%s: %s must be a boolean", api, field);
    *out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return true;
}

static int l_plugin_set_music_layout(lua_State *L) {
    const char *api = "plugin.set_music_layout";
    plugin_instance_t *inst = require_ui_layout_load(L, api);
    ui_customization_override_t parsed = {0};
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        parsed.has_music = parse_ui_order_field(L, "order", UI_ORDER_MUSIC, &parsed.value.music, api);
        parsed.has_music_toolbar = parse_ui_order_field(L, "toolbar", UI_ORDER_MUSIC_TOOLBAR,
                                                       &parsed.value.music_toolbar, api);
    }
    inst->ui_override.has_music = parsed.has_music;
    inst->ui_override.has_music_toolbar = parsed.has_music_toolbar;
    inst->ui_override.value.music = parsed.value.music;
    inst->ui_override.value.music_toolbar = parsed.value.music_toolbar;
    inst->music_layout_sequence = parsed.has_music || parsed.has_music_toolbar ? ++ui_layout_sequence : 0;
    refresh_plugin_ui_customization();
    return 0;
}

static int l_plugin_set_quick_drawer_layout(lua_State *L) {
    const char *api = "plugin.set_quick_drawer_layout";
    plugin_instance_t *inst = require_ui_layout_load(L, api);
    ui_customization_override_t parsed = {0};
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        parsed.has_drawer = parse_ui_order_field(L, "order", UI_ORDER_DRAWER, &parsed.value.drawer, api);
        parsed.has_drawer_enabled = parse_ui_bool_field(L, "enabled", &parsed.value.drawer_enabled, api);
        parsed.has_drawer_eq_visible = parse_ui_bool_field(L, "eq_visible", &parsed.value.drawer_eq_visible, api);
        parsed.has_drawer_volume_visible = parse_ui_bool_field(L, "volume_visible", &parsed.drawer_volume_visible, api);
    }
    inst->ui_override.has_drawer = parsed.has_drawer;
    inst->ui_override.has_drawer_enabled = parsed.has_drawer_enabled;
    inst->ui_override.has_drawer_eq_visible = parsed.has_drawer_eq_visible;
    inst->ui_override.has_drawer_volume_visible = parsed.has_drawer_volume_visible;
    inst->ui_override.value.drawer = parsed.value.drawer;
    inst->ui_override.value.drawer_enabled = parsed.value.drawer_enabled;
    inst->ui_override.value.drawer_eq_visible = parsed.value.drawer_eq_visible;
    inst->ui_override.drawer_volume_visible = parsed.drawer_volume_visible;
    inst->drawer_layout_sequence = parsed.has_drawer || parsed.has_drawer_enabled ||
        parsed.has_drawer_eq_visible || parsed.has_drawer_volume_visible ? ++ui_layout_sequence : 0;
    refresh_plugin_ui_customization();
    return 0;
}

static void parse_launcher_menu_layout(lua_State * L, int idx, const char * menu,
                                       launcher_menu_layout_t * out) {
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, "mode");
    const char * mode = luaL_optstring(L, -1, "tile");
    if (strcmp(mode, "tile") != 0 && strcmp(mode, "list") != 0)
        luaL_error(L, "plugin.set_launcher_layout: %s.mode must be \"tile\" or \"list\"", menu);
    lua_pop(L, 1);

    out->row_gap = check_int32_field(L, plugin_lua_opt_integer_field(L, idx, "row_gap", 0),
                                     "plugin.set_launcher_layout", "row_gap");
    out->height = check_int32_field(L, plugin_lua_opt_integer_field(L, idx, "height", 0),
                                    "plugin.set_launcher_layout", "height");
    out->width = check_int32_field(L, plugin_lua_opt_integer_field(L, idx, "width", 0),
                                   "plugin.set_launcher_layout", "width");

    get_opt_color_field(L, idx, "bg_color", &out->has_bg_color, &out->bg_color);
    get_opt_color_field(L, idx, "text_color", &out->has_text_color, &out->text_color);
    lua_getfield(L, idx, "radius");
    if (!lua_isnil(L, -1)) {
        lua_Integer radius = luaL_checkinteger(L, -1);
        if (radius < 0) luaL_error(L, "plugin.set_launcher_layout: %s.radius cannot be negative", menu);
        out->has_radius = true;
        out->radius = check_int32_field(L, radius, "plugin.set_launcher_layout", "radius");
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "align");
    const char * align = lua_tostring(L, -1);
    if (align) {
        if (strcmp(align, "left") != 0 && strcmp(align, "center") != 0 && strcmp(align, "right") != 0)
            luaL_error(L, "plugin.set_launcher_layout: %s.align must be left, center, or right", menu);
        snprintf(out->align, sizeof(out->align), "%s", align);
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "text_size");
    const char * text_size = lua_tostring(L, -1);
    if (text_size) {
        if (!is_valid_text_size(text_size))
            luaL_error(L, "plugin.set_launcher_layout: %s.text_size is invalid", menu);
        snprintf(out->text_size, sizeof(out->text_size), "%s", text_size);
    }
    lua_pop(L, 1);
    get_opt_bool_field(L, idx, "accessory", &out->has_accessory, &out->accessory);
    get_opt_bool_field(L, idx, "icon", &out->has_icon, &out->icon);
}

static int l_plugin_set_launcher_layout(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    launcher_layout_config_t config;
    memset(&config, 0, sizeof(config));
    const char * names[] = { "music", "stream_media", "wireless" };
    launcher_menu_layout_t * menus[] = { &config.music, &config.stream_media, &config.wireless };
    for (int i = 0; i < 3; i++) {
        lua_getfield(L, 1, names[i]);
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1))
                return luaL_error(L, "plugin.set_launcher_layout: %s must be a table", names[i]);
            parse_launcher_menu_layout(L, -1, names[i], menus[i]);
        }
        lua_pop(L, 1);
    }
    gui_plugin_set_launcher_layout(&config);
    return 0;
}

static int l_plugin_set_player_layout(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    player_layout_config_t config;
    memset(&config, 0, sizeof(config));
    config.configured = true;

    lua_getfield(L, 1, "flat");
    if (!lua_isnil(L, -1)) {
        config.flat = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    get_opt_color_field(L, 1, "bg_color", &config.has_bg_color, &config.bg_color);

    lua_getfield(L, 1, "blur_radius");
    if (!lua_isnil(L, -1)) {
        lua_Integer val = luaL_checkinteger(L, -1);
        config.has_blur_radius = true;
        config.blur_radius = check_int32_field(L, val, "plugin.set_player_layout", "blur_radius");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "blur_passes");
    if (!lua_isnil(L, -1)) {
        lua_Integer val = luaL_checkinteger(L, -1);
        config.has_blur_passes = true;
        config.blur_passes = check_int32_field(L, val, "plugin.set_player_layout", "blur_passes");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "darken_num");
    bool has_num = !lua_isnil(L, -1);
    lua_getfield(L, 1, "darken_den");
    bool has_den = !lua_isnil(L, -1);

    if (has_num != has_den) {
        lua_pop(L, 2);
        return luaL_error(L, "plugin.set_player_layout: darken_num and darken_den must both be set together");
    }
    if (has_num && has_den) {
        lua_Integer num = luaL_checkinteger(L, -2);
        lua_Integer den = luaL_checkinteger(L, -1);
        config.has_darken = true;
        config.darken_num = check_int32_field(L, num, "plugin.set_player_layout", "darken_num");
        config.darken_den = check_int32_field(L, den, "plugin.set_player_layout", "darken_den");
    }
    lua_pop(L, 2);

    /* Optional XML layout for the Player screen, registered for this session only. */
    bool layout_changed = false;
    lua_getfield(L, 1, "xml");
    if (!lua_isnil(L, -1)) {
        const char * xml = luaL_checkstring(L, -1);
        lua_getfield(L, 1, "id");
        const char * id_arg = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_getfield(L, 1, "name");
        const char * name_arg = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);

        char plugin_dir[PATH_MAX], plugin_dir_real[PATH_MAX], full[PATH_MAX], full_real[PATH_MAX];
        snprintf(plugin_dir, sizeof(plugin_dir), "%s/.plugins", MUSIC_ROOT_DIR);
        /* Relative to the plugins folder (<SD card>/.plugins, where plugin
         * .lua files live), and never outside it: no absolute paths, no ".." segment, and the
         * resolved file (symlinks included) must stay under that folder. */
        bool has_dotdot = false;
        for (const char * p = xml; *p; p++) {
            if (p[0] == '.' && p[1] == '.' && (p == xml || p[-1] == '/') && (p[2] == '/' || p[2] == '\0'))
                has_dotdot = true;
        }
        if (!xml[0] || xml[0] == '/' || strchr(xml, '\\') || has_dotdot ||
            snprintf(full, sizeof(full), "%s/%s", plugin_dir, xml) >= (int) sizeof(full))
            return luaL_error(L, "plugin.set_player_layout: xml must be a relative path inside the plugins folder");
        if (!realpath(plugin_dir, plugin_dir_real) || !realpath(full, full_real))
            return luaL_error(L, "plugin.set_player_layout: xml file '%s' not found", xml);
        size_t dir_len = strlen(plugin_dir_real);
        if (strncmp(full_real, plugin_dir_real, dir_len) != 0 || full_real[dir_len] != '/')
            return luaL_error(L, "plugin.set_player_layout: xml must stay inside the plugins folder");

        /* Default id/name come from the file name: "a/Clean Look.xml" is id "plugin.Clean_Look". */
        const char * base = strrchr(xml, '/');
        base = base ? base + 1 : xml;
        char stem[PLAYER_LAYOUT_ID_MAX];
        snprintf(stem, sizeof(stem), "%s", base);
        char * dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        char id_buf[PLAYER_LAYOUT_ID_MAX];
        if (id_arg) {
            snprintf(id_buf, sizeof(id_buf), "%s", id_arg);
        } else {
            char clean[PLAYER_LAYOUT_ID_MAX - 8];
            size_t n = 0;
            for (const char * p = stem; *p && n + 1 < sizeof(clean); p++)
                clean[n++] = (isalnum((unsigned char) *p) || *p == '_' || *p == '-' || *p == '.') ? *p : '_';
            clean[n] = '\0';
            snprintf(id_buf, sizeof(id_buf), "plugin.%s", clean);
        }
        if (!player_layouts_id_is_valid(id_buf))
            return luaL_error(L, "plugin.set_player_layout: id must be 1-63 letters, digits, '_', '-' or '.', and not 'default'");
        /* While the plugin loads the layout is only offered in Settings: the
         * user's pick (the saved id) wins, and the plugin registers it again
         * on every load. A callback selects it for the session right away. */
        (void) plugin_set_player_layout_xml(L, id_buf, name_arg ? name_arg : stem, full_real,
                                            loading_plugin_slot >= 0, &layout_changed);
        lua_pop(L, 2);
    }
    lua_pop(L, 1);

    gui_plugin_set_player_layout(&config);
    gui_player_refresh_frosted_background();
    /* The player screen only exists once the UI is up. Plugins run before it
     * is built on boot and inside a reload, where the new layout is picked up
     * anyway; a call from a callback needs the rebuild. */
    if (layout_changed && gui_player_get_screen()) gui_player_layout_reload_request();
    return 0;
}

static int l_plugin_restore_player_layout(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(plugin_lua_main_state(L));
    if (!inst || !inst->defined || !inst->id[0])
        return luaL_error(L, "plugin.restore_player_layout requires plugin.define({id=...}) to run first");
    if (inst->disposing || inst->aborted)
        return luaL_error(L, "plugin.restore_player_layout: plugin is no longer active");
    bool changed = player_layouts_session_clear_plugin_selection(inst->id);
    if (changed && gui_player_get_screen()) gui_player_layout_reload_request();
    lua_pushboolean(L, changed);
    return 1;
}

/* plugin.reload_ui() -- rebuilds every screen/style in the same process
 * (see gui_reload.h/.c) so a plugin.set_icon()/set_background_color()/
 * set_text_color()/set_home_layout() call takes full effect without
 * needing the player killed and relaunched. Never touches audio playback
 * or any network/Bluetooth/D-Bus connection -- see gui_reload.c's own top
 * comment for the exact scope.
 *
 * Deliberately does NOT call gui_soft_reload() directly: this binding runs
 * via plugin_call() on the calling plugin's own lua_State, still live on
 * the C stack inside lua_pcall() -- gui_soft_reload() closes every plugin's
 * L, including this one, which would be undefined behavior if done while
 * that same L is still executing. gui_reload_request() only schedules the
 * real reload for the next main-loop pass, strictly after this call (and
 * the whole plugin_call() it's part of) has returned. */
static int l_plugin_reload_ui(lua_State * L) {
    (void) L;
    gui_reload_request();
    return 0;
}

static int l_plugin_refresh_theme(lua_State * L) {
    (void) L;
    gui_theme_refresh_request();
    return 0;
}

const luaL_Reg plugin_layout_funcs[] = {
    { "set_music_layout",          l_plugin_set_music_layout },
    { "set_quick_drawer_layout",   l_plugin_set_quick_drawer_layout },
    { "set_launcher_layout",       l_plugin_set_launcher_layout },
    { "set_player_layout",         l_plugin_set_player_layout },
    { "restore_player_layout",     l_plugin_restore_player_layout },
    { "register_player_action",    l_plugin_register_player_action },
    { "refresh_theme",             l_plugin_refresh_theme },
    { "reload_ui",                 l_plugin_reload_ui },
    { NULL, NULL }
};
