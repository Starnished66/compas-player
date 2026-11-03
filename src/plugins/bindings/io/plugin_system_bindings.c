/* plugin.* bindings for LEDs, button lock, storage/secrets, HTTP, md5, HTML, text view, album covers and library refresh. */
#include "../../../ui/application/gui.h"
#include "../../../hardware/indicators/led_control.h"
#include "../../../network/http/client/http_client.h"
#include "../ui/plugin_text_view_bindings.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../storage/plugin_storage.h"
#include "../../manager/plugin_internal.h"
#include "../../../hardware/input/hw_buttons.h"
#include "mbedtls/md5.h" /* plugin.md5() -- same primitive subsonic_client.c already uses for its own token auth */
#include "../../../core/ui/web/html_blocks.h"
#include <string.h>
#include <stdlib.h>
#include "../../manager/plugin_manager_internal.h"

// How many album cover requests to queue, before rejecting
#define PLUGIN_ALBUM_COVER_QUEUE 32
/* Lookups answered per player tick. Each one is a database query plus a
 * cache-file check on the UI thread, so a full queue is spread over ticks. */
#define PLUGIN_ALBUM_COVER_PER_TICK 4
lua_State * plugin_led_override_owner = NULL;

/* Returns charge-status control to the current user setting only when the
 * plugin whose main Lua state is L owns the shared override. L must already
 * be a main state: the error and failed-load paths pass one directly and
 * never touch a possibly hard-aborted state's registry. */
void release_plugin_led_override(lua_State * L) {
    if (plugin_led_override_owner != L) return;
    plugin_led_override_owner = NULL;
    led_control_clear_override();
    led_control_apply(current_settings.led_indicator_enabled);
}

/* Shared by every plugin.storage and plugin.secrets binding below --
 * requires plugin.define({id=...}) to have already run on this lua_State,
 * matching plugin_storage.c's own expectation that plugin_id is a real,
 * validated identity, not an empty/default string. */
static const char * require_plugin_id(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!inst || !inst->defined || !inst->id[0]) {
        luaL_error(L, "plugin.storage/secrets requires plugin.define({id=...}) to run first");
        return NULL; /* unreachable -- luaL_error() longjmps */
    }
    return inst->id;
}

/* plugin.http_get(url [, verify_tls]) -> status, body | nil, "network error"
 * -- bridges http_client.c's http_get_to_buffer() directly (no gui.c layer,
 * same reasoning as plugin.eq_*() (plugin_playback_bindings.c): no LVGL/gui-state dependency).
 * body is pushed with lua_pushlstring() (exact byte count), not
 * lua_pushstring(), since a response body isn't guaranteed NUL-free. Runs
 * synchronously on the calling (main UI) thread -- see this file's own
 * compas-plugins/PLUGINS.md for the blocking caveat. */
static int l_plugin_http_get(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    bool verify_tls = lua_gettop(L) >= 2 ? lua_toboolean(L, 2) : true;

    int status = 0;
    uint8_t * body = NULL;
    size_t body_size = 0;
    bool ok = http_get_to_buffer(url, verify_tls, &status, &body, &body_size);
    if (!ok) {
        return plugin_lua_push_error(L, "network error");
    }

    lua_pushinteger(L, status);
    lua_pushlstring(L, (const char *) body, body_size);
    free(body);
    return 2;
}

/* plugin.http_post(url, body [, content_type] [, verify_tls]) -> status,
 * body | nil, "network error" -- same contract/shape as l_plugin_http_get()
 * above, bridging http_client.c's new http_post_to_buffer(). body is read
 * with luaL_checklstring() (not luaL_checkstring()) since a POST body isn't
 * guaranteed NUL-free either (e.g. a binary payload), same reasoning as the
 * response body's own lua_pushlstring() below. content_type defaults to
 * "application/x-www-form-urlencoded" (what most simple API POSTs,
 * including Last.fm's, expect) when omitted or nil. */
static int l_plugin_http_post(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char * body = luaL_checklstring(L, 2, &body_len);
    const char * content_type =
        (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? luaL_checkstring(L, 3) : "application/x-www-form-urlencoded";
    bool verify_tls = lua_gettop(L) >= 4 ? lua_toboolean(L, 4) : true;

    int status = 0;
    uint8_t * resp_body = NULL;
    size_t resp_body_size = 0;
    bool ok = http_post_to_buffer(url, verify_tls, content_type, (const uint8_t *) body, body_len, &status,
                                   &resp_body, &resp_body_size);
    if (!ok) {
        return plugin_lua_push_error(L, "network error");
    }

    lua_pushinteger(L, status);
    lua_pushlstring(L, (const char *) resp_body, resp_body_size);
    free(resp_body);
    return 2;
}

/* plugin.md5(text) -> lowercase hex string -- thin wrapper around
 * mbedtls_md5(), already vendored and already used exactly this way in
 * subsonic_client.c's own token-auth code, so no new dependency. Needed for
 * Last.fm's api_sig signing scheme (md5(sorted params + shared secret)),
 * and generically useful beyond that. */
static int l_plugin_md5(lua_State * L) {
    size_t len = 0;
    const char * data = luaL_checklstring(L, 1, &len);

    unsigned char digest[16];
    mbedtls_md5((const unsigned char *) data, len, digest);

    char hex[33];
    md5_digest_to_hex(digest, hex);
    lua_pushstring(L, hex);
    return 1;
}

typedef struct {
    lua_State * L;
    int callback_ref;
    int64_t song_id;
    int size_px;
} plugin_album_cover_req_t;

static plugin_album_cover_req_t plugin_album_cover_queue[PLUGIN_ALBUM_COVER_QUEUE];

static void plugin_album_cover_drop(plugin_album_cover_req_t * req) {
    plugin_unref(req->L, req->callback_ref);
    req->L = NULL;
    req->callback_ref = LUA_NOREF;
}

void plugin_album_cover_drop_owner(lua_State * owner, bool aborted) {
    for (int i = 0; i < PLUGIN_ALBUM_COVER_QUEUE; i++) {
        plugin_album_cover_req_t * req = &plugin_album_cover_queue[i];
        if (req->L && req->L == owner) plugin_album_cover_drop(req);
    }
}

void plugin_album_cover_cancel_all(void) {
    for (int i = 0; i < PLUGIN_ALBUM_COVER_QUEUE; i++) {
        plugin_album_cover_req_t * req = &plugin_album_cover_queue[i];
        if (!req->L) continue;
        plugin_album_cover_drop(req);
    }
}

void plugin_album_cover_poll(void) {
    /* Rotates the starting slot so later requests are not starved. */
    static int next_slot;
    int answered = 0;
    for (int n = 0; n < PLUGIN_ALBUM_COVER_QUEUE && answered < PLUGIN_ALBUM_COVER_PER_TICK; n++) {
        int i = (next_slot + n) % PLUGIN_ALBUM_COVER_QUEUE;
        plugin_album_cover_req_t * req = &plugin_album_cover_queue[i];
        if (!req->L) continue;
        answered++;
        next_slot = (i + 1) % PLUGIN_ALBUM_COVER_QUEUE;
        lua_State * L = req->L;
        int ref = req->callback_ref;
        int64_t song_id = req->song_id;
        int size_px = req->size_px;
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
        if (plugin_state_frozen(L)) continue;
        char path[PATH_MAX];
        bool found = gui_plugin_library_album_cover(song_id, size_px, path, sizeof(path));
        if (!plugin_push_callback(L, ref)) continue;
        if (found) {
            lua_pushstring(L, path);
            lua_pushnil(L);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "not_generated");
        }
        plugin_unref(L, ref);
        plugin_call_report(L, 2, false, "library_album_cover_async callback error");
    }
}

static int l_plugin_library_album_cover_async(lua_State * L) {
    int64_t song_id = (int64_t) luaL_checkinteger(L, 1);
    int size_px = (int) luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    /* 72 was the thumbnail size of earlier releases; it stays accepted and
     * serves the current thumbnail (80, or 40 on the TempoTec V1). */
    if (size_px == 72) size_px = ALBUMART_THUMBNAIL_SIZE;
    if (size_px != ALBUMART_THUMBNAIL_SIZE && size_px != ALBUMART_PLAYER_CACHE_SIZE)
        return luaL_error(L, "plugin.library_album_cover_async: size must be %d (or the legacy 72) or %d",
                          ALBUMART_THUMBNAIL_SIZE, ALBUMART_PLAYER_CACHE_SIZE);
    plugin_album_cover_req_t * slot = NULL;
    for (int i = 0; i < PLUGIN_ALBUM_COVER_QUEUE && !slot; i++)
        if (!plugin_album_cover_queue[i].L) slot = &plugin_album_cover_queue[i];
    if (!slot) {
        return plugin_lua_push_error(L, "busy");
    }
    lua_pushvalue(L, 3);
    slot->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    slot->L = plugin_lua_main_state(L);
    slot->song_id = song_id;
    slot->size_px = size_px;
    lua_pushboolean(L, 1);
    return 1;
}

/* plugin.refresh_library() -- triggers the same background rescan as
 * Settings > Update Music Database, so a plugin that wrote new files under
 * plugin.sd_root() can make them show up in library_*() without the user
 * finding that menu item themselves. No-op if a rescan is already running. */
static int l_plugin_refresh_library(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!inst || !inst->defined) return luaL_error(L, "plugin.refresh_library requires plugin.define first");
    time_t now = time(NULL);
    if (inst->last_library_refresh > 0 && now >= inst->last_library_refresh && now - inst->last_library_refresh < 60) {
        lua_pushboolean(L, false); lua_pushliteral(L, "rate_limited"); return 2;
    }
    if (!gui_plugin_refresh_library()) {
        lua_pushboolean(L, false); lua_pushliteral(L, "already_running"); return 2;
    }
    inst->last_library_refresh = now;
    lua_pushboolean(L, true); lua_pushliteral(L, "started"); return 2;
}

lua_State * button_lock_owner = NULL;

/* Native recognition remains available independently of Lua callbacks. */
static int l_plugin_configure_button_lock(lua_State * L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(owner);
    if (!inst || !inst->defined || inst->disposing || inst->aborted)
        return luaL_error(L, "plugin.configure_button_lock: plugin is not active");
    bool enabled = lua_toboolean(L, 1);
    if (enabled && button_lock_owner && button_lock_owner != owner)
        return luaL_error(L, "plugin.configure_button_lock: another plugin owns button lock");
    if (enabled || button_lock_owner == owner) {
        button_lock_owner = enabled ? owner : NULL;
        hw_buttons_set_button_lock_enabled(enabled);
    }
    lua_pushliteral(L, HW_BUTTONS_LOCK_CHORD_NAME);
    return 1;
}


static led_control_color_t check_led_color(lua_State * L, const char * api) {
    const char * color = luaL_checkstring(L, 1);
    if (strcmp(color, "red") == 0) return LED_CONTROL_RED;
    if (strcmp(color, "blue") == 0) return LED_CONTROL_BLUE;
    luaL_error(L, "%s: color must be 'red' or 'blue'", api);
    return LED_CONTROL_RED; /* unreachable -- luaL_error() longjmps */
}

static int check_led_level(lua_State * L, int arg, int default_level) {
    if (lua_gettop(L) < arg || lua_isnil(L, arg)) return default_level;
    lua_Integer level = luaL_checkinteger(L, arg);
    if (level < 0) return 0;
    if (level > 100) return 100;
    return (int) level;
}

static int check_led_interval(lua_State * L, int arg) {
    lua_Integer value = luaL_checkinteger(L, arg);
    if (value < 50) return 50;
    if (value > 60000) return 60000;
    return (int) value;
}

static int l_plugin_led_available(lua_State * L) {
    lua_pushboolean(L, led_control_available());
    return 1;
}

static int l_plugin_led_set(lua_State * L) {
    led_control_color_t color = check_led_color(L, "plugin.led_set");
    int level;
    if (lua_type(L, 2) == LUA_TBOOLEAN) {
        level = lua_toboolean(L, 2) ? 100 : 0;
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        level = check_led_level(L, 2, 0);
    } else {
        return luaL_error(L, "plugin.led_set: second argument must be a boolean or integer brightness 0..100");
    }
    if (!led_control_available())
        return luaL_error(L, "plugin.led_set: charge-status LEDs are not available on this board");

    plugin_led_override_owner = plugin_lua_main_state(L);
    led_control_set_override(color, level);
    return 0;
}

static int l_plugin_led_blink(lua_State * L) {
    led_control_color_t color = check_led_color(L, "plugin.led_blink");
    int on_ms = check_led_interval(L, 2);
    int off_ms = check_led_interval(L, 3);
    int level = check_led_level(L, 4, 100);
    if (!led_control_available())
        return luaL_error(L, "plugin.led_blink: charge-status LEDs are not available on this board");

    plugin_led_override_owner = plugin_lua_main_state(L);
    if (!led_control_blink(color, on_ms, off_ms, level))
        return luaL_error(L, "plugin.led_blink: could not create an LVGL effect timer");
    return 0;
}

static int l_plugin_led_breathe(lua_State * L) {
    led_control_color_t color = check_led_color(L, "plugin.led_breathe");
    lua_Integer input = luaL_checkinteger(L, 2);
    if (input < 1)
        return luaL_error(L, "plugin.led_breathe: period or breaths per minute must be positive");
    int period_ms;
    if (input <= 600) {
        int bpm = (int) input;
        period_ms = (60000 + bpm / 2) / bpm;
    } else {
        period_ms = input > 120000 ? 120000 : (int) input;
    }
    int level = check_led_level(L, 3, 100);
    if (!led_control_available())
        return luaL_error(L, "plugin.led_breathe: charge-status LEDs are not available on this board");

    plugin_led_override_owner = plugin_lua_main_state(L);
    if (!led_control_breathe(color, period_ms, level))
        return luaL_error(L, "plugin.led_breathe: could not create an LVGL effect timer");
    return 0;
}

static int l_plugin_led_get(lua_State * L) {
    led_control_color_t color = check_led_color(L, "plugin.led_get");
    static const char * const modes[] = { "off", "on", "blink", "breathe", "status" };
    led_control_mode_t mode = led_control_get_mode(color);
    int level = led_control_get_level(color);
    if (mode < LED_CONTROL_MODE_OFF || mode > LED_CONTROL_MODE_STATUS) mode = LED_CONTROL_MODE_OFF;
    lua_newtable(L);
    lua_pushstring(L, modes[mode]); lua_setfield(L, -2, "mode");
    lua_pushinteger(L, level); lua_setfield(L, -2, "level");
    return 1;
}

static int l_plugin_led_status(lua_State * L) {
    bool both = lua_gettop(L) == 0 || lua_isnil(L, 1);
    led_control_color_t color = LED_CONTROL_RED;
    if (!both) color = check_led_color(L, "plugin.led_status");
    if (!led_control_available())
        return luaL_error(L, "plugin.led_status: charge-status LEDs are not available on this board");

    plugin_led_override_owner = plugin_lua_main_state(L);
    if (both) led_control_set_all_status();
    else led_control_set_status(color);
    return 0;
}

static int l_plugin_led_release(lua_State * L) {
    release_plugin_led_override(plugin_lua_main_state(L));
    return 0;
}

/* plugin.storage.get(key [, default]) / .set(key, value) / .delete(key) /
 * .list(prefix) -- see plugin_storage.h's own threat-model comment.
 * Values round-trip arbitrary bytes (luaL_checklstring/lua_pushlstring),
 * not just NUL-terminated text. */
static int l_plugin_storage_get(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    char * value = NULL;
    size_t value_len = 0;
    if (plugin_storage_get(id, key, &value, &value_len)) {
        lua_pushlstring(L, value, value_len);
        free(value);
        return 1;
    }
    if (lua_gettop(L) >= 2) {
        lua_pushvalue(L, 2);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int l_plugin_storage_set(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    size_t value_len = 0;
    const char * value = luaL_checklstring(L, 2, &value_len);
    if (!plugin_storage_set(id, key, value, value_len)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "plugin.storage.set failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_storage_delete(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    lua_pushboolean(L, plugin_storage_delete(id, key));
    return 1;
}

static int l_plugin_storage_list(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * prefix = luaL_optstring(L, 1, "");
    char ** keys = NULL;
    int count = plugin_storage_list(id, prefix, &keys);
    if (count < 0) {
        return plugin_lua_push_error(L, "plugin.storage.list failed");
    }
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, keys[i]);
        lua_rawseti(L, -2, i + 1);
        free(keys[i]);
    }
    free(keys);
    return 1;
}

/* plugin.secrets.get/set/exists/delete -- deliberately no .list(), so a
 * plugin (or anything reading its own diagnostics) can't enumerate secret
 * key names; see plugin_storage.h's threat-model comment. get() reads only
 * the calling plugin's own namespace, same as plugin.storage.get(). */
static int l_plugin_secrets_get(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    char * value = NULL;
    size_t value_len = 0;
    if (plugin_secrets_get(id, key, &value, &value_len)) {
        lua_pushlstring(L, value, value_len);
        free(value);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int l_plugin_secrets_set(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    size_t value_len = 0;
    const char * value = luaL_checklstring(L, 2, &value_len);
    lua_pushboolean(L, plugin_secrets_set(id, key, value, value_len));
    return 1;
}

static int l_plugin_secrets_exists(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    lua_pushboolean(L, plugin_secrets_exists(id, key));
    return 1;
}

static int l_plugin_secrets_delete(lua_State * L) {
    const char * id = require_plugin_id(L);
    const char * key = luaL_checkstring(L, 1);
    lua_pushboolean(L, plugin_secrets_delete(id, key));
    return 1;
}


static void html_blocks_cleanup(void * mem) {
    html_blocks_free(mem);
}

static int html_blocks_push_lua(lua_State * L) {
    html_blocks * result = lua_touserdata(L, 1);
    lua_createtable(L, (int) result->count, 1);
    for (size_t i = 0; i < result->count; i++) {
        const html_block * b = &result->blocks[i];
        lua_createtable(L, 0, b->alt ? 4 : 3);

        lua_pushstring(L, b->kind);
        lua_setfield(L, -2, "kind");

        lua_pushinteger(L, b->level);
        lua_setfield(L, -2, "level");

        lua_pushstring(L, b->text ? b->text : "");
        lua_setfield(L, -2, "text");

        if (b->alt != NULL) {
            lua_pushstring(L, b->alt);
            lua_setfield(L, -2, "alt");
        }

        lua_rawseti(L, -2, (int) (i + 1));
    }

    lua_pushboolean(L, result->truncated);
    lua_setfield(L, -2, "truncated");
    return 1;
}

static int l_plugin_html_to_blocks(lua_State * L) {
    size_t len = 0;
    const char * bytes = luaL_checklstring(L, 1, &len);

    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (!lua_istable(L, 2)) {
            return luaL_error(L, "plugin.html_to_blocks: options must be a table");
        }
    }

    if (!lua_checkstack(L, 2)) return luaL_error(L, "stack overflow");
    lua_pushcfunction(L, html_blocks_push_lua);

    html_blocks result;
    memset(&result, 0, sizeof(result));
    html_status st = html_to_blocks(bytes, len, &result);
    if (st != HTML_OK) {
        lua_pop(L, 1);
        html_blocks_free(&result);
        lua_pushnil(L);
        lua_pushstring(L, html_status_reason(st));
        return 2;
    }

    return plugin_lua_pcall_then_cleanup(L, &result, 1, html_blocks_cleanup, &result);
}

/* This wrapper remains in the guarded plugin API table. The binding module
 * owns its Lua refs and GUI callback contexts; policy stays here. */

static int l_plugin_show_text_view(lua_State * L) {
    return plugin_text_view_bindings_show(L, plugin_call, plugin_state_frozen);
}

const luaL_Reg plugin_storage_funcs[] = {
    { "get",    l_plugin_storage_get },
    { "set",    l_plugin_storage_set },
    { "delete", l_plugin_storage_delete },
    { "list",   l_plugin_storage_list },
    { NULL, NULL }
};

const luaL_Reg plugin_secrets_funcs[] = {
    { "get",    l_plugin_secrets_get },
    { "set",    l_plugin_secrets_set },
    { "exists", l_plugin_secrets_exists },
    { "delete", l_plugin_secrets_delete },
    { NULL, NULL }
};

const luaL_Reg plugin_system_funcs[] = {
    { "led_available",             l_plugin_led_available },
    { "led_set",                   l_plugin_led_set },
    { "led_blink",                 l_plugin_led_blink },
    { "led_breathe",               l_plugin_led_breathe },
    { "led_get",                   l_plugin_led_get },
    { "led_status",                l_plugin_led_status },
    { "led_release",               l_plugin_led_release },
    { "http_get",                  l_plugin_http_get },
    { "http_post",                 l_plugin_http_post },
    { "md5",                       l_plugin_md5 },
    { "library_album_cover_async", l_plugin_library_album_cover_async },
    { "refresh_library",           l_plugin_refresh_library },
    { "configure_button_lock",     l_plugin_configure_button_lock },
    { "html_to_blocks",            l_plugin_html_to_blocks },
    { "show_text_view",            l_plugin_show_text_view },
    { NULL, NULL }
};
