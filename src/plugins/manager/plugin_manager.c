#include "plugin_manager.h"
#include "../../ui/text/i18n/i18n.h"
#include "../../ui/application/gui.h"
#include "../../ui/player/screen/gui_player.h"
#include "../../ui/player/screen/gui_lyrics.h"
#include "../../ui/shell/gui_shell.h"
#include "../../ui/shell/gui_reload.h"
#include "../../ui/player/layout/player_layouts.h"
#include "../../ui/settings/lock/gui_lock_screen.h"
#include "../../ui/plugins/plugin_control_dashboard.h"
#include "../../audio/effects/peq.h"
#include "../../audio/effects/mseb.h"
#include "../../hardware/indicators/led_control.h"
#include "../../hardware/power/battery.h"
#include "../../hardware/power/charge_limiter.h"
#include "../../audio/engine/audio.h"
#include "../../audio/output/audio_output.h"
#include "../../core/persistence/settings.h"
#include "../../core/themes/ui_customization.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../../hardware/usb/usb_mode_control.h"
#include "../../library/browser/files/file_ops.h"
#include "../../hardware/device/headphone_status.h"
#include "../../network/http/client/http_client.h"
#include "../../library/playlists/playlist_files.h"
#include "../json/plugin_json.h"
#include "../bindings/ui/plugin_info_bindings.h"
#include "../bindings/library/plugin_library_bindings.h"
#include "../bindings/library/plugin_library_bindings_internal.h"
#include "../bindings/audio/plugin_equalizer_bindings.h"
#include "../bindings/io/plugin_http_bindings.h"
#include "../bindings/ui/plugin_text_view_bindings.h"
#include "../bindings/io/plugin_archive_bindings.h"
#include "../bindings/io/plugin_apps_bindings.h"
#include "../storage/plugin_reserved_dirs.h"
#include "../lua/plugin_lua_runtime.h"
#include "../storage/plugin_storage.h"
#include "plugin_internal.h"
#include "../lifecycle/plugin_disabled_list.h"
#include "../../network/store/plugins/plugin_store.h"
#include "../../hardware/input/hw_buttons.h"
#include "../../ui/appearance/gui_themes.h"
#include "../../core/system/lifecycle/screenshot.h"
#include "../../core/system/logging/db_log.h"
#include "../../core/system/runtime/app_version.h"
#include "../../ui/text/rendering/fallback_font.h"
#include "mbedtls/md5.h" /* plugin.md5() -- same primitive subsonic_client.c already uses for its own token auth */
#include "../../core/text/zip_reader.h"
#include "../../core/ui/web/html_blocks.h"
#include "../../library/artwork/cache/image_thumb.h"
#include "../../library/metadata/db/metadata_db.h"
#include "../../core/ui/display/startup_splash.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <ctype.h>
#include <dirent.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>
#include <setjmp.h>

extern player_settings_t current_settings;

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- see gui.c's own MUSIC_ROOT_DIR comment; each
   * file that needs it defines its own copy rather than sharing a header,
   * matching gui.c/remote_control.c/metadata_db.c already doing the same. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* Same override root assets.c's own asset_path() checks (see its own
   * comment there) -- duplicated here rather than shared via a header,
   * same convention as MUSIC_ROOT_DIR above. Only defined on target:
   * asset_path() never checks any override root on HOST_BUILD, so
   * plugin.set_icon() is a no-op there (see its own comment below) rather
   * than writing files nothing would ever read. */
  #define PLUGIN_THEME_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#endif

/* Same "each file defines its own derived-path macro" convention as
 * MUSIC_ROOT_DIR above -- matches gui.c's own PLAYLISTS_DIR exactly. */
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

/* Concurrently loaded plugins. Each keeps its own lua_State for the whole
 * session and runs its top-level code at boot, so this bounds resident
 * memory and startup time; enabled files past it are reported as over the
 * limit in Plugin Manager instead of being skipped silently. */
#define PLUGIN_MAX_FILES 32
#define PLUGIN_MAX_LIST_ITEMS 500
#define PLUGIN_MAX_PLAYER_ACTIONS 32

// How many album cover requests to queue, before rejecting
#define PLUGIN_ALBUM_COVER_QUEUE 32
/* Lookups answered per player tick. Each one is a database query plus a
 * cache-file check on the UI thread, so a full queue is spread over ticks. */
#define PLUGIN_ALBUM_COVER_PER_TICK 4

typedef struct {
    lua_State * L;
    int open_ref; /* LUA_REGISTRYINDEX ref to the on_open function */
    char id[40];  /* only set/used for plugin_home_tiles[] -- empty for plugin_stream_tiles[],
                     which has no reordering concept and so never needs a stable name to
                     reference; home_layout_config.order[]/tiles[] key lookups need one. */
    char label[64];
    char icon[96];
    char icon_selected[96];
} plugin_tile_t;

/* Leaner sibling of plugin_tile_t for plugin.register_list_item() -- pill-
 * list rows (screen_builders.h's pill_list_item_t) now DO have an icon/
 * height/width/text_size slot (this session's row-layout work),
 * populated from this call's optional 4th `options` table -- see
 * append_list_item()'s own comment. Empty string ("") means "unset" for
 * icon_path/text_size, matching pill_list_item_t's own NULL convention one
 * level up (plugin_manager_get_*_list_item_options() below translates
 * empty-string back to NULL for its callers). */
typedef struct {
    lua_State * L;
    int open_ref;
    char label[64];
    char icon_path[256];
    char group[33];
    int32_t row_height;
    int32_t row_width;
    char text_size[8];
} plugin_list_item_t;

typedef struct {
    lua_State * L;
    char id[64];
    char name[96];
    char version[32];
    bool defined;
    /* Set before lua_close() on a failed load so __gc finalizers cannot
     * republish native plugin resources during teardown. */
    bool disposing;
    /* Set when a plugin call blew past the hard time budget and was
     * longjmp-aborted (see plugin_call()). Its lua_State is then left frozen
     * mid-execution and must never be run again. */
    bool aborted;
    ui_customization_override_t ui_override;
    uint64_t music_layout_sequence;
    uint64_t drawer_layout_sequence;
    time_t last_library_refresh;
    /* Source filename (basename, e.g. "Themes.lua"), not just the derived
     * plugin id -- needed by plugin_manager_scan_available() to map a
     * loaded instance back to the file it came from even when the plugin
     * declared its own custom plugin.define() id rather than falling back
     * to the "legacy.<filename>" id below. */
    char filename[256];
} plugin_instance_t;

static plugin_instance_t plugin_instances[PLUGIN_MAX_FILES];
static lua_State * mseb_owner;
static int plugin_instance_count = 0;
/* Enabled files the last init left unloaded because PLUGIN_MAX_FILES was
 * reached, kept as filename hashes for plugin_manager_scan_available(). */
#define PLUGIN_OVER_LIMIT_MAX 64
static uint64_t plugin_over_limit[PLUGIN_OVER_LIMIT_MAX];
static int plugin_over_limit_count = 0;

static uint64_t plugin_filename_hash(const char * name) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) name; *p; p++) h = (h ^ *p) * UINT64_C(1099511628211);
    return h;
}
static int loading_plugin_slot = -1;
static lua_State * plugin_led_override_owner = NULL;
static uint64_t ui_layout_sequence;

typedef struct {
    lua_State * L;
    int callback_ref;
    char owner[64];
    char action[PLAYER_LAYOUT_ACTION_MAX];
} plugin_player_action_t;

static plugin_player_action_t plugin_player_actions[PLUGIN_MAX_PLAYER_ACTIONS];
static plugin_instance_t * plugin_instance_for_state(lua_State * L);
static int plugin_call(lua_State * L, int nargs, int nresults, int errfunc);
static bool plugin_archive_owner_is_aborted(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    return inst && inst->aborted;
}
static bool plugin_remote_resolver_registered(lua_State * L);
static void plugin_player_actions_clear_owner(lua_State * L, const char * owner, bool aborted);

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
        lua_rawgeti(L, LUA_REGISTRYINDEX, entry->callback_ref);
        lua_pushstring(L, action_copy);
        int result = plugin_call(L, 1, 0, 0);
        if (inst->aborted) {
            /* Hard-aborted states are frozen, so do not inspect their stack. */
            plugin_player_actions_clear_owner(L, inst->id, true);
        } else {
            if (result != LUA_OK) {
                const char * error = lua_tostring(L, -1);
                fprintf(stderr, "[plugins] player action '%s' callback error: %s\n",
                        action_copy, error ? error : "unknown error");
            }
            lua_settop(L, top);
        }
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

static void plugin_player_actions_clear_owner(lua_State * L, const char * owner, bool aborted) {
    if (owner && owner[0]) player_layouts_clear_plugin_owner(owner);
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        if (entry->L != L) continue;
        if (!aborted && entry->callback_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, entry->callback_ref);
        memset(entry, 0, sizeof(*entry));
        entry->callback_ref = LUA_NOREF;
    }
}

static void plugin_player_actions_clear_all(void) {
    player_layouts_set_action_handler(NULL);
    for (int i = 0; i < PLUGIN_MAX_PLAYER_ACTIONS; i++) {
        plugin_player_action_t * entry = &plugin_player_actions[i];
        plugin_instance_t * inst = entry->L ? plugin_instance_for_state(entry->L) : NULL;
        if (entry->L && entry->callback_ref != LUA_NOREF && (!inst || !inst->aborted))
            luaL_unref(entry->L, LUA_REGISTRYINDEX, entry->callback_ref);
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
    if (entry->L && entry->callback_ref != LUA_NOREF && !inst->aborted)
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

/* plugin.storage and plugin.secrets need to know WHICH plugin is calling
 * at arbitrary runtime (not just at plugin.define() time, unlike
 * loading_plugin_slot above) -- each plugin owns exactly one lua_State for
 * its whole lifetime (this file's own top comment), so a linear scan
 * matching L is enough; PLUGIN_MAX_FILES is small and this only runs on
 * an explicit plugin.storage/secrets call, not per frame. */
static plugin_instance_t * plugin_instance_for_state(lua_State * L) {
    for (int i = 0; i < PLUGIN_MAX_FILES; i++) {
        if (plugin_instances[i].L == L) return &plugin_instances[i];
    }
    return NULL;
}

/* A call made from inside a plugin coroutine runs on that coroutine's own
 * lua_State; the owner is always the plugin's main state so a release from
 * any of its threads matches. */


/* Returns charge-status control to the current user setting only when the
 * plugin whose main Lua state is L owns the shared override. L must already
 * be a main state: the error and failed-load paths pass one directly and
 * never touch a possibly hard-aborted state's registry. */
static void release_plugin_led_override(lua_State * L) {
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

/* Review finding: sandbox_plugin_lua_state()'s io.open()/io.lines()/
 * io.input()/io.output()/os.remove()/os.rename() wrappers (see that
 * function's own comment) only cover Lua's OWN stdlib entry points into
 * the filesystem. Several native plugin.* C functions accept a
 * caller-controlled path and reach the filesystem directly -- mkdir(),
 * fopen(), opendir(), a worker thread's own file creation -- entirely
 * bypassing those wrappers, since they never go through Lua's io/os
 * tables at all. This is the same reserved-tree check applied uniformly
 * to every one of those native entry points below (list_dir, mkdir,
 * play_file/play_list, set_icon's source path, eq_load/save_profile,
 * download_file_async's destination). Raises a clean Lua error (rather
 * than returning nil/empty) so a plugin author sees immediately that a
 * reserved path was rejected, matching io.lines()/io.input()/io.output()'s
 * own error convention above rather than inventing a different one per
 * call site. Returns the checked path so callers can use it exactly like
 * luaL_checkstring()'s result. */
static const char * check_plugin_external_path(lua_State * L, int index, const char * api) {
    return plugin_lua_check_external_path(L, index, api);
}

/* Registries for plugin.register_list_item(list_id, ...) -- native screens
 * (books, music, settings, display, playback, music_audio, music_controls,
 * music_timers, music_library, power, system, player) append their plugin rows
 * after built-in items. Storage is organized into a shared 2D array indexed
 * by target descriptor, while enforcing each target's individual max_items. */
typedef enum {
    PLUGIN_LIST_TARGET_BOOKS = 0,
    PLUGIN_LIST_TARGET_MUSIC,
    PLUGIN_LIST_TARGET_SETTINGS,
    PLUGIN_LIST_TARGET_DISPLAY,
    PLUGIN_LIST_TARGET_PLAYBACK,
    PLUGIN_LIST_TARGET_MUSIC_AUDIO,
    PLUGIN_LIST_TARGET_MUSIC_CONTROLS,
    PLUGIN_LIST_TARGET_MUSIC_TIMERS,
    PLUGIN_LIST_TARGET_MUSIC_LIBRARY,
    PLUGIN_LIST_TARGET_POWER,
    PLUGIN_LIST_TARGET_SYSTEM,
    PLUGIN_LIST_TARGET_PLAYER,
    PLUGIN_LIST_TARGET_MORE,
    PLUGIN_LIST_TARGET_COUNT
} plugin_list_target_t;

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

/* Keep physical registry storage at least as large as every per-list limit.
 * Compile-time checks prevent a future cap increase from accepting writes
 * beyond a target's row. */
#define PLUGIN_LIST_REGISTRY_CAPACITY 16
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
static plugin_list_item_t plugin_list_items[PLUGIN_LIST_TARGET_COUNT][PLUGIN_LIST_REGISTRY_CAPACITY];
static int plugin_list_item_counts[PLUGIN_LIST_TARGET_COUNT];
static uint32_t plugin_list_registry_generation = 1;

static void advance_list_registry_generation(void) {
    ++plugin_list_registry_generation;
    if (plugin_list_registry_generation == 0) ++plugin_list_registry_generation;
}

/* Separate registry for plugin.register_stream_media_tile() -- same
 * plugin_tile_t shape, different surface (gui.c's Stream Media screen).
 * See PLUGIN_MAX_STREAM_TILES's own comment in plugin_manager.h for why
 * Stream Media gets real tiles when Home doesn't. */
static plugin_tile_t plugin_stream_tiles[PLUGIN_MAX_STREAM_TILES];
static int plugin_stream_tile_count = 0;

/* Separate registry for plugin.register_home_tile() -- same plugin_tile_t
 * shape (its id field is what's actually used here), different surface
 * (gui_settings.c's build_home_screen(), via home_layout_config.order[]/
 * tiles[] naming a tile by this id). */
static plugin_tile_t plugin_home_tiles[PLUGIN_MAX_HOME_TILES];
static int plugin_home_tile_count = 0;

/* Registry for plugin.register_quick_toggle() -- its own struct rather than
 * plugin_tile_t because a quick toggle carries live state (value) and two
 * state captions a tile has no concept of, and its Lua ref is an
 * on_change(new_value) rather than a no-arg on_open. */
typedef struct {
    lua_State * L;
    int change_ref; /* LUA_REGISTRYINDEX ref to the on_change function */
    int on_hold_ref; /* optional LUA_REGISTRYINDEX ref to settings-opening callback */
    char id[40];
    char label[64];
    char icon[96];
    char icon_selected[96];
    char on_text[16];
    char off_text[16];
    bool value;
} plugin_quick_toggle_t;

static plugin_quick_toggle_t plugin_quick_toggles[PLUGIN_MAX_QUICK_TOGGLES];
static int plugin_quick_toggle_count = 0;

/* Each reusable plugin.show_list() screen owns its on_select callback. This
 * matters after Back returns from a nested list: the older screen must route
 * through its original callback, not whichever list was opened most recently. */
typedef struct {
    lua_State * L;
    int select_ref;
    lua_Integer handle; /* show_list()'s return value for the list now in this slot */
} plugin_list_callback_t;

static plugin_list_callback_t plugin_list_callbacks[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lua_Integer plugin_list_next_handle = 1;

/* Per-(pool slot, row) Lua callback ref storage for plugin.show_settings_list()
 * -- a settings-list screen carries per-row toggle/slider state that
 * needs to survive being navigated away from and back to, so storage is
 * keyed by the actual gui.c pool slot instead of "whichever call was most
 * recent". L is NULL for a row that's never been populated (both the
 * static-array initial state, and after being unref'd when its slot is
 * reused for a shorter call) -- used as l_plugin_show_settings_list()'s own
 * guard for "does this row need unref'ing before I overwrite it". */
typedef struct {
    lua_State * L;
    int callback_ref; /* on_select for a "row" row, on_change for "toggle"/"slider" */
} plugin_settings_list_row_ref_t;

static plugin_settings_list_row_ref_t
    plugin_settings_list_rows[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_ROWS];
static int plugin_settings_list_row_counts[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];

typedef struct {
    lua_State * L;
    int callback_ref;
} plugin_order_editor_callback_t;
static plugin_order_editor_callback_t plugin_order_editor_callback = { NULL, LUA_NOREF };

/* ---- plugin.* C API, exposed as a global table `plugin` in every loaded
 * script's own lua_State (see register_plugin_api() below):
 *
 *   plugin.register_list_item(list_id, label, on_open)
 *     Adds a row labeled `label` to an existing native list screen,
 *     identified by `list_id` -- "books" (gui.c's Books screen, appended
 *     after its own "Books"/"Favorites" rows), "settings" (gui.c's
 *     Settings screen, appended after its own "Playback"/"Display"/
 *     "Power"/"System"/"About" rows), "display" (gui.c's Settings ->
 *     Display sub-screen, appended after its own "Accent Color"/"Font
 *     Size"/"Screen Timeout"/"Swipe Up for Home" rows), "playback" (Settings
 *     -> Playback, after "Car Mode"/"Crossfade"/"Equalizer"/"Resume Last
 *     Track"/"Sleep Timer"/"Startup Volume"), "power" (Settings -> Power),
 *     or "system" (Settings -> System); see PLUGIN_MAX_BOOKS_LIST_ITEMS,
 *     PLUGIN_MAX_SETTINGS_LIST_ITEMS, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
 *     PLUGIN_MAX_PLAYBACK_LIST_ITEMS, PLUGIN_MAX_POWER_LIST_ITEMS, and
 *     PLUGIN_MAX_SYSTEM_LIST_ITEMS in plugin_manager.h for the per-screen
 *     caps. Every plugin that calls this gets its own row (not just the
 *     first, unlike the old register_tile()/"Audio Books" row this
 *     replaced) -- tapping it calls on_open() with no arguments.
 *
 *     Optional 4th arg `options`: { icon = "...", height = n, text_size =
 *     "small"/"medium"/"large" } -- icon is a raw absolute filesystem path
 *     (e.g. plugin.sd_root() .. "/icon.png", NOT a theme2-relative one);
 *     height (px) resizes the row (clamped, see PILL_ROW_HEIGHT_MIN/MAX,
 *     screen_builders.h); text_size picks the row's font. All three default
 *     to sensible values matching this row's own pre-existing look if
 *     omitted -- see PLUGINS.md for the full picture.
 *
 *   plugin.show_list(title, items, on_select [, options])
 *     Opens a list screen titled `title`. Each `items` entry is either a
 *     plain string (a row with just a label) or a table { label = "...",
 *     icon = "...", text_size = "..." } for a row with its own icon/text
 *     size (same `icon`/`text_size` meaning as register_list_item()'s
 *     `options` above). on_select(index): called with the 1-based Lua index
 *     of the tapped row. Optional 4th arg `options`: { height = n,
 *     selected = n } --
 *     applies to every row in this call (not per-row).
 *
 *   plugin.show_settings_list(title, items [, options])
 *     Opens a nested settings submenu titled `title`, indistinguishable
 *     from a native Settings screen -- unlike show_list() above, each row
 *     carries a real accessory and its own callback rather than one shared
 *     on_select(index). Each entry in `items` is a table: { type = "row",
 *     label = "...", on_select = function() ... end } for a plain tap (an
 *     on_select that itself calls show_settings_list() again is how a
 *     submenu nests inside a submenu); { type = "toggle", label = "...",
 *     value = true/false, on_change = function(new_value) ... end } for an
 *     on/off switch; or { type = "slider", label = "...", min = n, max = n,
 *     value = n, on_change = function(new_value) ... end } for a slider --
 *     on_change fires once when the drag is released, not on every
 *     intermediate tick. Every row type also accepts `icon`/`text_size`
 *     (same meaning as register_list_item()'s `options` above); `height`
 *     too, except for a "slider" row (its own fixed-layout card has no
 *     spare room to grow into). Capped at 24 rows and 4 slider rows per
 *     call, silently truncated past that (same convention as show_list()'s
 *     own item cap) -- an unknown/missing `type`, an unrecognized
 *     `text_size`, or a row missing its required callback, all raise a Lua
 *     error instead. Optional 3rd arg options support `{ update = true }` to
 *     refresh a live screen with the same title in place, and `preview = { ... }`
 *     using the same mode/image_path/image_fit/clock_24h fields as
 *     plugin.show_lock_screen() to embed a lock-screen preview above the rows.
 *
 *   plugin.list_dir(path)
 *     Returns an array table of { name = "...", dir = true/false } for
 *     each non-hidden entry directly under the absolute path `path`.
 *
 *   plugin.sd_root()
 *     Returns the SD card's absolute mount path, so a script can build
 *     paths under it (e.g. plugin.sd_root() .. "/Audiobooks").
 *
 *   plugin.playlist_list() -> { m3u_path, ... } | nil
 *   plugin.playlist_read(m3u_path) -> { song_path, ... } | nil
 *   plugin.playlist_create(name, song_path) -> m3u_path | nil
 *   plugin.playlist_add(m3u_path, song_path) -> bool
 *   plugin.playlist_remove(m3u_path, song_path) -> bool
 *   plugin.playlist_delete(m3u_path) -> bool
 *     CRUD over .m3u playlists under the same Playlists folder the native
 *     Playlists screen uses. create()/delete() also update the app's
 *     playlist-existence cache, so the result shows up/disappears there
 *     immediately rather than needing a full library rescan.
 *
 *   plugin.play_file(path)
 *     Plays a single file as a fresh one-song playlist.
 *
 *   plugin.play_list(paths [, start_index])
 *     Plays `paths` (an array table of file paths) as a fresh playlist,
 *     starting at the 1-based `start_index` (default 1).
 *
 *   plugin.show_toast(message [, duration_ms])
 *     Shows the same transient toast used elsewhere in the app; duration
 *     defaults to 5000ms and is clamped by validation to 100..30000ms.
 *
 *   plugin.screenshot() -> true | false, reason
 *     Starts an asynchronous screenshot of the visible display. A successful
 *     capture is saved under SD/Screenshots, flashes the display, and fires
 *     screenshot_saved(path) or screenshot_failed(reason) on the UI thread.
 *
 *   plugin.led_available() -> bool
 *     True when both charge-status LED brightness sysfs nodes exist on this
 *     board.
 *
 *   plugin.led_set(color, on_or_level)
 *     Sets one color to off, full brightness (true), or integer brightness
 *     0..100. Setting a color stops its current effect.
 *
 *   plugin.led_blink(color, on_ms, off_ms [, level])
 *     Software blink using a UI-thread LVGL timer. Each interval is clamped
 *     to 50..60000ms; level defaults to 100 and is clamped to 0..100.
 *
 *   plugin.led_breathe(color, period_ms_or_bpm [, level])
 *     Red at peak level 100 uses the kernel breathing trigger. Other colors
 *     and partial peaks use software stepping at 25Hz or less. Numeric
 *     periods 1..600 mean breaths per minute; values above 600 mean period
 *     milliseconds. Leaving kernel breathing restores trigger=none.
 *
 *   plugin.led_get(color) -> { mode, level }
 *     Reports off/on/blink/breathe/status and the configured level or peak;
 *     status mode reports the current normal-indicator brightness.
 *
 *   plugin.led_status([color])
 *     Returns one color, or both when omitted/nil, to charge-status mode.
 *     The other color keeps its plugin mode.
 *
 *   plugin.led_release()
 *     Only the most recent plugin to control an LED owns the shared override.
 *     Releasing restores charge-status indication when LED Indicator is
 *     enabled, or turns both LEDs off otherwise. Errors and plugin/UI reloads
 *     also release the owner. These synchronous calls run on the main UI
 *     thread.
 *
 *   plugin.register_stream_media_tile(label, on_open [, icon])
 *     Appended as a real icon-grid tile in gui.c's Stream Media screen
 *     (after the built-in Subsonic tile) -- for plugins that thematically
 *     belong there (a Net Radio source, or similar) rather than in the
 *     Books list. Up to PLUGIN_MAX_STREAM_TILES (plugin_manager.h) across
 *     all loaded plugins combined. icon defaults to stream_media/radio.png
 *     if omitted.
 *
 *   plugin.set_icon(theme2_relative_path, source_file_path)
 *     Reskins an EXISTING tile's icon in place (e.g. "launcher/book.png"
 *     for Home's Books tile) by copying source_file_path's bytes into the
 *     app's writable theme-override directory. Top-level calls take effect
 *     during startup; a callback can call plugin.refresh_theme() after its
 *     copies to refresh decoded images safely.
 *
 *   plugin.set_background_color(slot, rgb)
 *     Sets one of three background-color slots app-wide, live, no restart
 *     needed: "screen", "card", or "list_row" -- see
 *     l_plugin_set_background_color()'s own comment for exactly what each
 *     covers. rgb is a packed 0xRRGGBB integer, e.g. 0x1E1E22.
 *
 *   plugin.set_text_color(slot, rgb)
 *     Sets one of two text-color slots app-wide, live, no restart needed --
 *     "primary" (dominant near-white text) or "muted" (secondary/disabled-
 *     ish gray text). Destructive-red and accent-tinted text are not
 *     covered -- see l_plugin_set_text_color()'s own comment.
 *
 *   plugin.set_home_layout(tiles, options)
 *     Restyles Home's 6 fixed tiles (color/radius/size/alignment/icon), and
 *     optionally switches Home from its native icon grid to a scrollable
 *     pill-list -- see l_plugin_set_home_layout()'s own comment for the full
 *     `tiles`/`options` shape. It takes effect at startup or after
 *     plugin.refresh_theme(); persistence remains the plugin's job.
 *
 *   plugin.refresh_theme()
 *     Drops decoded image caches, rebuilds Home, and refreshes transition
 *     snapshots after the callback returns. Other screens, plugins, and
 *     connection-owning services stay alive.
 *
 *   plugin.reload_ui()
 *     Rebuilds every screen/style in the same process -- see gui_reload.h/.c
 *     for the exact scope. This is what actually removes the "next app
 *     start only" constraint plugin.set_icon()/set_home_layout() above
 *     otherwise carry -- call it after one of those and the change is
 *     visible immediately with no restart. Never touches audio playback or
 *     any network/Bluetooth/D-Bus connection.
 *
 *     ONLY call this from a callback that fires in response to an actual
 *     user action (e.g. an on_select handler when the user changes a
 *     theme) -- NEVER unconditionally from a plugin's own top-level script
 *     code. The reload re-runs every plugin's top-level code as part of
 *     its own rebuild (plugin_manager_init(), same as a real boot); an
 *     unconditional top-level call there is a request made WHILE that same
 *     reload is still running, which gui_reload_request()'s reload_in_
 *     progress guard (gui_reload.c) drops outright rather than queuing --
 *     so this doesn't loop, but it does still cost one wasted extra reload
 *     the very first time that plugin ever loads (real boot, or the next
 *     reload after installing/updating it), for no reason. Calling this
 *     conditionally, only when something actually changed, is still the
 *     correct thing to do -- the guard exists to keep a mistake here cheap,
 *     not to make the mistake free.
 *
 *   plugin.eq_load_profile(path) -> bool
 *     Loads and applies a .peq profile file (src/audio/effects/peq.c's own
 *     save/load format) from an arbitrary path, then persists it as the
 *     new always-current EQ state. Returns false (not a Lua error) if path
 *     doesn't exist or isn't readable.
 *
 *   plugin.eq_save_profile(path) -> bool
 *     Saves the current EQ state (bypass, preamp, stereo width, crossfeed,
 *     all 10 bands) to path in the same format. Returns false on write failure.
 *
 *   plugin.eq_reset()
 *     Restores every band, the preamp, bypass, stereo width, and crossfeed
 *     to peq.c's own built-in defaults. Crossfeed is off; the native PEQ
 *     screen's Reset button preserves Crossfeed instead.
 *
 *   plugin.eq_set_bypass(enabled)
 *   plugin.eq_set_preamp(db)
 *     Toggles/sets the PEQ bypass and pre-amp (dB). Crossfeed is independent.
 *
 *   plugin.eq_set_band(index, freq_hz, gain_db, q)
 *   plugin.eq_set_band_type(index, type)
 *   plugin.eq_set_band_enabled(index, enabled)
 *     Per-band controls. index is 1-based (1..10, matching every other
 *     1-based index in this API). type is "peaking", "low_shelf", or
 *     "high_shelf".
 *
 *   plugin.toggle_pause()
 *   plugin.stop()
 *   plugin.next_track()
 *   plugin.prev_track()
 *   plugin.seek(seconds)
 *   plugin.set_volume(percent)
 *   plugin.is_playing() -> bool
 *   plugin.is_paused() -> bool
 *   plugin.get_position() -> seconds
 *   plugin.get_duration() -> seconds
 *   plugin.get_volume() -> 0..100
 *   plugin.get_battery() -> { level, charging, full, external_power }
 *     Playback control/query, bridged through gui.c so a plugin-driven
 *     change stays in sync with the play/pause icon, volume slider/popup,
 *     and shuffle-aware next/prev stepping the native UI itself uses -- see
 *     gui.h's own comment on gui_plugin_toggle_pause() and neighbors.
 *     percent is 0..100, clamped.
 *
 *   plugin.http_get(url [, verify_tls]) -> status, body
 *     Synchronous GET request (src/network/http/client/http_client.c). verify_tls
 *     defaults to true. Runs on the calling thread -- i.e. whatever plugin
 *     callback invoked it, always the main UI thread -- so a slow/hanging
 *     server blocks the UI until the request completes; keep calls fast or
 *     user-initiated. Returns nil, "network error" on a DNS/connect/TLS
 *     failure; a real HTTP error status (404, 500, ...) still returns
 *     normally with that status and whatever body the server sent.
 *
 *   plugin.http_post(url, body [, content_type] [, verify_tls]) -> status, body
 *     Same contract/blocking caveat as http_get() above, for a POST.
 *     content_type defaults to "application/x-www-form-urlencoded" if
 *     omitted/nil.
 *
 *   plugin.md5(text) -> lowercase hex string
 *     MD5 of text -- for API signing schemes that need it (e.g. Last.fm's
 *     own api_sig).
 *
 *   plugin.show_text_input(title, initial_text, is_password, on_submit)
 *     Opens the app's own T9 keypad text-entry screen. on_submit(text) fires
 *     only on an actual submit (T9 Enter) -- backing out via the screen's
 *     own back button never calls it at all. This is a true SINGLETON
 *     screen (same one Wi-Fi password entry/Subsonic login use): calling it
 *     again before a pending call resolves silently replaces the pending
 *     callback -- fine for one plugin chaining calls (ask username, then
 *     password), not safe against two unrelated calls racing.
 *
 *   plugin.get_now_playing() -> title, artist, album, duration_seconds, or a
 *   single nil if nothing is loaded
 *     Pull accessor for whatever's currently loaded -- same metadata a
 *     "track_started" event handler already receives as arguments, for code
 *     that isn't reacting to that event directly.
 *
 *   plugin.on(event, callback)
 *     Subscribes to a playback or device-state event -- every subscriber
 *     fires across every loaded plugin (unlike register_list_item(), an event
 *     isn't UI real estate to divide up). Playback, device, volume, and
 *     battery events are documented in PLUGINS.md:
 *       "track_started" -- callback(title, artist, album, duration_seconds),
 *         fires whenever a new track begins playing, whatever the cause
 *         (manual tap, next/prev, gapless/crossfade auto-advance, a
 *         plugin's own play_file/play_list, remote control).
 *       "paused" / "resumed" -- callback(), no arguments.
 *       "volume_changed" -- callback(percent), coalesced to the 500ms UI
 *         tick and fired only when the applied volume percent changes.
 *       "battery_changed" -- callback(info), when level/charging/full/
 *         external_power changes.
 *       "screenshot_saved" -- callback(path), after a PNG is saved.
 *       "screenshot_failed" -- callback(reason), if the asynchronous
 *         capture or save fails.
 *       "button_lock_changed" -- callback(locked), after the native
 *         Power + Volume Up chord toggles hardware button lock.
 *       "suspending" / "system_resumed" -- callback(), around suspend-to-RAM.
 *         system_resumed is separate from playback's existing "resumed".
 *       "network_changed" -- callback(info), info = { connected = bool,
 *         wifi_enabled = bool }, when the cached Wi-Fi state changes (API 18).
 *         Fires on the UI thread from the shell's Wi-Fi poll (~5 s with the
 *         screen on, ~45 s with it off while a plugin is subscribed).
 *         connected means associated (wpa COMPLETED), possibly before DHCP.
 *       "stopped" -- callback(), no arguments, fires when playback stops
 *         outright (not paused). Does NOT cover "playlist reaches its end
 *         with repeat off" (that path doesn't route through this app's own
 *         explicit stop call sites) -- a known, documented gap, not
 *         something every plugin needs.
 *     Unknown event name raises a Lua error immediately, same convention as
 *     an unknown list_id.
 *
 *   plugin.set_interval(seconds, callback) -> handle
 *   plugin.clear_interval(handle)
 *     Generic repeating timer (LVGL-backed, same mechanism this app's own
 *     500ms UI polling loop uses) -- e.g. for periodically checking
 *     get_position() against some threshold. seconds is clamped up to a
 *     1-second minimum if lower, silently (not an error). Errors if more
 *     than PLUGIN_MAX_INTERVALS (plugin_manager.h) are active at once,
 *     across every loaded plugin combined. A callback registered this way
 *     runs on the main UI thread exactly like every other plugin callback --
 *     a slow http_get()/http_post() inside one will visibly stall the UI
 *     until it returns, same tradeoff http_get() itself already documents.
 * ---- */

/* Used by l_plugin_register_stream_media_tile() -- fills t->icon/icon_selected
 * from an explicit icon string (deriving the "_s" selected-state variant
 * the same way every real icon pair in this app is named), or from
 * (default_icon, default_icon_selected) if icon is NULL. */
static void fill_tile_icon(plugin_tile_t * t, const char * icon, const char * default_icon,
                            const char * default_icon_selected) {
    if (icon) {
        char base[80];
        snprintf(base, sizeof(base), "%s", icon);
        char * dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(t->icon, sizeof(t->icon), "%s", icon);
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s_s.png", base);
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
static bool is_valid_text_size(const char * text_size) {
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
    utf8_truncate_safe(item->label, label, sizeof(item->label));
    utf8_sanitize(item->label);
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

        lua_getfield(L, 4, "height");
        item->row_height = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, 4, "width");
        item->row_width = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

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
    utf8_truncate_safe(t->label, label, sizeof(t->label));
    utf8_sanitize(t->label);
    fill_tile_icon(t, icon, "stream_media/radio.png", "stream_media/radio_s.png");
    return 0;
}

/* Defined further down this file (duplicate-plugin-id checking); forward-
 * declared here so l_plugin_register_home_tile() below can reuse the exact
 * same id character-set rule rather than inventing a second one -- a home
 * tile id rides inside a .theme file's comma-separated home_order= line
 * (Themes.lua) the same way a plugin id never has to, so it specifically
 * must not contain a comma or whitespace; letters/digits/'.'/'_'/'-' is
 * already exactly that safe set. */
static bool plugin_id_is_valid(const char * id);
/* Defined further down; every callback into a plugin needs its time budget
 * and aborted-plugin check. */
static int plugin_call(lua_State * L, int nargs, int nresults, int errfunc);

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
    if (!plugin_id_is_valid(id) || strlen(id) >= sizeof(plugin_home_tiles[0].id)) {
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
    utf8_truncate_safe(t->label, label, sizeof(t->label));
    utf8_sanitize(t->label);
    char base[80];
    snprintf(base, sizeof(base), "%s", icon);
    char * dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(t->icon, sizeof(t->icon), "%s", icon);
    snprintf(t->icon_selected, sizeof(t->icon_selected), "%s_s.png", base);
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

    if (!plugin_id_is_valid(id) || strlen(id) >= sizeof(plugin_quick_toggles[0].id)) {
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

    char base[80];
    snprintf(base, sizeof(base), "%s", icon_copy);
    char * dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(t->icon, sizeof(t->icon), "%s", icon_copy);
    snprintf(t->icon_selected, sizeof(t->icon_selected), "%s_s.png", base);

    lua_getfield(L, 4, "icon_selected");
    const char * icon_sel = lua_tostring(L, -1);
    if (icon_sel && icon_sel[0] != '\0')
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s", icon_sel);
    lua_pop(L, 1);

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
    utf8_truncate_safe(t->label, label, sizeof(t->label));
    utf8_sanitize(t->label);
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
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
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
        lua_getfield(L, 4, "height");
        height = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 4, "width");
        width = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
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
    int slot = gui_plugin_show_list(title, labels, icon_paths, text_sizes, wrap_labels, height, width, selected_index, n, columns, replace_slot);
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) {
        luaL_unref(L, LUA_REGISTRYINDEX, new_ref);
        lua_pushnil(L);
        return 1;
    }
    if (replace_slot >= 0 && reset_scroll) gui_plugin_list_reset_scroll(slot);
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (cb->L && cb->select_ref != LUA_NOREF) luaL_unref(cb->L, LUA_REGISTRYINDEX, cb->select_ref);
    cb->L = plugin_lua_main_state(L);
    cb->select_ref = new_ref;
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

/* plugin.show_settings_list(title, items) -- a nested settings submenu with
 * real toggle/slider rows, see this file's own top-of-file doc comment.
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
            utf8_truncate_safe(dashboard.detail, detail, sizeof(dashboard.detail));
            utf8_sanitize(dashboard.detail);
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
        new_refs[count] = luaL_ref(L, LUA_REGISTRYINDEX); /* pops the function */

        toggle_initial[count] = false;
        slider_min[count] = 0;
        slider_max[count] = 100;
        slider_value[count] = 0;
        if (row_type == PLUGIN_SETTINGS_ROW_TOGGLE) {
            lua_getfield(L, -1, "value");
            toggle_initial[count] = lua_toboolean(L, -1);
            lua_pop(L, 1);
        } else if (row_type == PLUGIN_SETTINGS_ROW_SLIDER) {
            lua_getfield(L, -1, "min");
            slider_min[count] = (int) luaL_optinteger(L, -1, 0);
            lua_pop(L, 1);
            lua_getfield(L, -1, "max");
            slider_max[count] = (int) luaL_optinteger(L, -1, 100);
            lua_pop(L, 1);
            lua_getfield(L, -1, "value");
            slider_value[count] = (int) luaL_optinteger(L, -1, slider_min[count]);
            lua_pop(L, 1);
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

        lua_getfield(L, -1, "height");
        heights[count] = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "width");
        widths[count] = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

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
        if (plugin_settings_list_rows[slot][i].L) {
            luaL_unref(plugin_settings_list_rows[slot][i].L, LUA_REGISTRYINDEX,
                       plugin_settings_list_rows[slot][i].callback_ref);
        }
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
    if (plugin_order_editor_callback.L &&
        plugin_order_editor_callback.callback_ref != LUA_NOREF)
        luaL_unref(plugin_order_editor_callback.L, LUA_REGISTRYINDEX,
                   plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = plugin_lua_main_state(L);
    plugin_order_editor_callback.callback_ref = new_ref;
    return 0;
}

static int l_plugin_list_dir(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.list_dir");
    lua_newtable(L);

    DIR * d = opendir(path);
    if (!d) return 1;

    int idx = 1;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        bool is_dir = false;
        bool have_stat = stat(full, &st) == 0;
        if (have_stat) is_dir = S_ISDIR(st.st_mode);

        lua_newtable(L);
        lua_pushstring(L, ent->d_name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, is_dir);
        lua_setfield(L, -2, "dir");
        if (have_stat) {
            lua_pushinteger(L, (lua_Integer) st.st_size);
            lua_setfield(L, -2, "size");
            lua_pushinteger(L, (lua_Integer) st.st_mtime);
            lua_setfield(L, -2, "modified");
        }
        lua_rawseti(L, -2, idx++);
    }
    closedir(d);
    return 1;
}

static int l_plugin_sd_root(lua_State * L) {
    lua_pushstring(L, MUSIC_ROOT_DIR);
    return 1;
}

static int l_plugin_realpath(lua_State * L) {
    size_t length;
    const char * path = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, strlen(path) == length, 1, "path contains NUL");
    check_plugin_external_path(L, 1, "plugin.realpath");
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return plugin_lua_push_error(L, strerror(errno));
    lua_pushstring(L, resolved);
    check_plugin_external_path(L, -1, "plugin.realpath");
    return 1;
}

/* ---- File Manager for plugins (API 16) ----
 * plugin.file_copy/file_move/file_delete run the File Manager's engine
 * (file_ops.c): one operation at a time, shared with the File Manager,
 * never overwriting, and limited to the SD card because it recurses into
 * folders. The player's own folders cannot be deleted or moved here; the
 * File Manager asks the user instead. */
#define PLUGIN_FILE_OP_MAX_PATHS 4096

typedef struct {
    bool active;
    unsigned id; /* file_op_status_t.id of the plugin's operation */
    lua_State * L;
    int callback_ref;
} plugin_file_op_t;

static plugin_file_op_t plugin_file_op = { .callback_ref = LUA_NOREF };

typedef struct { bool active; lua_State * L; int callback_ref; } plugin_file_picker_t;
static plugin_file_picker_t plugin_file_picker = { .callback_ref = LUA_NOREF };

/* A canonical path on the SD card (aliases such as "sd_0/." or "sd_0//"
 * would reach the whole card): the card root itself only when allow_root
 * (a destination folder). */
static bool plugin_sd_path_ok(const char * path, bool allow_root) {
    return file_op_path_in_root(path, MUSIC_ROOT_DIR) && (allow_root || strcmp(path, MUSIC_ROOT_DIR) != 0);
}

static bool plugin_song_path_valid(const char * path);

static bool plugin_picker_folder_valid(const char * folder) {
    if (!plugin_sd_path_ok(folder, true)) return false;
    char root_real[PATH_MAX], folder_real[PATH_MAX];
    if (!realpath(MUSIC_ROOT_DIR, root_real) || !realpath(folder, folder_real)) return false;
    size_t root_len = strlen(root_real);
    if (strncmp(folder_real, root_real, root_len) != 0 ||
        (folder_real[root_len] != '\0' && folder_real[root_len] != '/')) return false;
    struct stat st;
    return stat(folder_real, &st) == 0 && S_ISDIR(st.st_mode);
}

/* plugin.file_copy(paths, dest_dir [, callback]) and friends. paths is one
 * path or a list. Returns true, or nil and an error; callback(result) gets
 * { done, total, failed, stopped } when the operation ends. */
static int plugin_file_op_start(lua_State * L, file_op_kind_t kind, const char * api) {
    int callback_index = kind == FILE_OP_DELETE ? 2 : 3;
    const char * dest_dir = NULL;
    if (kind != FILE_OP_DELETE) {
        dest_dir = luaL_checkstring(L, 2);
        if (!plugin_sd_path_ok(dest_dir, true))
            return luaL_error(L, "%s: dest_dir must be a folder on the SD card", api);
        if (plugin_path_in_program_dirs(dest_dir))
            return luaL_error(L, "%s: the program folders (.apps, .apps-data, .emulators) are reserved", api);
    }
    bool has_callback = !lua_isnoneornil(L, callback_index);
    if (has_callback) luaL_checktype(L, callback_index, LUA_TFUNCTION);
    bool list = lua_istable(L, 1);
    if (!list) luaL_checkstring(L, 1);
    lua_Integer count = list ? (lua_Integer) lua_rawlen(L, 1) : 1;
    if (count < 1 || count > PLUGIN_FILE_OP_MAX_PATHS)
        return luaL_error(L, "%s: give 1 to %d paths", api, PLUGIN_FILE_OP_MAX_PATHS);
    /* Pointers into the strings of argument 1, which keeps them alive;
     * file_op_start() copies them. Userdata, so a Lua error cannot leak it. */
    char ** paths = lua_newuserdatauv(L, (size_t) count * sizeof(*paths), 0);
    for (lua_Integer i = 0; i < count; i++) {
        if (list) lua_rawgeti(L, 1, i + 1);
        const char * path = lua_type(L, list ? -1 : 1) == LUA_TSTRING ? lua_tostring(L, list ? -1 : 1) : NULL;
        if (!path || !plugin_sd_path_ok(path, false))
            return luaL_error(L, "%s: every path must be a file or folder on the SD card", api);
        if (kind != FILE_OP_COPY && file_op_is_player_folder(path, MUSIC_ROOT_DIR))
            return plugin_lua_push_error(L, "the player's own folders cannot be deleted or moved");
        if (kind != FILE_OP_COPY && plugin_path_in_program_dirs(path))
            return plugin_lua_push_error(L, "the program folders (.apps, .apps-data, .emulators) cannot be changed");
        if (kind != FILE_OP_DELETE) {
            /* The engine creates dest_dir/<source name>, which can itself be a reserved name. */
            const char * base = strrchr(path, '/');
            char created[PATH_MAX];
            if (snprintf(created, sizeof(created), "%s/%s", dest_dir, base ? base + 1 : path) >= (int) sizeof(created) ||
                plugin_path_in_program_dirs(created))
                return plugin_lua_push_error(L, "the program folders (.apps, .apps-data, .emulators) cannot be changed");
        }
        paths[i] = (char *) path;
        if (list) lua_pop(L, 1);
    }
    if (usb_mode_control_storage_write_blocked()) return plugin_lua_push_error(L, "USB storage is connected");
    if (plugin_file_op.active || !file_op_start(kind, paths, (int) count, dest_dir))
        return plugin_lua_push_error(L, "another file operation is running");

    file_op_status_t status;
    file_op_get_status(&status);
    plugin_file_op.active = true;
    plugin_file_op.id = status.id;
    plugin_file_op.L = plugin_lua_main_state(L);
    plugin_file_op.callback_ref = LUA_NOREF;
    if (has_callback) {
        lua_pushvalue(L, callback_index);
        plugin_file_op.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_file_copy(lua_State * L) {
    return plugin_file_op_start(L, FILE_OP_COPY, "plugin.file_copy");
}

static int l_plugin_file_move(lua_State * L) {
    return plugin_file_op_start(L, FILE_OP_MOVE, "plugin.file_move");
}

static int l_plugin_file_delete(lua_State * L) {
    return plugin_file_op_start(L, FILE_OP_DELETE, "plugin.file_delete");
}

static void push_file_op_status(lua_State * L, const file_op_status_t * status) {
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, status->running); lua_setfield(L, -2, "running");
    lua_pushinteger(L, status->done); lua_setfield(L, -2, "done");
    lua_pushinteger(L, status->total); lua_setfield(L, -2, "total");
    lua_pushinteger(L, status->failed); lua_setfield(L, -2, "failed");
    lua_pushboolean(L, status->stopped); lua_setfield(L, -2, "stopped");
    lua_pushstring(L, status->current); lua_setfield(L, -2, "current");
}

/* plugin.file_operation_status() -> the running or last operation, whoever
 * started it: { running, done, total, failed, stopped, current }. */
static int l_plugin_file_operation_status(lua_State * L) {
    file_op_status_t status;
    file_op_get_status(&status);
    push_file_op_status(L, &status);
    return 1;
}

/* plugin.cancel_file_operation() -> whether this plugin's operation was
 * asked to stop. An operation the user started is left alone. */
static int l_plugin_cancel_file_operation(lua_State * L) {
    file_op_status_t status;
    file_op_get_status(&status);
    bool own = plugin_file_op.active && plugin_file_op.L == plugin_lua_main_state(L) && status.running &&
               status.id == plugin_file_op.id;
    if (own) file_op_cancel();
    lua_pushboolean(L, own);
    return 1;
}

/* plugin.open_file_manager([folder]) -> true, or nil and an error. */
static int l_plugin_open_file_manager(lua_State * L) {
    const char * folder = luaL_optstring(L, 1, NULL);
    if (folder && !plugin_sd_path_ok(folder, true))
        return luaL_error(L, "plugin.open_file_manager: folder must be on the SD card");
    if (!gui_plugin_open_file_manager(folder)) return plugin_lua_push_error(L, "could not open that folder");
    lua_pushboolean(L, true);
    return 1;
}

/* plugin.pick_file([folder], callback) -> true | nil, error. */
static int l_plugin_pick_file(lua_State * L) {
    const char * folder = luaL_optstring(L, 1, NULL);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (folder && !plugin_picker_folder_valid(folder))
        return luaL_error(L, "plugin.pick_file: folder must be on the SD card");
    if (plugin_file_picker.active) return plugin_lua_push_error(L, "another file picker is already open");
    if (!gui_plugin_pick_file(folder)) return plugin_lua_push_error(L, "could not open that folder");
    plugin_file_picker.active = true;
    plugin_file_picker.L = plugin_lua_main_state(L);
    lua_pushvalue(L, 2);
    plugin_file_picker.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, true);
    return 1;
}

static void plugin_file_picker_release(bool cancel_ui) {
    plugin_file_picker_t * job = &plugin_file_picker;
    lua_State * L = job->L;
    int ref = job->callback_ref;
    job->active = false;
    job->L = NULL;
    job->callback_ref = LUA_NOREF;
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (L && ref != LUA_NOREF && !(inst && inst->aborted)) luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (cancel_ui) gui_plugin_cancel_file_picker();
}

void plugin_manager_file_picker_complete(const char * path) {
    plugin_file_picker_t * job = &plugin_file_picker;
    if (!job->active) return;
    lua_State * L = job->L;
    int ref = job->callback_ref;
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!L || ref == LUA_NOREF || (inst && inst->aborted)) {
        plugin_file_picker_release(false);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    plugin_file_picker_release(false);
    if (path && plugin_song_path_valid(path)) lua_pushstring(L, path);
    else lua_pushnil(L);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] file picker callback error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

/* Forgets the callback without stopping the operation, which the user may
 * still want to finish (plugin unload, failed load, deinit). */
static void plugin_file_op_release(void) {
    plugin_file_op_t * job = &plugin_file_op;
    plugin_instance_t * inst = plugin_instance_for_state(job->L);
    if (job->L && job->callback_ref != LUA_NOREF && !(inst && inst->aborted))
        luaL_unref(job->L, LUA_REGISTRYINDEX, job->callback_ref);
    job->callback_ref = LUA_NOREF;
    job->L = NULL;
}

/* Delivers the callback once the plugin's operation has ended. */
static void plugin_file_op_poll(void) {
    plugin_file_op_t * job = &plugin_file_op;
    if (!job->active) return;
    file_op_status_t status;
    file_op_get_status(&status);
    if (status.running && status.id == job->id) return;
    job->active = false;
    gui_plugin_refresh_file_browsers();
    lua_State * L = job->L;
    int ref = job->callback_ref;
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!L || ref == LUA_NOREF || (inst && inst->aborted)) {
        plugin_file_op_release();
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    plugin_file_op_release();
    /* A newer operation already replaced the result (it ended and another
     * began between two polls). */
    if (status.id == job->id) push_file_op_status(L, &status);
    else lua_pushnil(L);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] file operation callback error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

/* plugin.mkdir(path) -> true | nil, error. mkdir -p semantics: existing
 * directories are success, while an existing non-directory remains an
 * error. This intentionally follows Lua's existing unrestricted path-based
 * file operations; plugins are not confined to sd_root() today. */
static int l_plugin_mkdir(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.mkdir");
    if (plugin_path_in_program_dirs(path))
        return plugin_lua_push_error(L, "the program folders (.apps, .apps-data, .emulators) are reserved");
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) {
        return plugin_lua_push_error(L, "invalid path");
    }

    char buf[PATH_MAX];
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (buf[0] != '\0' && mkdir(buf, 0755) != 0) {
            int saved_errno = errno;
            struct stat st;
            if (saved_errno != EEXIST || stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
                buf[i] = saved;
                lua_pushnil(L);
                lua_pushstring(L, strerror(saved_errno));
                return 2;
            }
        }
        buf[i] = saved;
    }

    lua_pushboolean(L, true);
    return 1;
}

/* Optional { open_player = false } table at stack index `idx`: start
 * playback without opening Now Playing (capability playback.stay_on_screen). */
static bool play_opens_player(lua_State * L, int idx) {
    if (lua_isnoneornil(L, idx)) return true;
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "open_player");
    bool open = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    return open;
}

static int l_plugin_play_file(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.play_file");
    gui_plugin_play_paths(&path, 1, 0, play_opens_player(L, 2));
    return 0;
}

static int l_plugin_play_list(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int start = (int) luaL_optinteger(L, 2, 1) - 1;
    /* Read optional arguments before pushing userdata: Lua C API arguments
     * are stack indices, and the path buffers below would otherwise occupy
     * index 3 for the common two-argument plugin.play_list(paths, start) call. */
    bool open_player = play_opens_player(L, 3);

    lua_Unsigned raw_n = lua_rawlen(L, 1);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;
    if (n <= 0) return 0;
    if (start < 0) start = 0;
    if (start >= n) start = n - 1;

    char (*path_bufs)[512] = lua_newuserdata(L, (size_t) n * sizeof(*path_bufs));
    const char ** paths = lua_newuserdata(L, (size_t) n * sizeof(*paths));
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        const char * s = lua_tostring(L, -1);
        if (s && plugin_storage_path_is_reserved(s)) {
            lua_pop(L, 1);
            return luaL_error(L, "plugin.play_list: path is reserved for plugin.storage/plugin.secrets");
        }
        snprintf(path_bufs[i], sizeof(path_bufs[i]), "%s", s ? s : "");
        paths[i] = path_bufs[i];
        lua_pop(L, 1);
    }

    gui_plugin_play_paths(paths, n, start, open_player);
    return 0;
}

/* Shared by every numeric remote-track field below -- rejects (rather than
 * silently wrapping/truncating via an unsigned cast, which is what a bare
 * "(uint32_t) luaL_optnumber(...)" would otherwise do) a negative value, a
 * NaN, an infinity, or anything past max_value. A plain lua_Number
 * comparison against a finite max_value already correctly rejects NaN
 * (every comparison against NaN is false) and +-infinity (always outside
 * any finite range) with no separate isnan()/isfinite() check needed. */
static lua_Number check_bounded_number(lua_State * L, int table_index, const char * field, const char * fn_name, lua_Number max_value) {
    lua_getfield(L, table_index, field);
    lua_Number val = luaL_optnumber(L, -1, 0);
    lua_pop(L, 1);
    if (!(val >= 0) || !(val <= max_value)) {
        luaL_error(L, "%s: %s must be a finite number between 0 and %.0f", fn_name, field, (double) max_value);
    }
    return val;
}

/* Reads one remote_track_meta_t out of the Lua table at stack index
 * table_index (a plain {provider=..., track_id=..., stream_url=..., ...}
 * value, not a class/metatable of any kind). provider/track_id/stream_url
 * identify the track; stream_url may be omitted for on-demand resolution.
 * Other fields default to empty/zero/true(verify_tls),
 * matching plugin.http_request()'s own required-vs-optional-field style
 * above. Raises a Lua error (via luaL_error, which never returns) on any
 * oversized or missing-required field rather than silently truncating --
 * same reasoning as l_plugin_http_request()'s own header-length fix. */
/* Shared link fields for queue descriptors and asynchronous resolver answers. */
static void parse_remote_stream_fields(lua_State * L, int index, remote_track_meta_t * out,
                                      const char * fn, bool require_url) {
    lua_getfield(L, index, "stream_url");
    const char * url = luaL_optstring(L, -1, "");
    if (require_url && !url[0]) luaL_error(L, "%s: stream_url must not be empty", fn);
    if (strlen(url) >= sizeof(out->stream_url)) luaL_error(L, "%s: stream_url is too long", fn);
    snprintf(out->stream_url, sizeof(out->stream_url), "%s", url);
    lua_pop(L, 1);
    lua_getfield(L, index, "codec");
    const char * codec = luaL_optstring(L, -1, "");
    if (codec[0] && strcasecmp(codec, "mp3") && strcasecmp(codec, "flac") && strcasecmp(codec, "aac"))
        luaL_error(L, "%s: unknown codec '%s' -- must be mp3, flac, aac, or omitted", fn, codec);
    if (strlen(codec) >= sizeof(out->codec)) luaL_error(L, "%s: codec is too long", fn);
    snprintf(out->codec, sizeof(out->codec), "%s", codec);
    lua_pop(L, 1);
    out->sample_rate = (unsigned int) check_bounded_number(L, index, "sample_rate", fn, 1000000.0);
    out->bit_depth = (unsigned int) check_bounded_number(L, index, "bit_depth", fn, 64.0);
    out->channels = (unsigned int) check_bounded_number(L, index, "channels", fn, 64.0);
    out->bitrate_kbps = (unsigned int) check_bounded_number(L, index, "bitrate_kbps", fn, 100000.0);
    lua_getfield(L, index, "url_ttl_s");
    if (!lua_isnil(L, -1)) {
        lua_Integer ttl = luaL_checkinteger(L, -1);
        if (ttl < 1 || ttl > 86400) luaL_error(L, "%s: url_ttl_s must be between 1 and 86400", fn);
        out->url_ttl_s = (uint32_t) ttl;
    }
    lua_pop(L, 1);
}

static void parse_remote_track_table(lua_State * L, int table_index, remote_track_meta_t * out, const char * fn_name) {
    memset(out, 0, sizeof(*out));

    lua_getfield(L, table_index, "provider");
    const char * provider = luaL_checkstring(L, -1);
    if (provider[0] == '\0') luaL_error(L, "%s: provider must not be empty", fn_name);
    if (strlen(provider) >= sizeof(out->provider)) luaL_error(L, "%s: provider is too long", fn_name);
    snprintf(out->provider, sizeof(out->provider), "%s", provider);
    lua_pop(L, 1);

    lua_getfield(L, table_index, "track_id");
    const char * track_id = luaL_checkstring(L, -1);
    if (track_id[0] == '\0') luaL_error(L, "%s: track_id must not be empty", fn_name);
    if (strlen(track_id) >= sizeof(out->track_id)) luaL_error(L, "%s: track_id is too long", fn_name);
    snprintf(out->track_id, sizeof(out->track_id), "%s", track_id);
    lua_pop(L, 1);

    /* Reuses remote_track_make_key()'s own validation (non-empty, fits,
     * and -- the part a plain non-empty/length check above can't catch --
     * no '/' or control character in either field, which would otherwise
     * let two different (provider, track_id) pairs produce the exact same
     * "remote://..." key and collide in Favorites/History/play-count).
     * Only the validity is used here; the key itself is recomputed
     * on-demand wherever it's actually needed (remote_track.c). */
    {
        char key[256];
        if (!remote_track_make_key(out->provider, out->track_id, key, sizeof(key))) {
            luaL_error(L, "%s: provider/track_id must not contain '/' or control characters, and must fit the synthetic key", fn_name);
        }
    }

    parse_remote_stream_fields(L, table_index, out, fn_name, false);

#define OPT_STR_FIELD(field, name) \
    lua_getfield(L, table_index, name); \
    const char * field##_val = luaL_optstring(L, -1, ""); \
    if (strlen(field##_val) >= sizeof(out->field)) luaL_error(L, "%s: " name " is too long", fn_name); \
    snprintf(out->field, sizeof(out->field), "%s", field##_val); \
    lua_pop(L, 1);

    OPT_STR_FIELD(title, "title")
    OPT_STR_FIELD(artist, "artist")
    OPT_STR_FIELD(album, "album")
    OPT_STR_FIELD(artwork_url, "artwork_url")
#undef OPT_STR_FIELD

    /* Bounds are generous (well past anything a real audio stream would
     * ever declare) -- these are display-only fields (format badge,
     * quality metadata) except duration_ms, which also seeds the decoder's
     * total_frames for a remote MP3/AAC stream (audio.c's decoder_open());
     * the point is rejecting garbage (negative/NaN/absurdly large) before
     * it's cast to an unsigned type below, not modeling real codec limits. */
    out->duration_ms = (uint32_t) check_bounded_number(L, table_index, "duration_ms", fn_name, 24.0 * 3600.0 * 1000.0);

    lua_getfield(L, table_index, "replaygain_db");
    if (!lua_isnil(L, -1)) {
        double gain = luaL_checknumber(L, -1);
        /* +-100 dB is already an absurd amount of gain -- this is purely a
         * NaN/infinity/garbage-value guard (see resolve_replaygain()'s own
         * comment for how this feeds into playback volume), not a
         * meaningful real-world limit. */
        if (!(gain >= -100.0 && gain <= 100.0)) luaL_error(L, "%s: replaygain_db must be a finite number between -100 and 100", fn_name);
        out->has_replaygain = true;
        out->replaygain_db = gain;
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "verify_tls");
    out->verify_tls = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(owner);
    if (!inst || inst->disposing || inst->aborted)
        luaL_error(L, "%s: plugin is no longer active", fn_name);
    out->resolver_owner = (uintptr_t) owner;
    snprintf(out->resolver_plugin, sizeof(out->resolver_plugin), "%s", inst->id);
    out->resolve_stream = plugin_remote_resolver_registered(owner);
}

static int l_plugin_play_remote(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    remote_track_meta_t track;
    parse_remote_track_table(L, 1, &track, "plugin.play_remote");
    gui_plugin_play_remote_tracks(&track, 1, 0);
    return 0;
}

static int l_plugin_queue_remote_list(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int start = (int) luaL_optinteger(L, 2, 1) - 1;

    lua_Unsigned raw_n = lua_rawlen(L, 1);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;
    if (n <= 0) return 0;
    if (start < 0) start = 0;
    if (start >= n) start = n - 1;

    /* remote_track_meta_t is a few KB each (mostly its bounded string
     * fields) -- a fixed PLUGIN_MAX_LIST_ITEMS-sized static array here
     * would be a ~2MB permanent footprint on this device regardless of how
     * many tracks a given call actually queues. Sized to n (the real,
     * already-capped count) instead, and allocated as Lua userdata rather
     * than malloc()'d: parse_remote_track_table() below can luaL_error()
     * (longjmp) mid-loop on a malformed entry, and a plain malloc()'d
     * buffer would leak in that case since nothing downstream of a longjmp
     * ever runs our own free() -- Lua's GC owns and reclaims userdata
     * regardless of how the call unwinds, once nothing pushed after it on
     * the stack (nothing is, here) keeps it live past this function. */
    remote_track_meta_t * tracks = (remote_track_meta_t *) lua_newuserdata(L, sizeof(remote_track_meta_t) * (size_t) n);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        parse_remote_track_table(L, lua_gettop(L), &tracks[i], "plugin.queue_remote_list");
        lua_pop(L, 1);
    }

    gui_plugin_play_remote_tracks(tracks, n, start);
    return 0;
}

/* Plugin errors are English. A known reason becomes the player's own
 * translated message; otherwise the plugin text is kept only for English. */
static const char * remote_stream_error_text(const char * reason, const char * error) {
    static const struct { const char * reason; const char * text; } reasons[] = {
        { "subscription", N_("Not available in your subscription") },
        { "unavailable", N_("This track is unavailable") },
        { "quality", N_("This quality is unavailable for this track") },
        { "protected", N_("This track is copy-protected") },
        { "network", N_("Could not reach the streaming service") },
        { "auth", N_("Sign in to the streaming service again") },
    };
    if (reason)
        for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++)
            if (!strcmp(reason, reasons[i].reason)) return reasons[i].text;
    const char * language = i18n_get_language();
    bool english = !language || !language[0] || !strncmp(language, "en", 2);
    return english && error && error[0] ? error : N_("Could not get a stream link");
}

static int l_plugin_provide_remote_stream(lua_State * L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id <= 0) return luaL_error(L, "plugin.provide_remote_stream: invalid request id");
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(owner);
    if (!inst || inst->disposing || inst->aborted) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_getfield(L, 2, "error");
    const char * error = luaL_optstring(L, -1, NULL);
    lua_getfield(L, 2, "reason");
    const char * reason = luaL_optstring(L, -1, NULL);
    if ((error && error[0]) || (reason && reason[0])) {
        lua_pushboolean(L, remote_track_stream_provide((uintptr_t) owner, (uint64_t) id, NULL,
                                                       remote_stream_error_text(reason, error)));
    } else {
        remote_track_meta_t stream = {0};
        parse_remote_stream_fields(L, 2, &stream, "plugin.provide_remote_stream", true);
        lua_pushboolean(L, remote_track_stream_provide((uintptr_t) owner, (uint64_t) id, &stream, NULL));
    }
    return 1;
}

static int l_plugin_show_toast(lua_State * L) {
    const char * msg = luaL_checkstring(L, 1);
    lua_Integer duration_ms = luaL_optinteger(L, 2, 5000);
    if (duration_ms < 100 || duration_ms > 30000)
        return luaL_error(L, "plugin.show_toast: duration_ms must be between 100 and 30000");
    gui_plugin_show_toast(msg, (uint32_t) duration_ms);
    return 0;
}

/* API 16: one modeless progress card. Handles are never recycled during
 * this process, so a dismissed/replaced card cannot be reopened by a stale
 * asynchronous completion. All access is on the Lua/UI thread. */
static lua_State * plugin_progress_owner = NULL;
static lua_Integer plugin_progress_handle = 0;
static lua_Integer plugin_progress_next_handle = 1;

static int plugin_progress_percent(lua_State * L, int index) {
    if (lua_isnoneornil(L, index)) return -1;
    double fraction = luaL_checknumber(L, index);
    if (!isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
        luaL_error(L, "plugin progress: fraction must be between 0 and 1, or nil");
    return (int) (fraction * 100.0 + 0.5);
}

static const char * plugin_progress_text(lua_State * L, int index, size_t max) {
    size_t length;
    const char * text = luaL_checklstring(L, index, &length);
    if (length > max || memchr(text, '\0', length))
        luaL_error(L, "plugin progress: text is too long or contains NUL");
    return text;
}

static void plugin_progress_reset(void) {
    gui_plugin_close_progress();
    plugin_progress_owner = NULL;
    plugin_progress_handle = 0;
}

static int l_plugin_show_progress(lua_State * L) {
    const char * title = plugin_progress_text(L, 1, 128);
    const char * message = plugin_progress_text(L, 2, 512);
    int percent = plugin_progress_percent(L, 3);
    if (plugin_progress_next_handle == LUA_MAXINTEGER)
        return plugin_lua_push_error(L, "progress handle limit reached");
    if (!gui_plugin_show_progress(title, message, percent))
        return plugin_lua_push_error(L, "progress UI unavailable");
    plugin_progress_owner = plugin_lua_main_state(L);
    plugin_progress_handle = plugin_progress_next_handle++;
    lua_pushinteger(L, plugin_progress_handle);
    return 1;
}

static int l_plugin_update_progress(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    const char * message = plugin_progress_text(L, 2, 512);
    int percent = plugin_progress_percent(L, 3);
    bool ok = handle > 0 && handle == plugin_progress_handle
        && plugin_progress_owner == plugin_lua_main_state(L)
        && gui_plugin_update_progress(message, percent);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_close_progress(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    bool owned = handle > 0 && handle == plugin_progress_handle
        && plugin_progress_owner == plugin_lua_main_state(L);
    bool visible = owned && gui_plugin_progress_is_visible();
    if (owned) plugin_progress_reset();
    lua_pushboolean(L, visible);
    return 1;
}

static int l_plugin_screenshot(lua_State * L) {
    screenshot_result_t result = screenshot_start();
    if (result == SCREENSHOT_RESULT_STARTED) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    lua_pushstring(L, screenshot_result_reason(result));
    return 2;
}

#ifndef HOST_BUILD
/* True when both paths exist as regular files of equal size and equal
 * bytes. Used by copy_file() to skip a NAND rewrite of an icon that is
 * already the requested image -- Themes.lua copies ~140 assets on every
 * load, and after the first successful apply those destinations already
 * match. */
static bool files_identical(const char * a_path, const char * b_path) {
    struct stat sa, sb;
    if (stat(a_path, &sa) != 0 || stat(b_path, &sb) != 0) return false;
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode)) return false;
    if (sa.st_size != sb.st_size) return false;

    FILE * a = fopen(a_path, "rb");
    FILE * b = fopen(b_path, "rb");
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return false;
    }

    char ba[4096], bb[4096];
    bool same = true;
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), a);
        size_t nb = fread(bb, 1, sizeof(bb), b);
        if (na != nb || memcmp(ba, bb, na) != 0) {
            same = false;
            break;
        }
        if (na == 0) break;
    }
    if (ferror(a) || ferror(b)) same = false;
    fclose(a);
    fclose(b);
    return same;
}

/* Atomic byte copy: readers see either the old complete icon or the new
 * one, never a partially-written PNG. */
static bool copy_file(const char * src_path, const char * dst_path) {
    if (files_identical(src_path, dst_path)) return true;
    FILE * in = fopen(src_path, "rb");
    if (!in) return false;
    char tmp_path[640];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", dst_path, (long) getpid()) >= (int) sizeof(tmp_path)) {
        fclose(in);
        return false;
    }
    FILE * out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    bool ok = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) != 0) ok = false;

    if (ok) ok = rename(tmp_path, dst_path) == 0;
    if (!ok) remove(tmp_path);
    return ok;
}
#endif

#ifndef HOST_BUILD
/* options.background_image (PLUGINS.md, plugin.set_home_layout()): copies a
 * plugin-supplied SD-card image into a FIXED theme-override destination
 * ("home/background.<ext>", extension preserved from source_path so the
 * right LVGL decoder -- lv_lodepng_init()/lv_tjpgd_init(), assets.c's own
 * assets_init() -- picks it up), then records that relative path in
 * *config for build_home_screen() (gui_settings.c) to read. Unlike
 * l_plugin_set_icon() below, the destination name is fixed by native code
 * rather than plugin-controlled, so this needs none of set_icon()'s own
 * ".." rejection -- only source_path (already validated by
 * check_plugin_external_path() at the call site) ever reaches copy_file().
 * No-op (config left untouched) on HOST_BUILD, where there is no writable
 * override root for asset_path() to ever check -- see PLUGIN_THEME_
 * OVERRIDE_ROOT's own comment. */
static void set_home_background_image(lua_State * L, const char * source_path, home_layout_config_t * config) {
    const char * dot = strrchr(source_path, '.');
    const char * ext = NULL;
    if (dot && strcasecmp(dot, ".png") == 0) ext = ".png";
    else if (dot && strcasecmp(dot, ".jpg") == 0) ext = ".jpg";
    else if (dot && strcasecmp(dot, ".jpeg") == 0) ext = ".jpeg";
    else if (dot && strcasecmp(dot, ".bin") == 0) ext = ".bin";
    if (!ext) {
        luaL_error(L, "plugin.set_home_layout: options.background_image '%s' must be a .png, .jpg, .jpeg, or valid RGB565 .bin file",
                   source_path);
        return; /* unreachable -- luaL_error() longjmps */
    }

    char relative_path[32];
    snprintf(relative_path, sizeof(relative_path), "home/background%s", ext);
    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s%s", PLUGIN_THEME_OVERRIDE_ROOT, relative_path);

    if (strcmp(ext, ".bin") == 0) {
        int fd = open(source_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        lv_image_header_t header;
        memset(&header, 0, sizeof(header));
        struct stat st;
        memset(&st, 0, sizeof(st));
        lv_display_t * display = lv_display_get_default();
        int max_w = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
        int max_h = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
        bool valid = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
        if (valid) {
            size_t got = 0;
            while (got < sizeof(header)) {
                ssize_t n = read(fd, (uint8_t *) &header + got, sizeof(header) - got);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += (size_t) n;
            }
            valid = got == sizeof(header);
        }
        if (fd >= 0) close(fd);
        uint64_t expected = (uint64_t) sizeof(header) + (uint64_t) header.stride * header.h;
        valid = valid && header.magic == LV_IMAGE_HEADER_MAGIC && header.cf == LV_COLOR_FORMAT_RGB565 &&
                header.flags == 0 && header.reserved_2 == 0 &&
                header.w == (uint32_t) max_w && header.h == (uint32_t) max_h &&
                header.stride == header.w * 2u &&
                expected <= sizeof(header) + (uint64_t) max_w * (uint64_t) max_h * 2u &&
                st.st_size >= 0 && (uint64_t) st.st_size == expected;
        if (!valid) {
            luaL_error(L, "plugin.set_home_layout: options.background_image '%s' is not a valid RGB565 LVGL image for this display",
                       source_path);
            return;
        }
    }

    /* Same best-effort two-level mkdir as l_plugin_set_icon() below. */
    mkdir(PLUGIN_THEME_OVERRIDE_ROOT, 0755);
    char dir_only[600];
    snprintf(dir_only, sizeof(dir_only), "%shome", PLUGIN_THEME_OVERRIDE_ROOT);
    mkdir(dir_only, 0755);

    if (!copy_file(source_path, dst_path)) {
        luaL_error(L, "plugin.set_home_layout: could not copy options.background_image '%s' to '%s'",
                   source_path, relative_path);
        return; /* unreachable */
    }

    snprintf(config->background_image, sizeof(config->background_image), "%s", relative_path);
    config->has_background_image = true;
}
#endif

/* Reskins an EXISTING theme2 asset in place, e.g.
 * plugin.set_icon("launcher/book.png", plugin.sd_root() .. "/my_book.png")
 * to change what Home's Books tile looks like -- a different case from
 * plugin.register_stream_media_tile()'s own icon argument (which points a
 * BRAND NEW tile at whatever icon it likes with no special handling
 * needed). Copies source_path's bytes into
 * THEME_OVERRIDE_ROOT/relative_path, the same writable-override mechanism
 * assets.c's own asset_path() already checks first for every icon
 * resolution in the app (already how Subsonic's own non-stock icon works
 * today).
 *
 * LVGL caches decoded images by path, so callback-time batches must end
 * with plugin.refresh_theme(). Startup calls need no refresh because
 * plugin_manager_init() runs before the UI is built. */
static int l_plugin_set_icon(lua_State * L) {
    const char * relative_path = luaL_checkstring(L, 1);
    const char * source_path = check_plugin_external_path(L, 2, "plugin.set_icon");

#ifndef HOST_BUILD
    /* Reject absolute paths and directory traversal to constrain writes
     * within PLUGIN_THEME_OVERRIDE_ROOT. */
    if (relative_path[0] == '/' || strstr(relative_path, "..") != NULL) {
        return luaL_error(L, "plugin.set_icon: relative_path must be a plain path under the theme root, got '%s'",
                           relative_path);
    }

    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s%s", PLUGIN_THEME_OVERRIDE_ROOT, relative_path);

    /* Best-effort mkdir, ignore EEXIST -- same pattern
     * playlist_files_create() already uses. Every real theme2 asset in
     * this app is exactly one directory level deep (e.g. "launcher/",
     * "stream_media/"), so one mkdir beyond the root is enough; this
     * doesn't attempt a general recursive mkdir -p for deeper paths
     * nothing in this app's own asset layout actually needs. */
    mkdir(PLUGIN_THEME_OVERRIDE_ROOT, 0755);
    char dir_only[600];
    snprintf(dir_only, sizeof(dir_only), "%s", dst_path);
    char * slash = strrchr(dir_only, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir_only, 0755);
    }

    if (!copy_file(source_path, dst_path)) {
        return luaL_error(L, "plugin.set_icon: could not copy '%s' to '%s'", source_path, relative_path);
    }
    gui_themes_clear_legacy_exclusion(relative_path);
#else
    (void) source_path; /* no override root on HOST_BUILD -- see assets.c's asset_path() */
    gui_themes_clear_legacy_exclusion(relative_path);
#endif
    return 0;
}

/* Sets one of three fixed background-color slots, live, app-wide, no
 * restart needed -- see gui_plugin_set_background_color()'s own comment
 * in gui.c for the shared-style mechanism (same one apply_accent_color()
 * already uses successfully for the existing accent color feature).
 * "screen": every screen's own background. "card": popups, EQ cards,
 * settings slider cards -- every neutral dark surface that isn't a plain
 * screen or a list row. "list_row": every row in every list screen (All
 * Songs, Artists, Playlists, Files, Queue, ...). */
static int l_plugin_set_background_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "screen") != 0 && strcmp(slot, "card") != 0 && strcmp(slot, "list_row") != 0) {
        return luaL_error(L, "plugin.set_background_color: unknown slot '%s' (expected \"screen\", \"card\", or \"list_row\")",
                           slot);
    }

    gui_plugin_set_background_color(slot, (uint32_t) rgb);
    return 0;
}

/* Sets one of two fixed text-color slots, live, app-wide, no restart needed
 * -- see gui_plugin_set_text_color()'s own comment in gui.c. "primary": the
 * app's dominant near-white text (labels, titles, list rows). "muted":
 * secondary/disabled-ish gray text (chevrons, timestamps, subtitles).
 * Destructive-red and accent-tinted text are NOT covered by either slot --
 * they're semantically fixed, not part of the light/dark background split
 * plugin.set_background_color() drives. */
static int l_plugin_set_text_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "primary") != 0 && strcmp(slot, "muted") != 0) {
        return luaL_error(L, "plugin.set_text_color: unknown slot '%s' (expected \"primary\" or \"muted\")", slot);
    }

    gui_plugin_set_text_color(slot, (uint32_t) rgb);
    return 0;
}

/* Reads an optional 0xRRGGBB-style integer field into out_has/out_value --
 * *out_has stays whatever the caller initialized it to (false) if the field
 * is nil/absent, so a tile table that only sets some fields leaves the rest
 * genuinely unset rather than defaulting to black/0. luaL_checkinteger (not
 * lua_tointeger) so a wrong-typed value fails loudly rather than silently
 * becoming 0, same convention every other typed plugin.* field uses. */
static void get_opt_color_field(lua_State * L, int idx, const char * field, bool * out_has, uint32_t * out_value) {
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1)) {
        *out_has = true;
        *out_value = (uint32_t) luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
}

/* Same shape, for an optional boolean field (accessory/icon) -- lua_toboolean
 * alone can't tell "false" apart from "absent", both would otherwise read as
 * false, so *out_has records whether the field was actually present.
 * lua_toboolean() itself (not a stricter LUA_TBOOLEAN type check) is
 * deliberate: Lua's own C API has no narrower notion of "boolean" than
 * truthiness, and every other boolean-ish field in this file (toggle
 * initial state, verify_tls, ...) already reads exactly this way -- adding
 * a stricter check here alone would make this one field the inconsistent
 * one, not the other way around. */
static void get_opt_bool_field(lua_State * L, int idx, const char * field, bool * out_has, bool * out_value) {
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1)) {
        *out_has = true;
        *out_value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
}

/* Validates that a 64-bit Lua integer fits within int32_t bounds before casting. */
static int32_t check_int32_field(lua_State * L, lua_Integer value, const char * fn_name, const char * field) {
    if (value < INT32_MIN || value > INT32_MAX) {
        return (int32_t) luaL_error(L, "%s: %s (%lld) is out of range", fn_name, field, (long long) value);
    }
    return (int32_t) value;
}

/* Restyles Home's tiles and controls which ones are shown, and in what
 * order (PLUGINS.md). This never writes anything to disk; persistence
 * requires a top-level re-call next boot. plugin.refresh_theme() applies a
 * callback-time change immediately.
 *
 * Every plugin restyles the SAME single global layout -- there is no
 * per-plugin slot, so if two installed plugins both call this, whichever
 * one loads later (see plugin_manager_init()'s now-deterministic,
 * alphabetical-by-filename load order) completely replaces the earlier
 * one's config, not merged.
 *
 * `tiles`: array of tables, one per tile being restyled -- { key, bg_color,
 * text_color, radius, height, width, align, accessory, text_size, icon }.
 * `key` is one of the native names ("music"/"stream_media"/"wireless"/
 * "books"/"settings"/"dac"/"subsonic") or a plugin.register_home_tile() id -- unlike
 * before API 7, an unrecognized-looking key is NOT rejected here: it may
 * name a plugin tile that hasn't registered yet (plugin_manager_init()
 * loads plugins in filename order, so a theme plugin can run before the
 * plugin providing a tile it references), so resolution against both the
 * native keys and the live plugin-tile registry happens later, in
 * build_home_screen(), which always runs after every plugin has finished
 * loading. A tile key never mentioned keeps every field at its native
 * default; a REPEATED key within this same array cleanly replaces the
 * earlier entry for that tile rather than merging the two. height/width are
 * passed through unclamped (same as register_list_item()'s own options) --
 * build_pill_list_screen() clamps them to PILL_ROW_HEIGHT_MIN/MAX and
 * PILL_ROW_WIDTH_MIN/MAX itself when list mode actually renders them.
 *
 * `options` (optional): { mode = "tile"|"list", tile_gap, row_gap, order }.
 * `mode` defaults to "tile" (today's icon grid) if `options` or `mode` is
 * omitted. `tile_gap`/`row_gap` are passed through unclamped here too --
 * clamped to 0-64/0-84 respectively in build_icon_grid_screen()/build_pill_
 * list_screen() (screen_builders.c), the same "clamp at the point of use"
 * pattern height/width above already follow, since tile_gap in particular
 * feeds directly into an available-space subtraction that goes wrong long
 * before any Lua-side integer range check would think to reject it.
 *
 * `order` (optional array of strings): which tiles Home shows and in what
 * position -- position IS order. Each entry is a native key or a plugin
 * tile id, same "resolved later" deferral as `tiles`' own `key` above.
 * Omitted entirely (order left empty) means "unconfigured", falling back to
 * today's exact fixed 6-native-tile order. A native key simply not
 * mentioned in `order` is not shown at all -- this is how a theme drops a
 * tile, not just reorders one. Duplicate entries are rejected immediately
 * (unlike unresolved-key names, a literal repeat is unambiguously a
 * mistake, not a load-order timing issue). */
static int l_plugin_set_home_layout(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    home_layout_config_t config;
    memset(&config, 0, sizeof(config));
    config.configured = true;

    /* A `tiles` array longer than the max any theme could ever legitimately
     * need cannot arise from real use, only from a duplicate key or a
     * copy-paste mistake, so this fails loudly instead of quietly
     * truncating. */
    lua_Unsigned raw_n = lua_rawlen(L, 1);
    if (raw_n > (lua_Unsigned) HOME_LAYOUT_MAX_TILES) {
        return luaL_error(L, "plugin.set_home_layout: tiles has %lld entries, more than the %d-tile limit",
                           (long long) raw_n, HOME_LAYOUT_MAX_TILES);
    }
    int n = (int) raw_n;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1); /* tile table -> stack top */
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "plugin.set_home_layout: tile %d must be a table", i + 1);
        }

        lua_getfield(L, -1, "key");
        const char * key = lua_tostring(L, -1);
        if (!key || key[0] == '\0') {
            return luaL_error(L, "plugin.set_home_layout: tile %d has a missing or empty key", i + 1);
        }
        if (strlen(key) >= sizeof(config.tiles[0].key)) {
            return luaL_error(L, "plugin.set_home_layout: tile %d's key '%s' is too long", i + 1, key);
        }
        char key_copy[sizeof(config.tiles[0].key)];
        snprintf(key_copy, sizeof(key_copy), "%s", key);
        lua_pop(L, 1); /* key string */

        /* A repeated key within one call cleanly replaces the earlier entry
         * rather than merging fields across the two -- without this, a
         * second { key = "music", text_color = ... } entry would keep
         * whatever an earlier { key = "music", bg_color = ... } entry set,
         * silently combining two calls' worth of fields into one tile. */
        int idx = -1;
        for (int t = 0; t < config.tile_count; t++) {
            if (strcmp(config.tiles[t].key, key_copy) == 0) { idx = t; break; }
        }
        if (idx < 0) {
            if (config.tile_count >= HOME_LAYOUT_MAX_TILES) {
                return luaL_error(L, "plugin.set_home_layout: too many distinct tile keys (max %d)",
                                   HOME_LAYOUT_MAX_TILES);
            }
            idx = config.tile_count++;
            snprintf(config.tiles[idx].key, sizeof(config.tiles[idx].key), "%s", key_copy);
        }
        home_tile_override_t * ov = &config.tiles[idx].override;
        memset(ov, 0, sizeof(*ov));
        int row_idx = lua_gettop(L); /* the tile table itself, still on the stack */

        get_opt_color_field(L, row_idx, "bg_color", &ov->has_bg_color, &ov->bg_color);
        get_opt_color_field(L, row_idx, "text_color", &ov->has_text_color, &ov->text_color);

        lua_getfield(L, row_idx, "radius");
        if (!lua_isnil(L, -1)) {
            lua_Integer radius = luaL_checkinteger(L, -1);
            if (radius < 0) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has a negative radius", i + 1);
            }
            ov->has_radius = true;
            ov->radius = check_int32_field(L, radius, "plugin.set_home_layout", "radius");
        }
        lua_pop(L, 1);

        lua_getfield(L, row_idx, "height");
        ov->height = check_int32_field(L, luaL_optinteger(L, -1, 0), "plugin.set_home_layout", "height");
        lua_pop(L, 1);

        lua_getfield(L, row_idx, "width");
        ov->width = check_int32_field(L, luaL_optinteger(L, -1, 0), "plugin.set_home_layout", "width");
        lua_pop(L, 1);

        lua_getfield(L, row_idx, "align");
        const char * align = lua_tostring(L, -1);
        if (align) {
            if (strcmp(align, "left") != 0 && strcmp(align, "center") != 0 && strcmp(align, "right") != 0) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has an unknown align '%s' "
                                   "(expected \"left\", \"center\", or \"right\")", i + 1, align);
            }
            snprintf(ov->align, sizeof(ov->align), "%s", align);
        }
        lua_pop(L, 1);

        get_opt_bool_field(L, row_idx, "accessory", &ov->has_accessory, &ov->accessory);
        get_opt_bool_field(L, row_idx, "icon", &ov->has_icon, &ov->icon);

        lua_getfield(L, row_idx, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has an unknown text_size '%s' "
                                   "(expected \"small\", \"medium\", \"large\", or \"mono\")", i + 1, text_size);
            }
            snprintf(ov->text_size, sizeof(ov->text_size), "%s", text_size);
        }
        lua_pop(L, 1);

        lua_pop(L, 1); /* tile table */
    }

    /* Unlike show_list()'s own options table (a plain "resize every row"
     * convenience where silently ignoring a wrong-typed argument costs
     * nothing), a plugin passing a non-table, non-nil `options` here has
     * unambiguously made a mistake -- there is no reasonable "options" value
     * that isn't a table, so this fails loudly rather than silently
     * behaving as if `options` were omitted. */
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && !lua_istable(L, 2)) {
        return luaL_error(L, "plugin.set_home_layout: options must be a table");
    }
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "mode");
        const char * mode = lua_tostring(L, -1);
        if (mode) {
            if (strcmp(mode, "tile") == 0) config.list_mode = false;
            else if (strcmp(mode, "list") == 0) config.list_mode = true;
            else {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: unknown mode '%s' (expected \"tile\" or \"list\")", mode);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "tile_gap");
        config.tile_gap = check_int32_field(L, luaL_optinteger(L, -1, 0), "plugin.set_home_layout", "tile_gap");
        lua_pop(L, 1);

        lua_getfield(L, 2, "row_gap");
        config.row_gap = check_int32_field(L, luaL_optinteger(L, -1, 0), "plugin.set_home_layout", "row_gap");
        lua_pop(L, 1);

#ifndef HOST_BUILD
        lua_getfield(L, 2, "background_image");
        if (!lua_isnil(L, -1)) {
            const char * source_path = check_plugin_external_path(L, -1, "plugin.set_home_layout");
            set_home_background_image(L, source_path, &config);
        }
        lua_pop(L, 1);
#endif

        lua_getfield(L, 2, "order");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                return luaL_error(L, "plugin.set_home_layout: options.order must be an array table");
            }
            lua_Unsigned order_n = lua_rawlen(L, -1);
            if (order_n > (lua_Unsigned) HOME_LAYOUT_MAX_TILES) {
                return luaL_error(L, "plugin.set_home_layout: options.order has %lld entries, more than the %d-tile limit",
                                   (long long) order_n, HOME_LAYOUT_MAX_TILES);
            }
            int order_idx = lua_gettop(L); /* the order table itself, still on the stack */
            int order_n_int = (int) order_n;
            for (int i = 0; i < order_n_int; i++) {
                lua_rawgeti(L, order_idx, i + 1);
                const char * entry = lua_tostring(L, -1);
                if (!entry || entry[0] == '\0') {
                    return luaL_error(L, "plugin.set_home_layout: options.order entry %d must be a non-empty string",
                                       i + 1);
                }
                if (strlen(entry) >= sizeof(config.order[0])) {
                    return luaL_error(L, "plugin.set_home_layout: options.order entry %d ('%s') is too long",
                                       i + 1, entry);
                }
                /* Unlike an unresolved-key name (which may just not have
                 * registered yet, see this function's own doc comment), a
                 * literal duplicate within one `order` array is unambiguous
                 * -- reject immediately rather than defer. */
                for (int j = 0; j < i; j++) {
                    if (strcmp(config.order[j], entry) == 0) {
                        return luaL_error(L, "plugin.set_home_layout: options.order has a duplicate entry '%s'", entry);
                    }
                }
                snprintf(config.order[i], sizeof(config.order[i]), "%s", entry);
                lua_pop(L, 1); /* entry string */
            }
            config.order_count = order_n_int;
        }
        lua_pop(L, 1); /* order (nil or table) */

        /* build_icon_grid_screen() (screen_builders.c) cannot scroll -- its
         * vertical scroll-direction setting exists only to stop it from
         * capturing the app-wide back-swipe gesture, not to provide real
         * scrolling -- so a tile-mode order past 2 columns x 3 reference
         * rows (ICON_GRID_REFERENCE_ROWS*2, screen_builders.c -- duplicated
         * here as a literal since that constant isn't exposed via a header)
         * would render with tiles silently unreachable off-screen. Fail
         * loud here instead, matching this function's own tiles-array-
         * length rejection above and register_stream_media_tile()'s own
         * cap rejection. */
        if (!config.list_mode && config.order_count > 6) {
            return luaL_error(L, "plugin.set_home_layout: tile mode supports at most 6 tiles (got %d in "
                               "options.order) -- use { mode = \"list\" } for more", config.order_count);
        }
    }

    gui_plugin_set_home_layout(&config);
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

static void refresh_plugin_ui_customization(void) {
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
    out->list_mode = strcmp(mode, "list") == 0;
    lua_pop(L, 1);

    lua_getfield(L, idx, "row_gap");
    out->row_gap = check_int32_field(L, luaL_optinteger(L, -1, 0),
                                     "plugin.set_launcher_layout", "row_gap");
    lua_pop(L, 1);
    lua_getfield(L, idx, "height");
    out->height = check_int32_field(L, luaL_optinteger(L, -1, 0),
                                    "plugin.set_launcher_layout", "height");
    lua_pop(L, 1);
    lua_getfield(L, idx, "width");
    out->width = check_int32_field(L, luaL_optinteger(L, -1, 0),
                                   "plugin.set_launcher_layout", "width");
    lua_pop(L, 1);

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

/* ---- plugin.eq_*() -- bridge straight into peq.c's public API, no gui.c
 * layer needed (peq.c has no LVGL dependency, matching l_plugin_list_dir()/
 * l_plugin_sd_root() already calling plain C functions directly). Every
 * setter ends with peq_save(), the same "persist on every change" pattern
 * every native EQ-screen event handler already follows (see e.g. gui.c's
 * eq_bypass_switch_event_cb()), so a plugin-driven EQ change survives
 * reboot with no new persistence code. ---- */

static int l_plugin_eq_load_profile(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.eq_load_profile");
    bool ok = peq_load_from_path(path);
    if (ok) peq_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_eq_apply_profile(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.eq_apply_profile");
    bool persist = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    bool ok = peq_load_from_path(path);
    if (ok && persist) peq_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_eq_save_profile(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.eq_save_profile");
    if (plugin_path_in_program_dirs(path)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, peq_save_to_path(path));
    return 1;
}

static bool plugin_song_path_valid(const char * path);

/* Stock MSEB is a separate native DSP stage. Keep this bridge deliberately
 * transactional: every field is validated before publishing a new config. */
static const char * const mseb_value_fields[] = {
    "temperature", "bass_ext", "bass_texture", "thickness", "vocal_pos",
    "female_vocal", "sibilance_lf", "sibilance_hf", "impulse_tone", "air"
};

static int l_plugin_mseb_apply_state(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    mseb_config_t config = { 0 };
    lua_getfield(L, 1, "enabled");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TBOOLEAN, 1, "enabled must be boolean");
    config.enabled = lua_toboolean(L, -1); lua_pop(L, 1);
    for (int i = 0; i < 10; i++) {
        lua_getfield(L, 1, mseb_value_fields[i]);
        luaL_argcheck(L, lua_isinteger(L, -1), 1, "all control values must be integers");
        lua_Integer value = lua_tointeger(L, -1); lua_pop(L, 1);
        luaL_argcheck(L, value >= -100 && value <= 100, 1, "all control values must be between -100 and 100");
        config.values[i] = (int) value;
    }
    lua_getfield(L, 1, "width");
    luaL_argcheck(L, lua_type(L, -1) == LUA_TNUMBER, 1, "width must be a number");
    config.width = lua_tonumber(L, -1); lua_pop(L, 1);
    luaL_argcheck(L, isfinite(config.width) && config.width >= 0.0 && config.width <= 2.0,
                  1, "width must be finite and between 0 and 2");
    if (!mseb_set_config(&config)) { lua_pushboolean(L, false); return 1; }
    mseb_owner = config.enabled ? plugin_lua_main_state(L) : NULL;
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_get_mseb_state(lua_State * L) {
    mseb_config_t config;
    mseb_get_config(&config);
    lua_createtable(L, 0, 12);
    lua_pushboolean(L, config.enabled); lua_setfield(L, -2, "enabled");
    for (int i = 0; i < 10; i++) {
        lua_pushinteger(L, config.values[i]); lua_setfield(L, -2, mseb_value_fields[i]);
    }
    lua_pushnumber(L, config.width); lua_setfield(L, -2, "width");
    return 1;
}

/* Bounded cache lookup for one validated local SD path. This never scans tags
 * or walks the library; an unopened cache or absent row simply returns nil. */
static int l_plugin_get_track_metadata(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    if (!plugin_song_path_valid(path)) { lua_pushnil(L); return 1; }
    song_row_t row;
    if (!metadata_db_get_song_by_path(path, &row)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, row.id); lua_setfield(L, -2, "id");
    lua_pushstring(L, row.path); lua_setfield(L, -2, "path");
    lua_pushstring(L, row.tags.title); lua_setfield(L, -2, "title");
    lua_pushstring(L, row.tags.artist); lua_setfield(L, -2, "artist");
    lua_pushstring(L, row.tags.album); lua_setfield(L, -2, "album");
    lua_pushstring(L, row.tags.album_artist); lua_setfield(L, -2, "album_artist");
    lua_pushstring(L, row.tags.genre); lua_setfield(L, -2, "genre");
    return 1;
}

/* Same "pure audio-domain, straight into audio.c, no gui.c bridge" shape as
 * plugin.eq_*() above -- setting a hardware volume curve isn't paired with
 * any gui.c-local UI state (it doesn't move the volume slider or its
 * popup, it changes what a given slider position maps to internally), so
 * it doesn't need one. In-process only, like every other plugin config
 * call in this file -- not persisted, a plugin re-applies it at the top of
 * its own script on every boot (same convention set_home_layout() etc.
 * already use, see their own comments). HW_VOLUME_CURVE_LEN is audio.h's
 * own shared definition, not a local one, so this validation can't drift
 * out of sync with audio_set_custom_hw_volume_curve()'s actual buffer
 * size.
 *
 * loading_plugin_slot >= 0 for exactly the duration of a plugin's own
 * top-level script run (set/cleared around plugin_call() in
 * load_plugin_file(), never true from a later callback -- a real Gain
 * Mode plugin's Settings-list on_select handler, for instance, always
 * runs with it back at -1). Inside that window this call is part of
 * plugin_manager_init()'s own load transaction (which may still load more
 * plugin files, or discard this very one if it fails on a later call),
 * so it only stages the state change -- audio_stage_custom_hw_volume_
 * curve(), no hardware write, and no effect on the live state
 * audio_apply_volume() reads -- and load_plugin_file()/plugin_manager_
 * init() own the single deferred commit (see audio.h's own comment on
 * why an immediate write per call here would risk an intermediate value
 * actually reaching the DAC, or surviving a later failure). Outside that
 * window -- a live, already-loaded plugin changing its own curve --
 * audio_set_custom_hw_volume_curve() applies it immediately instead, for
 * real-time audible feedback. */
static int l_plugin_set_hw_volume_curve(lua_State * L) {
    bool during_load = loading_plugin_slot >= 0;

    if (lua_isnoneornil(L, 1)) {
        if (during_load) audio_stage_custom_hw_volume_curve(false, NULL);
        else audio_set_custom_hw_volume_curve(NULL);
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Unsigned raw_n = lua_rawlen(L, 1);
    if (raw_n != HW_VOLUME_CURVE_LEN)
        return luaL_error(L, "plugin.set_hw_volume_curve: table must have exactly %d entries (UI volume 0..100), got %d",
                           HW_VOLUME_CURVE_LEN, (int) raw_n);

    uint8_t curve[HW_VOLUME_CURVE_LEN];
    for (int i = 0; i < HW_VOLUME_CURVE_LEN; i++) {
        lua_rawgeti(L, 1, i + 1);
        /* lua_tointeger() alone silently converts a missing/nonnumeric/
         * non-integral entry to 0 -- and raw 0 is the hardware's LOUDEST
         * point (see compute_hw_raw()'s own comment in audio.c), so a
         * malformed table (a hole, a string, a float like 128.5) could
         * otherwise turn into an unexpected full-volume step instead of a
         * caught error. lua_isinteger() rejects all of those before any
         * conversion happens. */
        if (!lua_isinteger(L, -1)) {
            int t = lua_type(L, -1);
            lua_pop(L, 1);
            return luaL_error(L, "plugin.set_hw_volume_curve: table[%d] (UI volume %d%%) is not an integer (got %s)",
                               i + 1, i, lua_typename(L, t));
        }
        lua_Integer v = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (v < 0 || v > 255)
            return luaL_error(L, "plugin.set_hw_volume_curve: table[%d] (UI volume %d%%) is %lld, must be 0..255",
                               i + 1, i, (long long) v);
        curve[i] = (uint8_t) v;
    }
    if (during_load) audio_stage_custom_hw_volume_curve(true, curve);
    else audio_set_custom_hw_volume_curve(curve);
    return 0;
}

/* ---- plugin.toggle_pause()/stop()/next_track()/prev_track()/seek()/
 * set_volume()/is_playing()/is_paused()/get_position()/get_duration() --
 * thin wrappers over gui.h's gui_plugin_*() bridges, needed here (unlike
 * plugin.eq_*() above) because audio.c's own functions must be paired with
 * gui.c-local UI state (play/pause icon, volume slider/popup, shuffle-aware
 * stepping) -- see gui.h's own comment on gui_plugin_toggle_pause() and
 * neighbors for why. ---- */

static int l_plugin_toggle_pause(lua_State * L) {
    (void) L;
    gui_plugin_toggle_pause();
    return 0;
}

static int l_plugin_stop(lua_State * L) {
    (void) L;
    gui_plugin_stop();
    return 0;
}

static int l_plugin_next_track(lua_State * L) {
    (void) L;
    gui_plugin_next_track();
    return 0;
}

static int l_plugin_prev_track(lua_State * L) {
    (void) L;
    gui_plugin_prev_track();
    return 0;
}

static int l_plugin_seek(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    gui_plugin_seek(seconds);
    return 0;
}

static int l_plugin_set_playback_speed(lua_State * L) {
    const char * directory = lua_isnil(L, 1) ? NULL :
        check_plugin_external_path(L, 1, "plugin.set_playback_speed");
    double speed = luaL_checknumber(L, 2);
    lua_pushboolean(L, audio_set_playback_speed(directory, speed));
    return 1;
}

static int l_plugin_get_playback_speed(lua_State * L) {
    lua_pushnumber(L, audio_get_playback_speed());
    return 1;
}

static int l_plugin_set_ab_loop(lua_State * L) {
    lua_pushboolean(L, audio_set_ab_loop(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
static int l_plugin_get_ab_loop(lua_State * L) {
    double start, end;
    if (!audio_get_ab_loop(&start, &end)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushnumber(L, start); lua_setfield(L, -2, "start");
    lua_pushnumber(L, end); lua_setfield(L, -2, "finish");
    return 1;
}
static int l_plugin_clear_ab_loop(lua_State * L) {
    (void)L;
    audio_clear_ab_loop();
    return 0;
}
/* plugin.audio_loop_start(path) -> true | false. Pauses a playing track
 * first; the clip then repeats until audio_loop_stop() or until a track
 * resumes or starts. See audio_loop_start(). */
static int l_plugin_audio_loop_start(lua_State * L) {
    const char * path = check_plugin_external_path(L, 1, "plugin.audio_loop_start");
    if (audio_is_playing()) gui_plugin_toggle_pause();
    lua_pushboolean(L, audio_loop_start(path));
    return 1;
}
static int l_plugin_audio_loop_stop(lua_State * L) {
    (void)L;
    audio_loop_stop();
    return 0;
}
static int l_plugin_audio_loop_active(lua_State * L) {
    lua_pushboolean(L, audio_loop_is_active());
    return 1;
}
static int l_plugin_prepare_ab_switch(lua_State * L) {
    const char *path = check_plugin_external_path(L, 1, "plugin.prepare_ab_switch");
    lua_pushboolean(L, audio_prepare_ab_switch(path));
    return 1;
}
static int l_plugin_select_ab_source(lua_State * L) {
    const char *source = luaL_checkstring(L, 1);
    if (strcmp(source, "a") != 0 && strcmp(source, "b") != 0)
        return luaL_error(L, "source must be 'a' or 'b'");
    lua_pushboolean(L, audio_select_ab_source(strcmp(source, "b") == 0));
    return 1;
}
static int l_plugin_clear_ab_switch(lua_State * L) {
    (void)L;
    audio_clear_ab_switch();
    return 0;
}
static int l_plugin_get_ab_switch(lua_State * L) {
    bool preparing, ready, source_b;
    audio_get_ab_switch(&preparing, &ready, &source_b);
    lua_newtable(L);
    lua_pushboolean(L, preparing); lua_setfield(L, -2, "preparing");
    lua_pushboolean(L, ready); lua_setfield(L, -2, "ready");
    lua_pushstring(L, source_b ? "b" : "a"); lua_setfield(L, -2, "source");
    return 1;
}

static int l_plugin_get_playback_format(lua_State * L) {
    audio_current_format_info_t f;
    if (!audio_get_current_format_info(&f) || !f.valid) { lua_pushnil(L); return 1; }
    static const char * const codecs[] = { "unknown", "flac", "mp3", "pcm", "dsd", "aac", "alac", "ape", "wma", "opus", "vorbis", "wavpack" };
    lua_newtable(L);
#define FMT_NUM(name, val) do { lua_pushinteger(L, (lua_Integer)(val)); lua_setfield(L, -2, name); } while (0)
    lua_pushstring(L, f.path); lua_setfield(L, -2, "path");
    int codec = (int)f.codec;
    lua_pushstring(L, codec >= 0 && (size_t)codec < sizeof(codecs) / sizeof(codecs[0]) ? codecs[codec] : "unknown"); lua_setfield(L, -2, "codec");
    FMT_NUM("sample_rate", f.source_sample_rate);
    FMT_NUM("bit_depth", f.source_bit_depth);
    FMT_NUM("output_sample_rate", f.output_sample_rate);
    FMT_NUM("output_bit_depth", f.output_bit_depth);
    FMT_NUM("channels", f.channels);
    FMT_NUM("bitrate_kbps", f.bitrate_kbps);
    lua_pushnumber(L, f.duration_seconds); lua_setfield(L, -2, "duration_seconds");
    lua_pushboolean(L, f.is_stream); lua_setfield(L, -2, "is_stream");
    lua_pushboolean(L, f.seekable); lua_setfield(L, -2, "seekable");
    lua_pushboolean(L, f.is_dsd); lua_setfield(L, -2, "is_dsd");
    lua_pushboolean(L, f.dsd_native); lua_setfield(L, -2, "dop");
    lua_pushboolean(L, f.replaygain_applied); lua_setfield(L, -2, "replaygain_applied");
    lua_pushnumber(L, f.replaygain_applied_db); lua_setfield(L, -2, "replaygain_applied_db");
    lua_pushnumber(L, f.software_volume_gain); lua_setfield(L, -2, "software_volume_gain");
    lua_pushnumber(L, f.playback_speed); lua_setfield(L, -2, "playback_speed");
    lua_pushboolean(L, f.crossfade_enabled); lua_setfield(L, -2, "crossfade_enabled");
    FMT_NUM("generation", f.generation);
#undef FMT_NUM
    return 1;
}

static int l_plugin_get_output_info(lua_State * L) {
    audio_current_format_info_t f = {0};
    bool have_format = audio_get_current_format_info(&f) && f.valid;
    audio_output_info_t out = {0};
#ifndef HOST_BUILD
    audio_output_get_info(&out);
#else
    out.active = have_format;
    out.route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.requested_route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.sample_rate = have_format ? f.output_sample_rate : 0;
    out.bit_depth = have_format ? f.output_bit_depth : 0;
    out.hardware_format_known = false;
#endif
    static const char * const route_names[] = { "wired", "bluetooth", "usb_dac" };
    const char * route = route_names[out.active ? out.route : out.requested_route];
    lua_newtable(L);
    lua_pushstring(L, route); lua_setfield(L, -2, "route");
    lua_pushboolean(L, out.active); lua_setfield(L, -2, "active");
    lua_pushboolean(L, out.dop); lua_setfield(L, -2, "dop");
    lua_pushboolean(L, out.native_dsd); lua_setfield(L, -2, "native_dsd");
    bool resampling_known = have_format && out.active && out.hardware_format_known;
    lua_pushboolean(L, resampling_known && !out.native_dsd && (out.sample_rate != f.output_sample_rate)); lua_setfield(L, -2, "resampling");
    lua_pushboolean(L, resampling_known); lua_setfield(L, -2, "resampling_known");
    if (have_format) {
        lua_pushinteger(L, out.active ? out.sample_rate : f.output_sample_rate); lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, out.active ? out.bit_depth : f.output_bit_depth); lua_setfield(L, -2, "bit_depth");
    } else { lua_pushnil(L); lua_setfield(L, -2, "sample_rate"); lua_pushnil(L); lua_setfield(L, -2, "bit_depth"); }
    if (out.hardware_format_known) {
        lua_pushinteger(L, out.sample_rate); lua_setfield(L, -2, "hardware_sample_rate");
        lua_pushinteger(L, out.bit_depth); lua_setfield(L, -2, "hardware_bit_depth");
    } else { lua_pushnil(L); lua_setfield(L, -2, "hardware_sample_rate"); lua_pushnil(L); lua_setfield(L, -2, "hardware_bit_depth"); }
#ifndef HOST_BUILD
    char codec[32] = {0};
    if ((out.active ? out.route : out.requested_route) == AUDIO_OUTPUT_ROUTE_BLUETOOTH && gui_shell_get_bt_audio_codec(codec, sizeof(codec))) {
        lua_pushstring(L, codec); lua_setfield(L, -2, "bluetooth_codec");
    } else { lua_pushnil(L); lua_setfield(L, -2, "bluetooth_codec"); }
#else
    lua_pushnil(L); lua_setfield(L, -2, "bluetooth_codec");
#endif
    return 1;
}

static int l_plugin_set_transport_skip(lua_State * L) {
    const char * directory = check_plugin_external_path(L, 1, "plugin.set_transport_skip");
    lua_Integer seconds = luaL_checkinteger(L, 2);
    if (seconds < 0 || seconds > 300) return luaL_error(L, "skip seconds must be 0..300");
    if (strlen(directory) >= PATH_MAX) return luaL_error(L, "skip directory too long");
    gui_player_set_transport_skip(directory, (int) seconds);
    return 0;
}

static int l_plugin_set_volume(lua_State * L) {
    lua_Integer percent = luaL_checkinteger(L, 1);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bool silent = false;
    bool persist = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "silent");
        silent = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    if (!persist) gui_plugin_set_volume_transient((int) percent, !silent);
    else if (silent) gui_plugin_set_volume_silent((int) percent);
    else gui_plugin_set_volume((int) percent);
    return 0;
}

static int l_plugin_is_playing(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_playing());
    return 1;
}

static int l_plugin_is_paused(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_paused());
    return 1;
}

static int l_plugin_get_position(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_position_seconds());
    return 1;
}

static int l_plugin_get_duration(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_duration_seconds());
    return 1;
}

/* plugin.get_playback_progress(expected_path) -> coherent snapshot | nil.
 * This query runs on every plugin progress tick, so keep its path check
 * bounded and allocation-free. audio.c compares it with the exact current
 * path under the playback-state lock; no filesystem lookup is needed here. */
static int l_plugin_get_playback_progress(lua_State * L) {
    size_t path_len = 0;
    const char * expected_path = luaL_checklstring(L, 1, &path_len);
    if (path_len == 0 || path_len >= PATH_MAX || expected_path[0] != '/' ||
        memchr(expected_path, '\0', path_len) != NULL) {
        lua_pushnil(L);
        return 1;
    }

    audio_playback_progress_t progress;
    if (!audio_get_playback_progress(expected_path, &progress)
        || progress.state == AUDIO_PLAYBACK_PROGRESS_NOT_READY) {
        lua_pushnil(L);
        return 1;
    }

    const char * terminal = NULL;
    switch (progress.state) {
    case AUDIO_PLAYBACK_PROGRESS_ACTIVE: terminal = "active"; break;
    case AUDIO_PLAYBACK_PROGRESS_NATURAL_EOF: terminal = "natural_eof"; break;
    case AUDIO_PLAYBACK_PROGRESS_MANUAL_STOP: terminal = "manual_stop"; break;
    case AUDIO_PLAYBACK_PROGRESS_NOT_READY:
    default:
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushnumber(L, progress.position_seconds); lua_setfield(L, -2, "position");
    lua_pushnumber(L, progress.duration_seconds); lua_setfield(L, -2, "duration");
    lua_pushboolean(L, progress.is_playing); lua_setfield(L, -2, "playing");
    lua_pushboolean(L, progress.is_paused); lua_setfield(L, -2, "paused");
    lua_pushstring(L, terminal); lua_setfield(L, -2, "terminal");
    return 1;
}

typedef struct {
    int level;
    bool charging;
    bool full;
    bool external_power;
} plugin_battery_info_t;

static plugin_battery_info_t plugin_read_battery_info(void) {
    plugin_battery_info_t info;
    int level = battery_get_display_percent(); /* same value the status bar shows */
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    battery_external_power_state_t power_state = battery_get_external_power_state();
    bool power_disconnected = power_state == BATTERY_EXTERNAL_POWER_DISCONNECTED;
    bool capped = charge_limiter_is_confirmed_off();
    info.level = level;
    info.external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
    info.full = !power_disconnected && (battery_is_full() || capped);
    info.charging = !info.full && !power_disconnected && battery_is_charging();
    return info;
}

static void push_battery_info(lua_State * L, const plugin_battery_info_t * info) {
    lua_newtable(L);
    lua_pushinteger(L, info->level); lua_setfield(L, -2, "level");
    lua_pushboolean(L, info->charging); lua_setfield(L, -2, "charging");
    lua_pushboolean(L, info->full); lua_setfield(L, -2, "full");
    lua_pushboolean(L, info->external_power); lua_setfield(L, -2, "external_power");
}

static int current_volume_percent(void) {
    int percent = (int) (audio_get_volume() * 100.0f + 0.5f);
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static int l_plugin_get_volume(lua_State * L) {
    lua_pushinteger(L, current_volume_percent());
    return 1;
}

static int l_plugin_get_battery(lua_State * L) {
    plugin_battery_info_t info = plugin_read_battery_info();
    push_battery_info(L, &info);
    return 1;
}

/* plugin.http_get(url [, verify_tls]) -> status, body | nil, "network error"
 * -- bridges http_client.c's http_get_to_buffer() directly (no gui.c layer,
 * same reasoning as plugin.eq_*() above: no LVGL/gui-state dependency).
 * body is pushed with lua_pushlstring() (exact byte count), not
 * lua_pushstring(), since a response body isn't guaranteed NUL-free. Runs
 * synchronously on the calling (main UI) thread -- see this file's own
 * top-of-file doc comment for the blocking caveat. */
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

/* The single pending plugin.show_text_input() call's on_submit function --
 * show_text_entry() (gui.c) is itself a true singleton screen. A second
 * plugin request now returns "text input busy" rather than overwriting this
 * callback, and Back explicitly releases it. on_submit only fires on a T9
 * keypad Enter, never on the screen's own back button. Documented in
 * PLUGINS.md rather than engineered away, matching this codebase's existing
 * tolerance for show_list()'s own analogous single-ref caveat. */
static lua_State * pending_text_input_L = NULL;
static int pending_text_input_ref = LUA_NOREF;

/* A confirmation callback is rooted in its owner's main state, even when
 * show_confirmation() was called from a coroutine. */
static lua_State * pending_confirmation_L = NULL;
static int pending_confirmation_ref = LUA_NOREF;

static int l_plugin_show_text_input(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    const char * initial_text = (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) ? luaL_checkstring(L, 2) : NULL;
    bool is_password = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    if (pending_text_input_L && pending_text_input_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "text input busy");
        return 2;
    }
    lua_pushvalue(L, 4);
    pending_text_input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pending_text_input_L = plugin_lua_main_state(L);

    gui_plugin_show_text_input(title, initial_text, is_password);
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_show_confirmation(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    const char * confirm_label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (pending_confirmation_L && pending_confirmation_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "confirmation busy");
        return 2;
    }
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_State * owner = plugin_lua_main_state(L);
    if (!gui_plugin_show_confirmation(title, confirm_label)) {
        luaL_unref(owner, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(L, false);
        lua_pushstring(L, "unable to show confirmation");
        return 2;
    }
    pending_confirmation_ref = ref;
    pending_confirmation_L = owner;
    lua_pushboolean(L, true);
    return 1;
}

/* plugin.get_now_playing() -> title, artist, album, duration_seconds, or a
 * single nil if nothing is loaded -- pull accessor for anything that isn't
 * reacting to the "track_started" event directly (e.g. a set_interval()
 * tick double-checking what's still playing). Backed by gui.c's own cached
 * now-playing globals, populated at the same two call sites that fire
 * "track_started" -- see gui_plugin_get_now_playing()'s own comment. */
static int l_plugin_get_now_playing(lua_State * L) {
    char title[128], artist[128], album[128];
    double duration_seconds = 0.0;
    if (!gui_plugin_get_now_playing(title, sizeof(title), artist, sizeof(artist), album, sizeof(album),
                                     &duration_seconds)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, title);
    lua_pushstring(L, artist);
    lua_pushstring(L, album);
    lua_pushnumber(L, duration_seconds);
    return 4;
}

/* plugin.get_play_mode() -> "sequential" | "repeat_all" | "repeat_one" |
 * "shuffle" | "shuffle_once" -- thin wrap of gui_plugin_get_play_mode(), same "no gui-state
 * code in this file" boundary as is_playing()/get_position() above. */
static int l_plugin_get_play_mode(lua_State * L) {
    lua_pushstring(L, gui_plugin_get_play_mode());
    return 1;
}

static int l_plugin_set_play_mode(lua_State * L) {
    const char * mode = luaL_checkstring(L, 1);
    int value;
    if (!strcmp(mode, "sequential")) value = 0;
    else if (!strcmp(mode, "repeat_all")) value = 1;
    else if (!strcmp(mode, "repeat_one")) value = 2;
    else if (!strcmp(mode, "shuffle")) value = PLAY_MODE_SHUFFLE;
    else if (!strcmp(mode, "shuffle_once")) value = PLAY_MODE_SHUFFLE_ONCE;
    else return luaL_error(L, "plugin.set_play_mode: expected sequential, repeat_all, repeat_one, shuffle, or shuffle_once");
    gui_player_set_play_mode(value);
    return 0;
}

static int l_plugin_get_crossfade(lua_State * L) { lua_pushboolean(L, current_settings.crossfade_enabled); return 1; }
static int l_plugin_get_gapless(lua_State * L) { lua_pushboolean(L, current_settings.gapless_enabled); return 1; }
static int l_plugin_get_replaygain_mode(lua_State * L) {
    static const char * const modes[] = { "off", "track", "album" };
    int mode = current_settings.replaygain_mode;
    if (mode < 0 || mode > 2) mode = 0;
    lua_pushstring(L, modes[mode]); return 1;
}
static int l_plugin_set_crossfade(lua_State * L) {
    gui_player_set_crossfade_enabled(lua_toboolean(L, 1));
    return 0;
}
static int l_plugin_set_gapless(lua_State * L) {
    gui_player_set_gapless_enabled(lua_toboolean(L, 1));
    return 0;
}
static int l_plugin_set_replaygain_mode(lua_State * L) {
    const char * mode = luaL_checkstring(L, 1);
    int value;
    if (!strcmp(mode, "off")) value = 0;
    else if (!strcmp(mode, "track")) value = 1;
    else if (!strcmp(mode, "album")) value = 2;
    else return luaL_error(L, "plugin.set_replaygain_mode: expected off, track, or album");
    gui_player_set_replaygain_mode(value);
    return 0;
}

/* plugin.get_current_track_path() -> path | nil, if nothing is loaded. */
static int l_plugin_get_current_track_path(lua_State * L) {
    const char * path = gui_plugin_get_current_track_path();
    if (!path) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, path);
    return 1;
}

/* A fetch completion must only refresh the exact track it saved for. The
 * native loader schedules disk/tag work asynchronously and rejects stale jobs. */
static int l_plugin_refresh_lyrics(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (!inst || !inst->defined)
        return luaL_error(L, "plugin.refresh_lyrics requires plugin.define first");
    size_t len = 0;
    const char * expected = luaL_checklstring(L, 1, &len);
    if (!len || len >= PATH_MAX || memchr(expected, '\0', len))
        return luaL_error(L, "plugin.refresh_lyrics: invalid track path");
    const char * current = gui_plugin_get_current_track_path();
    if (!current || strcmp(expected, current) != 0) {
        lua_pushboolean(L, false);
        lua_pushliteral(L, "track_changed");
        return 2;
    }
    gui_lyrics_load_track(gui_player_get_playlist_index(), current);
    lua_pushboolean(L, true);
    return 1;
}

/* ---- plugin.playlist_* -- CRUD over .m3u playlists under PLAYLISTS_DIR,
 * exactly where the native Playlists screen looks. playlist_create()/
 * playlist_delete() also update metadata_db's playlist-existence cache
 * (metadata_db_playlist_insert_one()/_delete_one(), reachable here via
 * gui.h's own #include "metadata_db.h") so a plugin-created/deleted
 * playlist shows up/disappears there immediately instead of needing a full
 * rescan -- matching every native call site (gui.c's own
 * add_to_playlist_confirm_cb()/playlist_delete_row_cb() and friends).
 * playlist_add()/playlist_remove() only edit a playlist's contents, which
 * that cache never tracks (confirmed against every native call site doing
 * the same), so they call playlist_files_append()/_remove() alone. ---- */

static bool plugin_playlist_name_valid(const char * name) {
    if (!name || !name[0] || strlen(name) > 200 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const unsigned char * p = (const unsigned char *) name; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return false;
    return true;
}

/* Plugin playlist CRUD is deliberately narrower than the native helpers:
 * only direct .m3u/.m3u8 children returned by playlist_list() are accepted.
 * realpath() on both the root and existing target also closes symlink
 * escapes, rather than relying on a lexical "starts with" check. */
static bool plugin_playlist_path_valid(const char * path) {
    if (!path || plugin_storage_path_is_reserved(path)) return false;
    char root_real[PATH_MAX], path_real[PATH_MAX];
    if (!realpath(PLAYLISTS_DIR, root_real) || !realpath(path, path_real)) return false;
    size_t root_len = strlen(root_real);
    if (strncmp(path_real, root_real, root_len) != 0 || path_real[root_len] != '/') return false;
    const char * base = path_real + root_len + 1;
    if (!base[0] || strchr(base, '/')) return false;
    const char * ext = strrchr(base, '.');
    if (!ext || (strcasecmp(ext, ".m3u") != 0 && strcasecmp(ext, ".m3u8") != 0)) return false;
    struct stat st;
    return stat(path_real, &st) == 0 && S_ISREG(st.st_mode);
}

static bool plugin_song_path_valid(const char * path) {
    if (!path || plugin_storage_path_is_reserved(path)) return false;
    char root_real[PATH_MAX], path_real[PATH_MAX];
    if (!realpath(MUSIC_ROOT_DIR, root_real) || !realpath(path, path_real)) return false;
    size_t root_len = strlen(root_real);
    if (strncmp(path_real, root_real, root_len) != 0 || path_real[root_len] != '/') return false;
    struct stat st;
    return stat(path_real, &st) == 0 && S_ISREG(st.st_mode);
}



/* plugin.playlist_list() -> { m3u_path, ... } | nil */
static int l_plugin_playlist_list(lua_State * L) {
    char ** paths = NULL;
    int count = 0;
    if (!playlist_files_scan(PLAYLISTS_DIR, &paths, &count)) {
        lua_pushnil(L);
        return 1;
    }
    return plugin_library_push_string_array_result(L, paths, count);
}

/* plugin.playlist_read(m3u_path) -> { song_path, ... } | nil */
static int l_plugin_playlist_read(lua_State * L) {
    const char * m3u_path = luaL_checkstring(L, 1);
    if (!plugin_playlist_path_valid(m3u_path)) return plugin_lua_push_error(L, "playlist path must be an existing .m3u file in the SD Playlists folder");
    char ** paths = NULL;
    int count = 0;
    if (!playlist_files_read(m3u_path, &paths, &count)) {
        lua_pushnil(L);
        return 1;
    }
    return plugin_library_push_string_array_result(L, paths, count);
}

/* plugin.playlist_create(name, song_path) -> m3u_path | nil */
static int l_plugin_playlist_create(lua_State * L) {
    const char * name = luaL_checkstring(L, 1);
    const char * song_path = luaL_checkstring(L, 2);

    if (!plugin_playlist_name_valid(name)) return plugin_lua_push_error(L, "invalid playlist name");
    if (!plugin_song_path_valid(song_path)) return plugin_lua_push_error(L, "song path must be an existing file on the SD card");

    char created_path[512];
    if (!playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path))) {
        return plugin_lua_push_error(L, errno == EEXIST ? "playlist already exists" : "could not create playlist");
    }
    metadata_db_playlist_insert_one(created_path);
    lua_pushstring(L, created_path);
    return 1;
}

/* plugin.playlist_add(m3u_path, song_path) -> bool */
static int l_plugin_playlist_add(lua_State * L) {
    const char * m3u_path = luaL_checkstring(L, 1);
    const char * song_path = luaL_checkstring(L, 2);
    if (!plugin_playlist_path_valid(m3u_path)) return plugin_lua_push_error(L, "invalid playlist path");
    if (!plugin_song_path_valid(song_path)) return plugin_lua_push_error(L, "invalid song path");
    if (!playlist_files_append(m3u_path, song_path)) return plugin_lua_push_error(L, "could not update playlist");
    lua_pushboolean(L, true);
    return 1;
}

/* plugin.playlist_remove(m3u_path, song_path) -> bool */
static int l_plugin_playlist_remove(lua_State * L) {
    const char * m3u_path = luaL_checkstring(L, 1);
    const char * song_path = luaL_checkstring(L, 2);
    if (!plugin_playlist_path_valid(m3u_path)) return plugin_lua_push_error(L, "invalid playlist path");
    if (!plugin_song_path_valid(song_path)) return plugin_lua_push_error(L, "invalid song path");
    if (!playlist_files_remove(m3u_path, song_path)) return plugin_lua_push_error(L, "could not update playlist");
    lua_pushboolean(L, true);
    return 1;
}

/* plugin.playlist_delete(m3u_path) -> bool */
static int l_plugin_playlist_delete(lua_State * L) {
    const char * m3u_path = luaL_checkstring(L, 1);
    if (!plugin_playlist_path_valid(m3u_path)) return plugin_lua_push_error(L, "invalid playlist path");
    bool ok = playlist_files_delete(m3u_path);
    if (ok) metadata_db_playlist_delete_one(m3u_path);
    lua_pushboolean(L, ok);
    return 1;
}

typedef struct {
    lua_State * L;
    int callback_ref;
    int64_t song_id;
    int size_px;
} plugin_album_cover_req_t;

static plugin_album_cover_req_t plugin_album_cover_queue[PLUGIN_ALBUM_COVER_QUEUE];

static void plugin_album_cover_drop(plugin_album_cover_req_t * req, bool unref) {
    if (unref) luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
    req->L = NULL;
    req->callback_ref = LUA_NOREF;
}

static void plugin_album_cover_drop_owner(lua_State * owner, bool aborted) {
    for (int i = 0; i < PLUGIN_ALBUM_COVER_QUEUE; i++) {
        plugin_album_cover_req_t * req = &plugin_album_cover_queue[i];
        if (req->L && req->L == owner) plugin_album_cover_drop(req, !aborted);
    }
}

static void plugin_album_cover_cancel_all(void) {
    for (int i = 0; i < PLUGIN_ALBUM_COVER_QUEUE; i++) {
        plugin_album_cover_req_t * req = &plugin_album_cover_queue[i];
        if (!req->L) continue;
        plugin_instance_t * inst = plugin_instance_for_state(req->L);
        plugin_album_cover_drop(req, !(inst && inst->aborted));
    }
}

static void plugin_album_cover_poll(void) {
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
        plugin_instance_t * inst = plugin_instance_for_state(L);
        if (inst && inst->aborted) continue;
        char path[PATH_MAX];
        bool found = gui_plugin_library_album_cover(song_id, size_px, path, sizeof(path));
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (found) {
            lua_pushstring(L, path);
            lua_pushnil(L);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "not_generated");
        }
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        if (plugin_call(L, 2, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(L, -1);
            fprintf(stderr, "[plugins] library_album_cover_async callback error: %s\n", err ? err : "unknown error");
            lua_pop(L, 1);
        }
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

/* ---- plugin.on(event, callback) -- see plugin_manager.h's own
 * PLUGIN_MAX_EVENT_SUBSCRIBERS comment for the design rationale (multiple
 * plugins can subscribe to the same event, unlike register_list_item()'s
 * "each plugin's row coexists as its own entry" model). Each event has its
 * own small subscriber array -- storage kept separate per event (not a
 * generic dispatch table keyed by string) for the same reason
 * l_plugin_register_list_item() gives each list_id its own array: this is a
 * known, fixed set rather than genericity built ahead of needing it. ---- */

typedef enum {
    PLUGIN_EVENT_TRACK_STARTED = 0,
    PLUGIN_EVENT_PAUSED,
    PLUGIN_EVENT_RESUMED,
    PLUGIN_EVENT_STOPPED,
    PLUGIN_EVENT_SCREEN_WOKE,
    PLUGIN_EVENT_QUEUE_EXHAUSTED,
    PLUGIN_EVENT_VOLUME_CHANGED,
    PLUGIN_EVENT_BATTERY_CHANGED,
    PLUGIN_EVENT_SUSPENDING,
    PLUGIN_EVENT_SYSTEM_RESUMED,
    PLUGIN_EVENT_SCREENSHOT_SAVED,
    PLUGIN_EVENT_SCREENSHOT_FAILED,
    PLUGIN_EVENT_OUTPUT_CHANGED,
    PLUGIN_EVENT_BUTTON_LOCK_CHANGED,
    PLUGIN_EVENT_REMOTE_STREAM_NEEDED,
    PLUGIN_EVENT_NETWORK_CHANGED,
    PLUGIN_EVENT_COUNT,
} plugin_event_t;

typedef struct {
    lua_State * L;
    int ref;
} plugin_event_subscriber_t;

static plugin_event_subscriber_t plugin_event_subscribers[PLUGIN_EVENT_COUNT][PLUGIN_MAX_EVENT_SUBSCRIBERS];
static int plugin_event_subscriber_count[PLUGIN_EVENT_COUNT];
static bool plugin_remote_resolver_registered(lua_State * L) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_REMOTE_STREAM_NEEDED]; i++)
        if (plugin_event_subscribers[PLUGIN_EVENT_REMOTE_STREAM_NEEDED][i].L == L) return true;
    return false;
}

/* UI/Lua-thread dispatch only. The audio thread queues ids, never Lua pointers
 * or callbacks; reload invalidates all ids before any state is closed. */
static void plugin_remote_stream_poll(void) {
    remote_stream_request_t request;
    for (int budget = 0; budget < 4 && remote_track_stream_take_request(&request); budget++) {
        lua_State * L = (lua_State *) request.owner;
        plugin_instance_t * inst = plugin_instance_for_state(L);
        if (!inst || inst->disposing || inst->aborted) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("The streaming plugin is not available"));
            continue;
        }
        int ref = LUA_NOREF;
        for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_REMOTE_STREAM_NEEDED]; i++) {
            plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_REMOTE_STREAM_NEEDED][i];
            if (sub->L == L) { ref = sub->ref; break; }
        }
        if (ref == LUA_NOREF) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("The streaming plugin is not available"));
            continue;
        }
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushstring(L, request.provider);
        lua_pushstring(L, request.track_id);
        lua_pushinteger(L, (lua_Integer) request.request_id);
        if (plugin_call(L, 3, 0, 0) != LUA_OK) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("Could not get a stream link"));
        }
        if (!inst->aborted) lua_settop(L, top);
    }
}

static int plugin_last_volume_percent = -1;
static bool plugin_battery_snapshot_valid = false;
static plugin_battery_info_t plugin_last_battery_info;
static bool plugin_output_snapshot_valid;
typedef struct {
    int route, requested_route, headphones;
    bool active, bluetooth;
    char bluetooth_codec[32];
} plugin_output_snapshot_t;
static plugin_output_snapshot_t plugin_last_output_snapshot;

static bool plugin_output_snapshot(plugin_output_snapshot_t * s) {
    audio_output_info_t out = {0};
#ifndef HOST_BUILD
    audio_output_get_info(&out);
#else
    out.route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.requested_route = AUDIO_OUTPUT_ROUTE_WIRED;
#endif
    s->route = out.active ? (int)out.route : -1;
    s->requested_route = (int)out.requested_route;
    s->active = out.active;
    s->bluetooth = gui_shell_is_bt_audio_connected();
    (void)gui_shell_get_bt_audio_codec(s->bluetooth_codec, sizeof(s->bluetooth_codec));
    s->headphones = (int)get_headphone_state();
    return true;
}

static void push_output_snapshot(lua_State * L, const plugin_output_snapshot_t * s) {
    static const char * const routes[] = { "wired", "bluetooth", "usb_dac" };
    lua_newtable(L);
    lua_pushstring(L, s->route >= 0 && s->route < 3 ? routes[s->route] : "inactive"); lua_setfield(L, -2, "route");
    lua_pushstring(L, s->requested_route >= 0 && s->requested_route < 3 ? routes[s->requested_route] : "unknown"); lua_setfield(L, -2, "requested_route");
    lua_pushboolean(L, s->active); lua_setfield(L, -2, "active");
    lua_pushboolean(L, s->bluetooth); lua_setfield(L, -2, "bluetooth_connected");
    if (s->bluetooth_codec[0]) lua_pushstring(L, s->bluetooth_codec);
    else lua_pushnil(L);
    lua_setfield(L, -2, "bluetooth_codec");
    lua_pushinteger(L, s->headphones); lua_setfield(L, -2, "headphone_state");
}

/* Field by field: struct assignment need not copy padding, so memcmp could
 * report a change that never happened. */
static bool plugin_output_snapshot_equal(const plugin_output_snapshot_t * a,
                                         const plugin_output_snapshot_t * b) {
    return a->route == b->route && a->requested_route == b->requested_route &&
           a->headphones == b->headphones && a->active == b->active &&
           a->bluetooth == b->bluetooth &&
           strcmp(a->bluetooth_codec, b->bluetooth_codec) == 0;
}

static void plugin_output_poll(void) {
    if (plugin_event_subscriber_count[PLUGIN_EVENT_OUTPUT_CHANGED] == 0) return;
    plugin_output_snapshot_t now = {0};
    if (!plugin_output_snapshot(&now)) return;
    if (!plugin_output_snapshot_valid) {
        plugin_last_output_snapshot = now;
        plugin_output_snapshot_valid = true;
        return;
    }
    if (plugin_output_snapshot_equal(&now, &plugin_last_output_snapshot)) return;
    plugin_output_snapshot_t previous = plugin_last_output_snapshot;
    plugin_last_output_snapshot = now;
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_OUTPUT_CHANGED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_OUTPUT_CHANGED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        push_output_snapshot(sub->L, &now);
        push_output_snapshot(sub->L, &previous);
        if (plugin_call(sub->L, 2, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] output_changed handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

static int l_plugin_on(lua_State * L) {
    const char * event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    plugin_event_t idx;
    if (strcmp(event, "track_started") == 0) idx = PLUGIN_EVENT_TRACK_STARTED;
    else if (strcmp(event, "paused") == 0) idx = PLUGIN_EVENT_PAUSED;
    else if (strcmp(event, "resumed") == 0) idx = PLUGIN_EVENT_RESUMED;
    else if (strcmp(event, "stopped") == 0) idx = PLUGIN_EVENT_STOPPED;
    else if (strcmp(event, "screen_woke") == 0) idx = PLUGIN_EVENT_SCREEN_WOKE;
    else if (strcmp(event, "queue_exhausted") == 0) idx = PLUGIN_EVENT_QUEUE_EXHAUSTED;
    else if (strcmp(event, "volume_changed") == 0) idx = PLUGIN_EVENT_VOLUME_CHANGED;
    else if (strcmp(event, "battery_changed") == 0) idx = PLUGIN_EVENT_BATTERY_CHANGED;
    else if (strcmp(event, "suspending") == 0) idx = PLUGIN_EVENT_SUSPENDING;
    else if (strcmp(event, "system_resumed") == 0) idx = PLUGIN_EVENT_SYSTEM_RESUMED;
    else if (strcmp(event, "screenshot_saved") == 0) idx = PLUGIN_EVENT_SCREENSHOT_SAVED;
    else if (strcmp(event, "screenshot_failed") == 0) idx = PLUGIN_EVENT_SCREENSHOT_FAILED;
    else if (strcmp(event, "output_changed") == 0) idx = PLUGIN_EVENT_OUTPUT_CHANGED;
    else if (strcmp(event, "button_lock_changed") == 0) idx = PLUGIN_EVENT_BUTTON_LOCK_CHANGED;
    else if (strcmp(event, "remote_stream_needed") == 0) idx = PLUGIN_EVENT_REMOTE_STREAM_NEEDED;
    else if (strcmp(event, "network_changed") == 0) idx = PLUGIN_EVENT_NETWORK_CHANGED;
    else return luaL_error(L, "plugin.on: unknown event '%s'", event);

    if (idx == PLUGIN_EVENT_REMOTE_STREAM_NEEDED && plugin_remote_resolver_registered(plugin_lua_main_state(L)))
        return luaL_error(L, "plugin.on: only one remote_stream_needed resolver per plugin is allowed");

    if (plugin_event_subscriber_count[idx] >= PLUGIN_MAX_EVENT_SUBSCRIBERS) {
        return luaL_error(L, "plugin.on: too many subscribers registered for \"%s\" (max %d)", event,
                           PLUGIN_MAX_EVENT_SUBSCRIBERS);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][plugin_event_subscriber_count[idx]++];
    sub->L = plugin_lua_main_state(L);
    sub->ref = ref;
    if (idx == PLUGIN_EVENT_REMOTE_STREAM_NEEDED) {
        /* A reloaded plugin takes over the queue its previous instance built. */
        plugin_instance_t * inst = plugin_instance_for_state(sub->L);
        if (inst && inst->defined) remote_track_stream_adopt_owner(inst->id, (uintptr_t) sub->L);
    }
    return 0;
}

/* ---- plugin.set_interval(seconds, callback) -> handle /
 * plugin.clear_interval(handle) -- see plugin_manager.h's own
 * PLUGIN_MAX_INTERVALS/PLUGIN_INTERVAL_MIN_MS comments. handle is the pool
 * slot + 1 (0 reserved as "invalid"/never returned). ---- */

typedef struct {
    lua_State * L;
    int ref;
    bool active;
} plugin_interval_t;

static plugin_interval_t plugin_intervals[PLUGIN_MAX_INTERVALS];

static int l_plugin_set_interval(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return luaL_error(L, "plugin.set_interval: too many active intervals (max %d)", PLUGIN_MAX_INTERVALS);
    }

    uint32_t period_ms = (uint32_t) (seconds * 1000.0);
    if (period_ms < PLUGIN_INTERVAL_MIN_MS) period_ms = PLUGIN_INTERVAL_MIN_MS;

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_intervals[slot].L = plugin_lua_main_state(L);
    plugin_intervals[slot].ref = ref;
    plugin_intervals[slot].active = true;

    gui_plugin_set_interval(slot, period_ms);
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int l_plugin_clear_interval(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    int slot = (int) handle - 1;
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return 0;

    luaL_unref(plugin_intervals[slot].L, LUA_REGISTRYINDEX, plugin_intervals[slot].ref);
    plugin_intervals[slot].active = false;
    plugin_intervals[slot].L = NULL;
    gui_plugin_clear_interval(slot);
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

static lua_State * button_lock_owner = NULL;

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

static bool plugin_id_is_valid(const char * id) {
    return plugin_id_charset_ok(id);
}

/* Checks whether a plugin ID collides with any already-loaded plugin instance. */
static bool plugin_id_collides(const char * id, int exclude_slot) {
    for (int i = 0; i < plugin_instance_count; i++) {
        if (i != exclude_slot && plugin_instances[i].defined && strcmp(plugin_instances[i].id, id) == 0) return true;
    }
    return false;
}

/* plugin.define({ id=..., name=..., version=..., api_min=... }) establishes
 * stable identity before future storage/permission APIs are added. It is
 * deliberately legal only while this file is executing its top-level code:
 * identity cannot change after callbacks and resources have been registered. */
static int l_plugin_define(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (loading_plugin_slot < 0 || loading_plugin_slot >= PLUGIN_MAX_FILES ||
        plugin_instances[loading_plugin_slot].L != L) {
        return luaL_error(L, "plugin.define may only be called once during plugin loading");
    }
    plugin_instance_t * inst = &plugin_instances[loading_plugin_slot];
    if (inst->defined) return luaL_error(L, "plugin.define may only be called once");

    lua_getfield(L, 1, "id");
    const char * id = luaL_checkstring(L, -1);
    if (!plugin_id_is_valid(id) || strlen(id) >= sizeof(inst->id)) {
        return luaL_error(L, "plugin.define: id must be 1-%zu characters using letters, digits, '.', '_' or '-'",
                          sizeof(inst->id) - 1);
    }
    if (gui_themes_is_legacy_theme_loader(id, inst->filename)) {
        return luaL_error(L, "plugin.define: plugin id '%s' is a legacy theme loader and cannot be registered", id);
    }
    if (plugin_id_collides(id, loading_plugin_slot)) {
        return luaL_error(L, "plugin.define: duplicate plugin id '%s'", id);
    }
    snprintf(inst->id, sizeof(inst->id), "%s", id);
    lua_pop(L, 1);

    lua_getfield(L, 1, "name");
    const char * name = luaL_optstring(L, -1, id);
    snprintf(inst->name, sizeof(inst->name), "%s", name);
    lua_pop(L, 1);
    lua_getfield(L, 1, "version");
    const char * version = luaL_optstring(L, -1, "0");
    snprintf(inst->version, sizeof(inst->version), "%s", version);
    lua_pop(L, 1);
    lua_getfield(L, 1, "api_min");
    lua_Integer api_min = luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    if (api_min > PLUGIN_API_VERSION) {
        return luaL_error(L, "plugin '%s' requires API %lld, player provides API %d", id,
                          (long long) api_min, PLUGIN_API_VERSION);
    }
    inst->defined = true;
    return 0;
}


static led_control_color_t check_led_color(lua_State * L, const char * api) {
    const char * color = luaL_checkstring(L, 1);
    if (strcmp(color, "red") == 0) return LED_CONTROL_RED;
    if (strcmp(color, "blue") == 0) return LED_CONTROL_BLUE;
    luaL_error(L, "%s: color must be 'red' or 'blue'", api);
    return LED_CONTROL_RED; /* unreachable -- luaL_error() longjmps */
}

static int check_led_level(lua_State * L, int arg, int default_level, const char * api) {
    (void) api;
    if (lua_gettop(L) < arg || lua_isnil(L, arg)) return default_level;
    lua_Integer level = luaL_checkinteger(L, arg);
    if (level < 0) return 0;
    if (level > 100) return 100;
    return (int) level;
}

static int check_led_interval(lua_State * L, int arg, const char * api) {
    lua_Integer value = luaL_checkinteger(L, arg);
    if (value < 50) return 50;
    if (value > 60000) return 60000;
    (void) api;
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
        level = check_led_level(L, 2, 0, "plugin.led_set");
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
    int on_ms = check_led_interval(L, 2, "plugin.led_blink");
    int off_ms = check_led_interval(L, 3, "plugin.led_blink");
    int level = check_led_level(L, 4, 100, "plugin.led_blink");
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
    int level = check_led_level(L, 3, 100, "plugin.led_breathe");
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
static bool plugin_text_view_owner_is_aborted(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    return inst && inst->aborted;
}

static int l_plugin_show_text_view(lua_State * L) {
    return plugin_text_view_bindings_show(L, plugin_call, plugin_text_view_owner_is_aborted);
}

static int l_plugin_network_status(lua_State * L);

static const luaL_Reg plugin_funcs[] = {
    { "set_music_layout",          l_plugin_set_music_layout },
    { "set_quick_drawer_layout",   l_plugin_set_quick_drawer_layout },
    { "define",                    l_plugin_define },
    { "api_version",               l_plugin_api_version },
    { "network_status",            l_plugin_network_status },
    { "has_capability",            l_plugin_has_capability },
    { "get_app_info",              l_plugin_get_app_info },
    { "led_available",             l_plugin_led_available },
    { "led_set",                   l_plugin_led_set },
    { "led_blink",                 l_plugin_led_blink },
    { "led_breathe",               l_plugin_led_breathe },
    { "led_get",                   l_plugin_led_get },
    { "led_status",                l_plugin_led_status },
    { "led_release",               l_plugin_led_release },
    { "register_list_item",        l_plugin_register_list_item },
    { "register_stream_media_tile", l_plugin_register_stream_media_tile },
    { "register_home_tile",        l_plugin_register_home_tile },
    { "register_quick_toggle",     l_plugin_register_quick_toggle },
    { "set_quick_toggle",          l_plugin_set_quick_toggle },
    { "show_list",                 l_plugin_show_list },
    { "is_list_showing",           l_plugin_is_list_showing },
    { "show_settings_list",        l_plugin_show_settings_list },
    { "show_order_editor",         l_plugin_show_order_editor },
    { "list_dir",                  l_plugin_list_dir },
    { "sd_root",                   l_plugin_sd_root },
    { "realpath",                  l_plugin_realpath },
    { "playlist_list",             l_plugin_playlist_list },
    { "playlist_read",             l_plugin_playlist_read },
    { "playlist_create",           l_plugin_playlist_create },
    { "playlist_add",              l_plugin_playlist_add },
    { "playlist_remove",           l_plugin_playlist_remove },
    { "playlist_delete",           l_plugin_playlist_delete },
    { "mkdir",                     l_plugin_mkdir },
    { "file_copy",                 l_plugin_file_copy },
    { "file_move",                 l_plugin_file_move },
    { "file_delete",               l_plugin_file_delete },
    { "file_operation_status",     l_plugin_file_operation_status },
    { "cancel_file_operation",     l_plugin_cancel_file_operation },
    { "open_file_manager",         l_plugin_open_file_manager },
    { "pick_file",                 l_plugin_pick_file },
    { "play_file",                 l_plugin_play_file },
    { "play_list",                 l_plugin_play_list },
    { "play_remote",               l_plugin_play_remote },
    { "queue_remote_list",         l_plugin_queue_remote_list },
    { "provide_remote_stream",     l_plugin_provide_remote_stream },
    { "show_toast",                l_plugin_show_toast },
    { "show_progress",             l_plugin_show_progress },
    { "update_progress",           l_plugin_update_progress },
    { "close_progress",            l_plugin_close_progress },
    { "screenshot",                l_plugin_screenshot },
    { "set_icon",                  l_plugin_set_icon },
    { "set_background_color",      l_plugin_set_background_color },
    { "set_text_color",            l_plugin_set_text_color },
    { "set_home_layout",           l_plugin_set_home_layout },
    { "set_launcher_layout",       l_plugin_set_launcher_layout },
    { "set_player_layout",         l_plugin_set_player_layout },
    { "restore_player_layout",     l_plugin_restore_player_layout },
    { "register_player_action",    l_plugin_register_player_action },
    { "refresh_theme",             l_plugin_refresh_theme },
    { "reload_ui",                 l_plugin_reload_ui },
    { "eq_load_profile",           l_plugin_eq_load_profile },
    { "eq_apply_profile",          l_plugin_eq_apply_profile },
    { "eq_save_profile",           l_plugin_eq_save_profile },
    { "eq_reset",                  l_plugin_eq_reset },
    { "eq_set_bypass",             l_plugin_eq_set_bypass },
    { "eq_set_preamp",             l_plugin_eq_set_preamp },
    { "eq_get_stereo_width",       l_plugin_eq_get_stereo_width },
    { "eq_set_stereo_width",       l_plugin_eq_set_stereo_width },
    { "eq_get_crossfeed",          l_plugin_eq_get_crossfeed },
    { "eq_set_crossfeed",          l_plugin_eq_set_crossfeed },
    { "eq_set_band",               l_plugin_eq_set_band },
    { "eq_set_band_type",          l_plugin_eq_set_band_type },
    { "eq_set_band_enabled",       l_plugin_eq_set_band_enabled },
    { "get_eq_state",              l_plugin_get_eq_state },
    { "eq_apply_state",             l_plugin_eq_apply_state },
    { "mseb_apply_state",            l_plugin_mseb_apply_state },
    { "get_mseb_state",              l_plugin_get_mseb_state },
    { "get_track_metadata",         l_plugin_get_track_metadata },
    { "set_hw_volume_curve",       l_plugin_set_hw_volume_curve },
    { "toggle_pause",              l_plugin_toggle_pause },
    { "stop",                      l_plugin_stop },
    { "next_track",                l_plugin_next_track },
    { "prev_track",                l_plugin_prev_track },
    { "seek",                      l_plugin_seek },
    { "set_playback_speed",        l_plugin_set_playback_speed },
    { "get_playback_speed",        l_plugin_get_playback_speed },
    { "set_transport_skip",        l_plugin_set_transport_skip },
    { "set_volume",                l_plugin_set_volume },
    { "set_ab_loop",               l_plugin_set_ab_loop },
    { "clear_ab_loop",             l_plugin_clear_ab_loop },
    { "get_ab_loop",               l_plugin_get_ab_loop },
    { "audio_loop_start",          l_plugin_audio_loop_start },
    { "audio_loop_stop",           l_plugin_audio_loop_stop },
    { "audio_loop_active",         l_plugin_audio_loop_active },
    { "prepare_ab_switch",         l_plugin_prepare_ab_switch },
    { "select_ab_source",          l_plugin_select_ab_source },
    { "get_ab_switch",             l_plugin_get_ab_switch },
    { "clear_ab_switch",           l_plugin_clear_ab_switch },
    { "get_playback_format",       l_plugin_get_playback_format },
    { "get_output_info",           l_plugin_get_output_info },
    { "get_volume",                l_plugin_get_volume },
    { "get_battery",               l_plugin_get_battery },
    { "is_playing",                l_plugin_is_playing },
    { "is_paused",                 l_plugin_is_paused },
    { "get_position",              l_plugin_get_position },
    { "get_duration",              l_plugin_get_duration },
    { "get_playback_progress",     l_plugin_get_playback_progress },
    { "http_get",                  l_plugin_http_get },
    { "http_post",                 l_plugin_http_post },
    { "http_request",              l_plugin_http_request },
    { "download_file_async",       l_plugin_download_file_async },
    { "get_download_progress",     l_plugin_get_download_progress },
    { "cancel",                    l_plugin_cancel },
    { "md5",                       l_plugin_md5 },
    { "json_decode",               l_plugin_json_decode },
    { "json_encode",               l_plugin_json_encode },
    { "media_capabilities",        l_plugin_media_capabilities },
    { "show_text_input",           l_plugin_show_text_input },
    { "show_confirmation",         l_plugin_show_confirmation },
    { "get_now_playing",           l_plugin_get_now_playing },
    { "get_play_mode",             l_plugin_get_play_mode },
    { "set_play_mode",             l_plugin_set_play_mode },
    { "get_crossfade",             l_plugin_get_crossfade },
    { "set_crossfade",             l_plugin_set_crossfade },
    { "get_gapless",               l_plugin_get_gapless },
    { "set_gapless",               l_plugin_set_gapless },
    { "get_replaygain_mode",       l_plugin_get_replaygain_mode },
    { "set_replaygain_mode",       l_plugin_set_replaygain_mode },
    { "get_current_track_path",    l_plugin_get_current_track_path },
    { "refresh_lyrics",            l_plugin_refresh_lyrics },
    { "get_artist_albums",         l_plugin_get_artist_albums },
    { "get_album_tracks",          l_plugin_get_album_tracks },
    { "get_next_album_tracks",     l_plugin_get_next_album_tracks },
    { "library_song_count",        l_plugin_library_song_count },
    { "library_get_songs",         l_plugin_library_get_songs },
    { "library_search",            l_plugin_library_search },
    { "library_get_song",          l_plugin_library_get_song },
    { "library_get_artists",       l_plugin_library_get_artists },
    { "library_get_albums",        l_plugin_library_get_albums },
    { "library_album_cover_async", l_plugin_library_album_cover_async },
    { "refresh_library",           l_plugin_refresh_library },
    { "on",                        l_plugin_on },
    { "set_interval",              l_plugin_set_interval },
    { "clear_interval",            l_plugin_clear_interval },
    { "show_lock_screen",          l_plugin_show_lock_screen },
    { "configure_button_lock",     l_plugin_configure_button_lock },
    { "set_startup_image_async",   l_plugin_set_startup_image_async },
    { "has_startup_image",         l_plugin_has_startup_image },
    { "list_apps",                 l_plugin_list_apps },
    { "launch_app",                l_plugin_launch_app },
    { "zip_read",                  l_plugin_zip_read },
    { "zip_list",                  l_plugin_zip_list },
    { "zip_image_async",           l_plugin_zip_image_async },
    { "image_thumbnail_async",     l_plugin_image_thumbnail_async },
    { "image_wallpaper_async",     l_plugin_image_wallpaper_async },
    { "image_startup_async",       l_plugin_image_startup_async },
    { "html_to_blocks",            l_plugin_html_to_blocks },
    { "show_text_view",            l_plugin_show_text_view },
    { NULL, NULL }
};

static const luaL_Reg plugin_storage_funcs[] = {
    { "get",    l_plugin_storage_get },
    { "set",    l_plugin_storage_set },
    { "delete", l_plugin_storage_delete },
    { "list",   l_plugin_storage_list },
    { NULL, NULL }
};

static const luaL_Reg plugin_secrets_funcs[] = {
    { "get",    l_plugin_secrets_get },
    { "set",    l_plugin_secrets_set },
    { "exists", l_plugin_secrets_exists },
    { "delete", l_plugin_secrets_delete },
    { NULL, NULL }
};

#define PLUGIN_CALL_MAX_MS 2000
/* One second of grace past the soft budget. A well-behaved plugin sees the
 * catchable luaL_error at PLUGIN_CALL_MAX_MS and unwinds; a plugin that keeps
 * running past this (e.g. by wrapping its loop body in its own pcall() and
 * swallowing that error) is hard-aborted via longjmp so it cannot pin the UI
 * thread indefinitely. */
#define PLUGIN_CALL_HARD_MS 3000

static struct timespec plugin_call_deadline_start;
static jmp_buf plugin_call_abort_jmp;
static volatile int plugin_call_abort_armed = 0;

/* Adds time spent executing inside native C functions to the deadline start. */
static void plugin_call_exclude_native_elapsed(const struct timespec * started) {
    struct timespec ended;
    clock_gettime(CLOCK_MONOTONIC, &ended);
    time_t sec = ended.tv_sec - started->tv_sec;
    long nsec = ended.tv_nsec - started->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    plugin_call_deadline_start.tv_sec += sec;
    plugin_call_deadline_start.tv_nsec += nsec;
    if (plugin_call_deadline_start.tv_nsec >= 1000000000L) {
        plugin_call_deadline_start.tv_sec++;
        plugin_call_deadline_start.tv_nsec -= 1000000000L;
    }
}

/* Invoke a plain native Lua function through a protected boundary so its
 * elapsed time is excluded on both success and luaL_error(). */
static int plugin_call_native(lua_State * L, lua_CFunction fn) {
    int nargs = lua_gettop(L);
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    lua_pushcfunction(L, fn);
    lua_insert(L, 1);
    int rc = lua_pcall(L, nargs, LUA_MULTRET, 0);
    plugin_call_exclude_native_elapsed(&started);
    if (rc != LUA_OK) return lua_error(L);
    return lua_gettop(L);
}

/* Wraps one plugin.* C function so native elapsed time is excluded even
 * when the inner function luaL_error()s (lua_pcall here catches that,
 * accounts, then rethrows). Without this, a long set_icon/http_get would
 * return to Lua with the original plugin_call() deadline already expired
 * and the next instruction-count hook would abort the whole plugin --
 * Themes.lua copies ~140 icons at load via set_icon(), so that abort
 * looked like "the Themes plugin didn't load" after a flash wiped
 * /usr/data/theme_overrides/. */
static int l_plugin_api_guard(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(plugin_lua_main_state(L));
    if (inst && inst->disposing)
        return luaL_error(L, "plugin API is unavailable while a plugin is closing");
    return plugin_call_native(L, lua_tocfunction(L, lua_upvalueindex(1)));
}

static void register_plugin_lib(lua_State * L, const luaL_Reg * regs) {
    lua_newtable(L);
    for (; regs->name != NULL; regs++) {
        lua_pushcfunction(L, regs->func);
        lua_pushcclosure(L, l_plugin_api_guard, 1);
        lua_setfield(L, -2, regs->name);
    }
}

static int plugin_now_playing_set_value(lua_State * L, bool image) {
    const char * api = image ? "plugin.now_playing.set_image" : "plugin.now_playing.set_text";
    plugin_instance_t * inst = plugin_instance_for_state(plugin_lua_main_state(L));
    if (!inst || !inst->defined || inst->disposing || inst->aborted)
        return luaL_error(L, "%s requires an active plugin.define({id=...})", api);
    size_t name_len;
    const char * name = luaL_checklstring(L, 1, &name_len);
    if (strlen(name) != name_len) return luaL_error(L, "%s: invalid widget name", api);
    const char * value = NULL;
    char source[515];
    if (!lua_isnoneornil(L, 2)) {
        size_t len;
        value = luaL_checklstring(L, 2, &len);
        if (strlen(value) != len || len > (image ? 511u : 4095u))
            return luaL_error(L, "%s: invalid or oversized value", api);
        if (image) {
            char path[512];
            int n = value[0] == '/' ? snprintf(path, sizeof(path), "%s", value) :
                snprintf(path, sizeof(path), "%s/.plugins/%s", MUSIC_ROOT_DIR, value);
            if (!value[0] || n < 0 || (size_t)n >= sizeof(path))
                return luaL_error(L, "%s: invalid or oversized image path", api);
            lua_pushstring(L, path);
            check_plugin_external_path(L, -1, api);
            lua_pop(L, 1);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
                return luaL_error(L, "%s: image file does not exist", api);
            snprintf(source, sizeof(source), "S:%s", path);
            lv_image_header_t header;
            if (lv_image_decoder_get_info(source, &header) != LV_RESULT_OK)
                return luaL_error(L, "%s: unsupported image file", api);
            value = source;
        }
    }
    const char * error = player_layouts_set_plugin_value(inst->id, name, value, image);
    if (error) return luaL_error(L, "%s: %s", api, error);
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_now_playing_set_image(lua_State * L) {
    return plugin_now_playing_set_value(L, true);
}

static int l_plugin_now_playing_set_text(lua_State * L) {
    return plugin_now_playing_set_value(L, false);
}

static const luaL_Reg plugin_now_playing_funcs[] = {
    { "set_image", l_plugin_now_playing_set_image },
    { "set_text", l_plugin_now_playing_set_text },
    { NULL, NULL }
};

static void register_plugin_api(lua_State * L) {
    register_plugin_lib(L, plugin_funcs);
    register_plugin_lib(L, plugin_now_playing_funcs);
    lua_setfield(L, -2, "now_playing");
    register_plugin_lib(L, plugin_storage_funcs);
    lua_setfield(L, -2, "storage");
    register_plugin_lib(L, plugin_secrets_funcs);
    lua_setfield(L, -2, "secrets");
    lua_setglobal(L, "plugin");
}

/* Sandboxes the Lua state by removing shell execution (os.execute, io.popen),
 * dynamic code loading (load, dofile, require), and debug library access.
 * Retains basic file I/O operations needed by plugins. */

/* Wrappers preventing direct access to reserved plugin storage paths
 * (plugin_storage_path_is_reserved) via standard io and os library functions. */
static lua_CFunction real_io_open = NULL;
static lua_CFunction real_io_lines = NULL;
static lua_CFunction real_io_input = NULL;
static lua_CFunction real_io_output = NULL;
static lua_CFunction real_os_remove = NULL;
static lua_CFunction real_os_rename = NULL;

static int l_guarded_io_open(lua_State * L) {
    const char * path = luaL_optstring(L, 1, "");
    if (plugin_storage_path_is_reserved(path)) {
        return plugin_lua_push_error(L, "io.open: path is reserved for plugin.storage/plugin.secrets");
    }
    /* A mode other than plain read ("r"/"rb") can create or change the file. */
    const char * mode = luaL_optstring(L, 2, "r");
    if ((mode[0] != 'r' || strchr(mode, '+')) && plugin_path_in_program_dirs(path)) {
        return plugin_lua_push_error(L, "io.open: the program folders (.apps, .apps-data, .emulators) are read-only");
    }
    return plugin_call_native(L, real_io_open);
}

static int l_guarded_io_lines(lua_State * L) {
    /* io.lines(), with no argument, reads the default input -- not a path,
     * nothing to check. Unlike io.open(), a real io.lines() failure
     * raises a Lua error rather than returning nil+message (per the Lua
     * manual), so this mirrors that convention instead of io.open()'s. */
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        const char * path = lua_tostring(L, 1);
        if (plugin_storage_path_is_reserved(path)) {
            return luaL_error(L, "io.lines: path is reserved for plugin.storage/plugin.secrets");
        }
    }
    return plugin_call_native(L, real_io_lines);
}

static int l_guarded_io_input(lua_State * L) {
    /* Only a STRING first argument names a path to open -- io.input() with
     * no argument returns the current default input, and io.input(handle)
     * with an already-open file handle just re-points the default input
     * at it, neither of which is a path to check. Like io.lines(), a real
     * io.input() failure raises a Lua error rather than returning
     * nil+message. */
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        const char * path = lua_tostring(L, 1);
        if (plugin_storage_path_is_reserved(path)) {
            return luaL_error(L, "io.input: path is reserved for plugin.storage/plugin.secrets");
        }
    }
    return plugin_call_native(L, real_io_input);
}

static int l_guarded_io_output(lua_State * L) {
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        const char * path = lua_tostring(L, 1);
        if (plugin_storage_path_is_reserved(path)) {
            return luaL_error(L, "io.output: path is reserved for plugin.storage/plugin.secrets");
        }
        if (plugin_path_in_program_dirs(path)) {
            return luaL_error(L, "io.output: the program folders (.apps, .apps-data, .emulators) are read-only");
        }
    }
    return plugin_call_native(L, real_io_output);
}

static int l_guarded_os_remove(lua_State * L) {
    const char * path = luaL_optstring(L, 1, "");
    if (plugin_storage_path_is_reserved(path)) {
        return plugin_lua_push_error(L, "os.remove: path is reserved for plugin.storage/plugin.secrets");
    }
    if (plugin_path_in_program_dirs(path)) {
        return plugin_lua_push_error(L, "os.remove: the program folders (.apps, .apps-data, .emulators) are read-only");
    }
    return plugin_call_native(L, real_os_remove);
}

static int l_guarded_os_rename(lua_State * L) {
    const char * from = luaL_optstring(L, 1, "");
    const char * to = luaL_optstring(L, 2, "");
    if (plugin_storage_path_is_reserved(from) || plugin_storage_path_is_reserved(to)) {
        return plugin_lua_push_error(L, "os.rename: path is reserved for plugin.storage/plugin.secrets");
    }
    if (plugin_path_in_program_dirs(from) || plugin_path_in_program_dirs(to)) {
        return plugin_lua_push_error(L, "os.rename: the program folders (.apps, .apps-data, .emulators) are read-only");
    }
    return plugin_call_native(L, real_os_rename);
}

static void sandbox_plugin_lua_state(lua_State * L) {
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "loadstring");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "require");
    lua_pushnil(L); lua_setglobal(L, "package");
    lua_pushnil(L); lua_setglobal(L, "debug");

    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        static const char * dangerous_os[] = { "execute", "getenv", "exit", "tmpname" };
        for (size_t i = 0; i < sizeof(dangerous_os) / sizeof(dangerous_os[0]); i++) {
            lua_pushnil(L);
            lua_setfield(L, -2, dangerous_os[i]);
        }

        lua_getfield(L, -1, "remove");
        if (lua_iscfunction(L, -1)) real_os_remove = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "rename");
        if (lua_iscfunction(L, -1)) real_os_rename = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        if (real_os_remove) { lua_pushcfunction(L, l_guarded_os_remove); lua_setfield(L, -2, "remove"); }
        if (real_os_rename) { lua_pushcfunction(L, l_guarded_os_rename); lua_setfield(L, -2, "rename"); }
    }
    lua_pop(L, 1);

    lua_getglobal(L, "io");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "popen"); /* the one io.* function that's a shell-exec primitive, not file I/O */

        lua_getfield(L, -1, "open");
        if (lua_iscfunction(L, -1)) real_io_open = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "lines");
        if (lua_iscfunction(L, -1)) real_io_lines = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "input");
        if (lua_iscfunction(L, -1)) real_io_input = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "output");
        if (lua_iscfunction(L, -1)) real_io_output = lua_tocfunction(L, -1);
        lua_pop(L, 1);
        if (real_io_open) { lua_pushcfunction(L, l_guarded_io_open); lua_setfield(L, -2, "open"); }
        if (real_io_lines) { lua_pushcfunction(L, l_guarded_io_lines); lua_setfield(L, -2, "lines"); }
        if (real_io_input) { lua_pushcfunction(L, l_guarded_io_input); lua_setfield(L, -2, "input"); }
        if (real_io_output) { lua_pushcfunction(L, l_guarded_io_output); lua_setfield(L, -2, "output"); }
    }
    lua_pop(L, 1);
}

static void plugin_call_timeout_hook(lua_State * L, lua_Debug * ar) {
    (void) ar;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - plugin_call_deadline_start.tv_sec) * 1000L +
                      (now.tv_nsec - plugin_call_deadline_start.tv_nsec) / 1000000L;
    if (elapsed_ms > PLUGIN_CALL_HARD_MS && plugin_call_abort_armed) {
        /* Jump straight out to plugin_call(), past any pcall the plugin itself
         * installed. The catchable luaL_error below is not enough on its own:
         * a plugin can wrap its work in pcall() and swallow it, so without this
         * a runaway loop would keep the UI thread busy forever. The VM is at an
         * instruction boundary here, so its stack is consistent for the
         * cleanup plugin_call() does after catching this. */
        plugin_call_abort_armed = 0;
        longjmp(plugin_call_abort_jmp, 1);
    }
    if (elapsed_ms > PLUGIN_CALL_MAX_MS) {
        luaL_error(L, "plugin call exceeded %dms time budget -- aborted to keep the UI responsive", PLUGIN_CALL_MAX_MS);
    }
}

/* Executes a Lua protected call with a wall-clock timeout enforced by an
 * instruction count hook. Time spent in native C functions is added to the
 * deadline to avoid penalizing I/O. A plugin that blows past the hard budget
 * (PLUGIN_CALL_HARD_MS) is longjmp-aborted from the hook, bypassing any pcall
 * it installed, and permanently disabled -- its lua_State is left frozen and
 * never run again. */
static int plugin_call(lua_State * L, int nargs, int nresults, int errfunc) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    if (inst && inst->aborted) {
        lua_pushliteral(L, "plugin disabled after exceeding its time budget");
        return LUA_ERRRUN;
    }

    /* Save/restore so a nested plugin_call (should one ever exist) can't lose
     * the outer call's abort target or deadline. */
    struct timespec saved_deadline = plugin_call_deadline_start;
    int saved_armed = plugin_call_abort_armed;
    lua_Hook saved_hook = lua_gethook(L);
    int saved_hook_mask = lua_gethookmask(L);
    int saved_hook_count = lua_gethookcount(L);
    jmp_buf saved_jmp;
    memcpy(saved_jmp, plugin_call_abort_jmp, sizeof(jmp_buf));

    clock_gettime(CLOCK_MONOTONIC, &plugin_call_deadline_start);
    lua_sethook(L, plugin_call_timeout_hook, LUA_MASKCOUNT, 10000);

    int result;
    if (setjmp(plugin_call_abort_jmp) == 0) {
        plugin_call_abort_armed = 1;
        result = lua_pcall(L, nargs, nresults, errfunc);
    } else {
        /* Hard-aborted from the hook. L is at a VM instruction boundary, so its
         * stack pointers are consistent: reset the stack and leave a readable
         * error for the caller's usual lua_tostring()/lua_pop(). Mark the
         * plugin dead and never call back into this state -- it is deliberately
         * not lua_close()d (a half-unwound VM), just frozen and leaked, the
         * same lifetime every other plugin state already has in this file. */
        if (inst) {
            inst->aborted = true;
            remote_track_stream_cancel_owner((uintptr_t) L);
        }
        lua_settop(L, 0);
        lua_pushliteral(L, "plugin call hard-aborted after exceeding its time budget");
        result = LUA_ERRRUN;
    }

    /* Restores the enclosing call's hook when nested; otherwise clears it,
     * so there is no cost between plugin calls. */
    lua_sethook(L, saved_hook, saved_hook_mask, saved_hook_count);
    plugin_call_abort_armed = saved_armed;
    memcpy(plugin_call_abort_jmp, saved_jmp, sizeof(jmp_buf));
    plugin_call_deadline_start = saved_deadline;
    if (result != LUA_OK) release_plugin_led_override(L);
    return result;
}

/* True only while a callback the user triggered by tapping (a list row or
 * menu entry, a tile, a confirmation) is running; plugin.launch_app() needs
 * it. Timers, events and network callbacks never set it. */
static lua_State * plugin_user_action_owner;
static bool plugin_user_action_used;

static int plugin_call_user_action(lua_State * L, int nargs, int nresults, int errfunc) {
    lua_State * saved_owner = plugin_user_action_owner;
    bool saved_used = plugin_user_action_used;
    plugin_user_action_owner = plugin_lua_main_state(L);
    plugin_user_action_used = false;
    int result = plugin_call(L, nargs, nresults, errfunc);
    plugin_user_action_owner = saved_owner;
    plugin_user_action_used = saved_used;
    return result;
}

bool plugin_manager_user_action_take(lua_State * L) {
    /* The tap belongs to the plugin whose callback it started; a handler of
     * another plugin that runs inside it (an event) does not inherit it. */
    if (!plugin_user_action_owner || plugin_user_action_used || plugin_lua_main_state(L) != plugin_user_action_owner)
        return false;
    plugin_user_action_used = true;
    return true;
}

/* Remove every callback/resource that can retain a failed load's Lua state.
 * A top-level script may have published callbacks before raising an error;
 * lua_close() alone leaves those native registries pointing into freed Lua. */
static bool failed_load_owner_matches(lua_State * owner, lua_State * L, bool aborted) {
    (void) aborted; /* Every retained owner is normalized when the resource is registered. */
    return owner && owner == L;
}

static void discard_failed_plugin_events(lua_State * L, bool aborted) {
    remote_track_stream_cancel_owner((uintptr_t) L);
    for (int event = 0; event < PLUGIN_EVENT_COUNT; event++) {
        int out = 0;
        for (int i = 0; i < plugin_event_subscriber_count[event]; i++) {
            plugin_event_subscriber_t item = plugin_event_subscribers[event][i];
            if (failed_load_owner_matches(item.L, L, aborted)) {
                if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, item.ref);
                continue;
            }
            plugin_event_subscribers[event][out++] = item;
        }
        memset(&plugin_event_subscribers[event][out], 0,
               (size_t)(plugin_event_subscriber_count[event] - out) * sizeof(plugin_event_subscribers[event][0]));
        plugin_event_subscriber_count[event] = out;
    }
}

static void discard_failed_plugin_intervals(lua_State * L, bool aborted) {
    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active || !failed_load_owner_matches(plugin_intervals[i].L, L, aborted)) continue;
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, plugin_intervals[i].ref);
        plugin_intervals[i].active = false;
        plugin_intervals[i].L = NULL;
        plugin_intervals[i].ref = LUA_NOREF;
        gui_plugin_clear_interval(i);
    }
}

static void discard_pending_confirmation(lua_State * L, bool aborted) {
    if (!failed_load_owner_matches(pending_confirmation_L, L, aborted)) return;
    gui_plugin_close_confirmation();
    if (!aborted && pending_confirmation_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, pending_confirmation_ref);
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
}

static void discard_failed_plugin_resources(lua_State * L, bool aborted) {
    if (failed_load_owner_matches(button_lock_owner, L, aborted)) {
        button_lock_owner = NULL;
        hw_buttons_set_button_lock_enabled(false);
    }
    plugin_instance_t * failed_inst = plugin_instance_for_state(L);
    plugin_player_actions_clear_owner(L, failed_inst && failed_inst->defined ? failed_inst->id : NULL, aborted);
    if (failed_load_owner_matches(plugin_order_editor_callback.L, L, aborted)) {
        if (!aborted && plugin_order_editor_callback.callback_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, plugin_order_editor_callback.callback_ref);
        plugin_order_editor_callback.L = NULL;
        plugin_order_editor_callback.callback_ref = LUA_NOREF;
    }
    if (failed_load_owner_matches(mseb_owner, L, aborted)) {
        mseb_reset();
        mseb_owner = NULL;
    }
    for (int i = 0; i < PLUGIN_MAX_FILES; ++i) {
        plugin_instance_t *inst = &plugin_instances[i];
        if (!failed_load_owner_matches(inst->L, L, aborted)) continue;
        memset(&inst->ui_override, 0, sizeof(inst->ui_override));
        inst->music_layout_sequence = inst->drawer_layout_sequence = 0;
    }
    refresh_plugin_ui_customization();
    if (failed_load_owner_matches(plugin_progress_owner, L, aborted))
        plugin_progress_reset();
    bool removed_list_item = false;
    for (int target = 0; target < PLUGIN_LIST_TARGET_COUNT; target++) {
        int out = 0;
        for (int i = 0; i < plugin_list_item_counts[target]; i++) {
            plugin_list_item_t item = plugin_list_items[target][i];
            if (failed_load_owner_matches(item.L, L, aborted)) {
                if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, item.open_ref);
                removed_list_item = true;
                continue;
            }
            plugin_list_items[target][out++] = item;
        }
        memset(&plugin_list_items[target][out], 0,
               (size_t)(plugin_list_item_counts[target] - out) * sizeof(plugin_list_items[target][0]));
        plugin_list_item_counts[target] = out;
    }
    if (removed_list_item) advance_list_registry_generation();

#define COMPACT_FAILED_LOAD_TILES(array, count) do { \
    int out = 0; \
    for (int i = 0; i < (count); i++) { \
        plugin_tile_t item = (array)[i]; \
        if (failed_load_owner_matches(item.L, L, aborted)) { \
            if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, item.open_ref); \
            continue; \
        } \
        (array)[out++] = item; \
    } \
    memset(&(array)[out], 0, (size_t)((count) - out) * sizeof((array)[0])); \
    (count) = out; \
} while (0)
    COMPACT_FAILED_LOAD_TILES(plugin_stream_tiles, plugin_stream_tile_count);
    COMPACT_FAILED_LOAD_TILES(plugin_home_tiles, plugin_home_tile_count);
#undef COMPACT_FAILED_LOAD_TILES

    int toggle_out = 0;
    for (int i = 0; i < plugin_quick_toggle_count; i++) {
        plugin_quick_toggle_t item = plugin_quick_toggles[i];
        if (failed_load_owner_matches(item.L, L, aborted)) {
            if (!aborted) {
                luaL_unref(L, LUA_REGISTRYINDEX, item.change_ref);
                if (item.on_hold_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, item.on_hold_ref);
            }
            continue;
        }
        plugin_quick_toggles[toggle_out++] = item;
    }
    memset(&plugin_quick_toggles[toggle_out], 0,
           (size_t)(plugin_quick_toggle_count - toggle_out) * sizeof(plugin_quick_toggles[0]));
    plugin_quick_toggle_count = toggle_out;

    discard_failed_plugin_events(L, aborted);
    discard_failed_plugin_intervals(L, aborted);

    for (int i = 0; i < PLUGIN_LIST_SCREEN_POOL_SIZE; i++) {
        plugin_list_callback_t * cb = &plugin_list_callbacks[i];
        if (!failed_load_owner_matches(cb->L, L, aborted)) continue;
        if (!aborted && cb->select_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, cb->select_ref);
        memset(cb, 0, sizeof(*cb));
        cb->select_ref = LUA_NOREF;
    }
    for (int slot = 0; slot < PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE; slot++) {
        for (int row = 0; row < plugin_settings_list_row_counts[slot]; row++) {
            plugin_settings_list_row_ref_t * ref = &plugin_settings_list_rows[slot][row];
            if (!failed_load_owner_matches(ref->L, L, aborted)) continue;
            if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, ref->callback_ref);
            ref->L = NULL;
            ref->callback_ref = LUA_NOREF;
        }
        while (plugin_settings_list_row_counts[slot] > 0 &&
               !plugin_settings_list_rows[slot][plugin_settings_list_row_counts[slot] - 1].L)
            plugin_settings_list_row_counts[slot]--;
    }

    if (failed_load_owner_matches(pending_text_input_L, L, aborted)) {
        if (!aborted && pending_text_input_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, pending_text_input_ref);
        pending_text_input_L = NULL;
        pending_text_input_ref = LUA_NOREF;
    }
    discard_pending_confirmation(L, aborted);
    plugin_text_view_bindings_clear_owner(L, aborted);

    plugin_http_bindings_clear_owner(L, aborted);
    plugin_archive_bindings_clear_owner(L, aborted);
    plugin_album_cover_drop_owner(L, aborted);
    if (failed_load_owner_matches(plugin_file_op.L, L, aborted)) plugin_file_op_release();
    if (failed_load_owner_matches(plugin_file_picker.L, L, aborted)) plugin_file_picker_release(true);
}

/* Discards failed plugin instance and rolls back staged hardware volume curve. */
static void discard_failed_plugin_load(plugin_instance_t * inst, lua_State * L,
                                        bool prev_curve_active, const uint8_t * prev_curve) {
    inst->disposing = true;
    audio_stage_custom_hw_volume_curve(prev_curve_active, prev_curve_active ? prev_curve : NULL);
    release_plugin_led_override(L);
    discard_failed_plugin_resources(L, inst->aborted);
    /* A hard-aborted state (load-time top-level code ran past the budget) was
     * longjmp'd out of mid-execution; closing it could run finalizers on a
     * half-unwound VM, so leave it frozen and leaked instead. */
    if (!inst->aborted) lua_close(L);
    memset(inst, 0, sizeof(*inst));
    loading_plugin_slot = -1;
}

static void load_plugin_file(const char * path) {
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (gui_themes_is_legacy_theme_loader(NULL, base)) {
        fprintf(stderr, "[plugins] suppressing legacy theme loader %s\n", path);
        return;
    }
    lua_State * L = luaL_newstate();
    if (!L) return;
    int slot = plugin_instance_count;
    plugin_instance_t * inst = &plugin_instances[slot];
    memset(inst, 0, sizeof(*inst));
    inst->L = plugin_lua_main_state(L);
    size_t base_len = strlen(base);
    if (base_len >= sizeof(inst->filename)) {
        fprintf(stderr, "[plugins] refusing to load %s: filename exceeds %zu bytes\n",
                path, sizeof(inst->filename) - 1);
        lua_close(L);
        memset(inst, 0, sizeof(*inst));
        return;
    }
    memcpy(inst->filename, base, base_len + 1);
    loading_plugin_slot = slot;
    luaL_openlibs(L);
    sandbox_plugin_lua_state(L);
    register_plugin_api(L);

    /* Snapshot staged hardware volume curve so it can be restored if this
     * plugin fails to load. */
    bool prev_curve_active;
    uint8_t prev_curve[HW_VOLUME_CURVE_LEN];
    audio_get_staged_hw_volume_curve_state(&prev_curve_active, prev_curve);

    /* luaL_dofile(L, path)'s own expansion, with plugin_call() in place of
     * a bare lua_pcall() -- top-level plugin code runs once here too, on
     * the same UI thread as every other plugin_call() site, so it needs
     * the same time budget (see plugin_call()'s own comment). */
    if ((luaL_loadfile(L, path) || plugin_call(L, 0, LUA_MULTRET, 0)) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] failed to load %s: %s\n", path, err ? err : "unknown error");
        discard_failed_plugin_load(inst, L, prev_curve_active, prev_curve);
        return;
    }

    if (!inst->defined) {
        snprintf(inst->id, sizeof(inst->id), "legacy.%.*s", (int) sizeof(inst->id) - 8, base);
        char * dot = strrchr(inst->id, '.');
        if (dot && strcasecmp(dot, ".lua") == 0) *dot = '\0';
        /* If generated legacy ID collides with an existing plugin, append
         * a deterministic 64-bit hash of the file path to disambiguate. */
        if (plugin_id_collides(inst->id, slot)) {
            uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a 64 */
            for (const char * p = path; *p; p++) { h ^= (unsigned char) *p; h *= 0x100000001b3ULL; }
            char base_no_ext[sizeof(inst->id)];
            size_t copy_len = base_len < sizeof(base_no_ext) ? base_len : sizeof(base_no_ext) - 1;
            memcpy(base_no_ext, base, copy_len);
            base_no_ext[copy_len] = '\0';
            char * base_dot = strrchr(base_no_ext, '.');
            if (base_dot && strcasecmp(base_dot, ".lua") == 0) *base_dot = '\0';
            snprintf(inst->id, sizeof(inst->id), "legacy.%.*s.%016llx",
                     (int) sizeof(inst->id) - 7 - 1 - 16 - 1, base_no_ext, (unsigned long long) h);
            if (plugin_id_collides(inst->id, slot)) {
                fprintf(stderr, "[plugins] refusing to load %s: generated id '%s' still collides with another plugin's id\n",
                        path, inst->id);
                discard_failed_plugin_load(inst, L, prev_curve_active, prev_curve);
                return;
            }
        }
        snprintf(inst->name, sizeof(inst->name), "%.*s", (int) sizeof(inst->name) - 1, base);
        snprintf(inst->version, sizeof(inst->version), "0");
        inst->defined = true;
    }
    loading_plugin_slot = -1;
    plugin_instance_count++;
}

static int plugin_filename_cmp(const void * a, const void * b) {
    return strcmp((const char *) a, (const char *) b);
}

/* Shared by plugin_manager_init() and plugin_manager_scan_available():
 * returns a malloc'd, filename-sorted, UNBOUNDED array of every *.lua
 * filename directly under <SD card>/.plugins/ (caller must free() it), and
 * writes that directory's path into dir_path_out. Returns NULL (and sets
 * *out_count to 0) if there's no .plugins folder or no .lua files in it.
 *
 * Deliberately not capped at PLUGIN_MAX_FILES here -- both callers need to
 * see the FULL on-disk list before applying their own separate cap
 * (plugin_manager_init() only after filtering out disabled names, so a
 * disabled file's load slot is actually reclaimed; plugin_manager_scan_available()
 * Scans and sorts plugin filenames alphabetically to ensure deterministic load order. */
static char (*scan_plugin_dir_sorted_names(char dir_path_out[600], int * out_count))[256] {
    *out_count = 0;
    snprintf(dir_path_out, 600, "%s/.plugins", MUSIC_ROOT_DIR);

    DIR * d = opendir(dir_path_out);
    if (!d) return NULL; /* no .plugins folder -- nothing to do, not an error */

    char (*names)[256] = NULL;
    size_t name_count = 0;
    size_t capacity = 0;
    struct dirent * ent;
    for (;;) {
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            if (errno != 0) goto fail;
            break;
        }
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".lua") != 0) continue;
        if (gui_themes_is_legacy_theme_loader(NULL, ent->d_name)) continue;
        if (name_count >= (size_t) INT_MAX) {
            errno = EOVERFLOW;
            goto fail;
        }
        if (name_count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 16;
            if (next_capacity < capacity || next_capacity > (size_t) INT_MAX)
                next_capacity = (size_t) INT_MAX;
            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*names)) {
                errno = EOVERFLOW;
                goto fail;
            }
            void * grown = realloc(names, next_capacity * sizeof(*names));
            if (!grown) goto fail;
            names = grown;
            capacity = next_capacity;
        }
        snprintf(names[name_count], sizeof(names[0]), "%s", ent->d_name);
        name_count++;
    }
    if (closedir(d) != 0) {
        free(names);
        return NULL;
    }

    if (name_count == 0) {
        free(names);
        return NULL;
    }
    qsort(names, name_count, sizeof(names[0]), plugin_filename_cmp);
    *out_count = (int) name_count;
    return names;

fail:
    closedir(d);
    free(names);
    return NULL;
}

void plugin_manager_init(void) {
    plugin_last_volume_percent = current_volume_percent();
    plugin_disabled_list_load();

    char dir_path[600];
    int name_count = 0;
    char (*names)[256] = scan_plugin_dir_sorted_names(dir_path, &name_count);
    if (names) {
        /* Filter disabled names out BEFORE the PLUGIN_MAX_FILES cap is
         * applied, not after -- so disabling a plugin actually frees its
         * slot for a 17th alphabetical file, rather than silently wasting
         * a load slot on a file that isn't going to load anyway. */
        int enabled_count = 0;
        for (int i = 0; i < name_count; i++) {
            if (plugin_disabled_list_contains(names[i])) continue;
            if (enabled_count != i) memcpy(names[enabled_count], names[i], sizeof(names[0]));
            enabled_count++;
        }

        int load_count = (enabled_count > PLUGIN_MAX_FILES) ? PLUGIN_MAX_FILES : enabled_count;
        for (int i = 0; i < load_count; i++) {
            if (plugin_instance_count >= PLUGIN_MAX_FILES) break;
            char full_path[700];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);
            load_plugin_file(full_path);
        }
        plugin_over_limit_count = 0;
        for (int i = load_count; i < enabled_count && plugin_over_limit_count < PLUGIN_OVER_LIMIT_MAX; i++) {
            plugin_over_limit[plugin_over_limit_count++] = plugin_filename_hash(names[i]);
            fprintf(stderr, "[plugins] not loading %s: limit of %d plugins reached\n", names[i], PLUGIN_MAX_FILES);
        }
        free(names);
    }
    /* Exactly one hardware-volume-curve commit for the whole (re)load pass,
     * reflecting whichever plugin (if any) called plugin.set_hw_volume_
     * curve() during loading above, or the built-in taper if none did --
     * unconditional (even when `names` was NULL/empty) so
     * plugin_manager_deinit()'s own state-only clear always gets a
     * matching final write. See audio_commit_hw_volume_curve()'s own
     * comment in audio.h for why this needs to be a single deferred commit
     * rather than each plugin.set_hw_volume_curve() call writing hardware
     * immediately during this specific transaction. */
    audio_commit_hw_volume_curve();
}

/* Fills `e` for `filename`, cross-referencing plugin_instances[] for its
 * loaded state and (when loaded) its real display name. */
static void fill_available_entry(plugin_available_entry_t * e, const char * filename) {
    snprintf(e->filename, sizeof(e->filename), "%s", filename);
    e->disabled = plugin_disabled_list_contains(filename);
    e->loaded = false;
    e->over_limit = false;
    e->player_layout = false;
    e->theme_package = false;
    uint64_t h = plugin_filename_hash(filename);
    for (int j = 0; j < plugin_over_limit_count; j++) {
        if (plugin_over_limit[j] == h) {
            e->over_limit = true;
            break;
        }
    }
    snprintf(e->display_name, sizeof(e->display_name), "%s", filename);

    for (int j = 0; j < plugin_instance_count; j++) {
        if (strcmp(plugin_instances[j].filename, filename) == 0) {
            e->loaded = true;
            snprintf(e->display_name, sizeof(e->display_name), "%s", plugin_instances[j].name);
            break;
        }
    }

    /* Layout-only bundles are still represented by their Lua loader file.
     * Detect their companion directory from disk so disabled and over-limit
     * plugins are classified without executing their scripts. */
    size_t filename_len = strlen(filename);
    if (filename_len > 4 && strcasecmp(filename + filename_len - 4, ".lua") == 0) {
        char stem[256];
        size_t stem_len = filename_len - 4;
        if (stem_len < sizeof(stem)) {
            memcpy(stem, filename, stem_len);
            stem[stem_len] = '\0';
            char bundle[900], layout_dir[1000];
            int bn = snprintf(bundle, sizeof(bundle), "%s/%s", MUSIC_ROOT_DIR "/.plugins", stem);
            int ln = (bn > 0 && bn < (int) sizeof(bundle)) ?
                snprintf(layout_dir, sizeof(layout_dir), "%s/player_layouts", bundle) : -1;
            struct stat st;
            if (ln > 0 && ln < (int) sizeof(layout_dir) &&
                lstat(bundle, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
                lstat(layout_dir, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                DIR * layouts = opendir(layout_dir);
                if (layouts) {
                    struct dirent * ent;
                    while ((ent = readdir(layouts)) != NULL) {
                        size_t len = strlen(ent->d_name);
                        if (ent->d_name[0] == '.' || len <= 4 ||
                            strcasecmp(ent->d_name + len - 4, ".xml") != 0) continue;
                        char path[1200];
                        int pn = snprintf(path, sizeof(path), "%s/%s", layout_dir, ent->d_name);
                        if (pn <= 0 || pn >= (int) sizeof(path)) continue;
                        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                            e->player_layout = true;
                            break;
                        }
                    }
                    closedir(layouts);
                }
            }
        }
    }
}

int plugin_manager_scan_available(plugin_available_entry_t * out, int max) {
    char dir_path[600];
    int name_count = 0;
    char (*names)[256] = scan_plugin_dir_sorted_names(dir_path, &name_count);
    if (!names) return 0;

    int count = 0;
    if (name_count <= max) {
        /* Common case -- every file fits, plain alphabetical order. */
        for (int i = 0; i < name_count; i++) fill_available_entry(&out[count++], names[i]);
    } else {
        /* More .lua files on disk than `max` can return. Guarantee every
         * currently-LOADED plugin is still included -- there can only ever
         * be PLUGIN_MAX_FILES of those, always <= max -- rather than
         * truncating in pure alphabetical order and silently hiding an
         * active plugin with no way to toggle it off from this list (real
         * finding: N alphabetically-early disabled files would otherwise
         * push a loaded, alphabetically-later one out of a naive cap).
         * Loaded entries go first, then whatever budget remains is filled
         * in alphabetical order -- sacrifices strict A-Z ordering only in
         * this already-degenerate case. */
        bool * included = calloc((size_t) name_count, sizeof(bool));
        if (!included) { free(names); return 0; }

        for (int i = 0; i < name_count && count < max; i++) {
            bool is_loaded = false;
            for (int j = 0; j < plugin_instance_count; j++) {
                if (strcmp(plugin_instances[j].filename, names[i]) == 0) { is_loaded = true; break; }
            }
            if (is_loaded) {
                fill_available_entry(&out[count++], names[i]);
                included[i] = true;
            }
        }
        for (int i = 0; i < name_count && count < max; i++) {
            if (!included[i]) fill_available_entry(&out[count++], names[i]);
        }
        free(included);
    }

    if (count > 0) {
        const char ** filenames = calloc((size_t) count, sizeof(*filenames));
        bool * layout_flags = calloc((size_t) count, sizeof(*layout_flags));
        bool * theme_flags = calloc((size_t) count, sizeof(*theme_flags));
        if (filenames && layout_flags) {
            for (int i = 0; i < count; i++) {
                filenames[i] = out[i].filename;
                layout_flags[i] = out[i].player_layout;
            }
            plugin_store_classify_loaders(filenames, layout_flags, theme_flags, (size_t) count);
            for (int i = 0; i < count; i++) out[i].player_layout = layout_flags[i];
        }
        if (theme_flags) for (int i = 0; i < count; i++) out[i].theme_package = theme_flags[i];
        free(filenames);
        free(layout_flags);
        free(theme_flags);
    }
    free(names);
    return count;
}

/* Forces every in-flight plugin.http_get_async()/download_file_async()
 * request to finish before a UI reload closes the lua_State it belongs to
 * -- see plugin_manager_deinit()'s own comment. http_cancel_token_cancel()
 * is the same mechanism plugin.http_cancel() already uses to unwind a
 * request early (shuts down the connection's fd so the worker thread wakes
 * up and exits promptly rather than potentially blocking on I/O for the
 * request's own timeout). Deliberately never invokes the Lua callback --
 * plugin_manager_poll()'s own cancelled-request branch already skips it for
 * the same reason (a cancelled request has nothing meaningful to report),
 * and here the callback's L is about to be closed by the caller regardless. */


/* Counterpart to plugin_manager_init(), for an in-process UI reload
 * (gui_reload.c) that needs to re-run every plugin's top-level script
 * against freshly rebuilt screens -- plugin_manager_init() alone is not
 * safe to call a second time: it would append new plugin_instances[]
 * entries after the existing ones (never resetting plugin_instance_count)
 * and leak every previous lua_State, since nothing today ever lua_close()s
 * one.
 *
 * Order matters: background work referencing a plugin's L (async HTTP
 * threads, interval timers, a pending text-input dialog) must be drained
 * BEFORE that L is closed, or the next tick of plugin_manager_poll()/
 * gui_plugin_clear_interval()'s own timer callback/plugin_manager_text_
 * input_submitted() touches freed memory. Every list-item/tile/list-
 * callback/settings-row/event-subscriber registry below is safe to just
 * zero AFTER its owning plugin's L is closed -- every ref in them
 * (luaL_ref) lives inside that same L, so closing it already frees the
 * ref; explicit luaL_unref first is only needed for leak-freedom within a
 * state that stays alive, which doesn't apply once it's about to be closed.
 *
 * Resets home_layout_config back to unconfigured state via gui_plugin_reset_home_layout()
 * so removed or disabled plugins do not leave stale layouts in effect.
 *
 * Does NOT touch the shared theme style objects (screen_builders.c's
 * style_theme_screen_bg/style_theme_card_bg/list_row_style/pill_row_bg_
 * style/style_theme_text_primary/style_theme_text_muted) -- those are live
 * style properties applied immediately by set_background_color()/set_text_
 * color() (PLUGINS.md: "safe to call at any time... no file/cache
 * involved"), with no analogous "reverts if nothing re-calls" contract to
 * honor; a reloaded plugin's own top-level code re-applies them the same
 * as at a normal boot, same as before this change. Also does not touch
 * PLUGIN_THEME_OVERRIDE_ROOT on disk -- that is exactly the icon-override
 * state a reload exists to re-read, not something to clear. */
/* Diagnostic logging for plugin deinitialization steps during reload. */
static void deinit_diag(const char * step) {
    if (!db_log_enabled()) return;
    (void) mkdir("/data/mnt/sd_0/.logs", 0755);
    int fd = open("/data/mnt/sd_0/.logs/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[224];
    int len = snprintf(line, sizeof(line), "[pid=%ld] %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(fd, line, (size_t) len);
        fsync(fd);
    }
    close(fd);
}

static void plugin_manager_mark_disposing(void) {
    /* Block finalizers from publishing new native resources during lua_close. */
    for (int i = 0; i < plugin_instance_count; i++)
        plugin_instances[i].disposing = true;
}

void plugin_manager_deinit(void) {
    remote_track_stream_cancel_all();
    for (int i = 0; i < PLUGIN_MAX_FILES; i++)
        if (plugin_instances[i].L) remote_track_stream_cancel_owner((uintptr_t) plugin_instances[i].L);
    advance_list_registry_generation();
    plugin_manager_mark_disposing();
    /* Layout events can be queued while plugin Lua states are being closed. */
    for (int i = 0; i < plugin_instance_count; i++)
        if (plugin_instances[i].defined) player_layouts_clear_plugin_owner(plugin_instances[i].id);
    plugin_player_actions_clear_all();
    if (plugin_order_editor_callback.L && plugin_order_editor_callback.callback_ref != LUA_NOREF)
        luaL_unref(plugin_order_editor_callback.L, LUA_REGISTRYINDEX,
                   plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = NULL;
    plugin_order_editor_callback.callback_ref = LUA_NOREF;
    mseb_reset();
    mseb_owner = NULL;
    button_lock_owner = NULL;
    hw_buttons_set_button_lock_enabled(false);
    ui_customization_set_override(NULL);
    ui_layout_sequence = 0;
    plugin_progress_reset();
    deinit_diag("plugin_manager_deinit: reset_home_layout before");
    gui_plugin_reset_home_layout();
    gui_plugin_reset_launcher_layout();
    gui_plugin_reset_player_layout();
    player_layouts_session_reset(); /* a plugin's XML layout must not outlive the plugin */
    gui_player_set_transport_skip(NULL, 0);
    audio_set_playback_speed(NULL, 1.0);
    audio_clear_ab_loop();
    audio_clear_ab_switch();
    audio_loop_stop();
    plugin_led_override_owner = NULL;
    led_control_clear_override();
    led_control_apply(current_settings.led_indicator_enabled);
    /* Same "in-process plugin config must not outlive the plugin" category
     * as the two resets above -- without this, disabling/removing a Gain
     * Mode-style plugin followed by a UI reload left its hardware volume
     * curve active indefinitely, with no plugin left running to ever call
     * plugin.set_hw_volume_curve(nil) itself. Stages the reset (NOT
     * audio_set_custom_hw_volume_curve(NULL), which would touch the LIVE
     * state directly and also write to hardware immediately) --
     * plugin_manager_init() always runs right after this and installs +
     * commits the real final value exactly once, see its own
     * audio_commit_hw_volume_curve() call and
     * audio_stage_custom_hw_volume_curve()'s own comment in audio.h for
     * why this needs to stay on the separate staged state rather than the
     * live one a concurrent volume request could still see. */
    audio_stage_custom_hw_volume_curve(false, NULL);
    deinit_diag("plugin_manager_deinit: cancel_all_async_http before");
    plugin_http_bindings_cancel_all();
    plugin_archive_bindings_cancel_all(plugin_archive_owner_is_aborted);
    plugin_album_cover_cancel_all();
    plugin_file_op_release();
    plugin_file_picker_release(true);
    deinit_diag("plugin_manager_deinit: cancel_all_async_http after");

    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active) continue;
        char msg[96];
        snprintf(msg, sizeof(msg), "plugin_manager_deinit: clearing interval slot %d before", i);
        deinit_diag(msg);
        luaL_unref(plugin_intervals[i].L, LUA_REGISTRYINDEX, plugin_intervals[i].ref);
        plugin_intervals[i].active = false;
        plugin_intervals[i].L = NULL;
        gui_plugin_clear_interval(i); /* deletes the backing lv_timer_t -- see its own comment */
    }

    deinit_diag("plugin_manager_deinit: text_input_cancelled before");
    plugin_manager_text_input_cancelled();
    gui_plugin_close_confirmation();
    plugin_manager_confirmation_cancelled();

    /* Deinit drops Lua refs without calling Lua callbacks; UI teardown
     * already ran and discarded any pending on_close. */
    plugin_text_view_bindings_clear_all();

    for (int i = 0; i < plugin_instance_count; i++) {
        if (plugin_instances[i].L) {
            char msg[160];
            snprintf(msg, sizeof(msg), "plugin_manager_deinit: lua_close slot %d id='%s' name='%s' before", i,
                     plugin_instances[i].id, plugin_instances[i].name);
            deinit_diag(msg);
            lua_close(plugin_instances[i].L);
        }
    }
    deinit_diag("plugin_manager_deinit: all lua_close done, memset before");
    memset(plugin_instances, 0, sizeof(plugin_instances));
    plugin_instance_count = 0;
    plugin_over_limit_count = 0;

    memset(plugin_list_items, 0, sizeof(plugin_list_items));
    for (int i = 0; i < PLUGIN_LIST_TARGET_COUNT; i++) {
        plugin_list_item_counts[i] = 0;
    }
    memset(plugin_stream_tiles, 0, sizeof(plugin_stream_tiles));
    plugin_stream_tile_count = 0;
    memset(plugin_home_tiles, 0, sizeof(plugin_home_tiles));
    plugin_home_tile_count = 0;
    memset(plugin_quick_toggles, 0, sizeof(plugin_quick_toggles));
    plugin_quick_toggle_count = 0;
    memset(plugin_list_callbacks, 0, sizeof(plugin_list_callbacks));
    memset(plugin_settings_list_rows, 0, sizeof(plugin_settings_list_rows));
    memset(plugin_settings_list_row_counts, 0, sizeof(plugin_settings_list_row_counts));
    memset(plugin_event_subscriber_count, 0, sizeof(plugin_event_subscriber_count));
    plugin_output_snapshot_valid = false;
}

void plugin_manager_dispatch_remote_streams(void) {
    plugin_remote_stream_poll();
}

void plugin_manager_poll(void) {
    plugin_remote_stream_poll();
    plugin_output_poll();
    plugin_archive_bindings_poll(plugin_call, plugin_archive_owner_is_aborted);
    plugin_album_cover_poll();
    plugin_file_op_poll();
    plugin_http_bindings_poll(plugin_call);
}

bool plugin_manager_has_background_work(void) {
    return plugin_http_bindings_has_work() || plugin_archive_bindings_has_work();
}

/* Shared by plugin_manager_books_list_item_clicked()/_settings_list_item_
 * clicked() below -- kind is just for the stderr message ("books list
 * item"/"settings list item"), so a load-time error in one plugin's row is
 * distinguishable from another's in the log. */
static void dispatch_list_item_open(plugin_list_item_t * item, const char * kind) {
    lua_rawgeti(item->L, LUA_REGISTRYINDEX, item->open_ref);
    if (plugin_call_user_action(item->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(item->L, -1);
        fprintf(stderr, "[plugins] %s '%s' on_open error: %s\n", kind, item->label, err ? err : "unknown error");
        lua_pop(item->L, 1);
    }
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

int plugin_manager_get_books_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_BOOKS); }
const char * plugin_manager_get_books_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_BOOKS, index); }
void plugin_manager_books_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_BOOKS, index); }
void plugin_manager_get_books_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_BOOKS, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_music_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MUSIC); }
const char * plugin_manager_get_music_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MUSIC, index); }
void plugin_manager_music_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MUSIC, index); }
void plugin_manager_get_music_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MUSIC, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_settings_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_SETTINGS); }
const char * plugin_manager_get_settings_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_SETTINGS, index); }
void plugin_manager_settings_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_SETTINGS, index); }
void plugin_manager_get_settings_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_SETTINGS, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_display_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_DISPLAY); }
const char * plugin_manager_get_display_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_DISPLAY, index); }
void plugin_manager_display_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_DISPLAY, index); }
void plugin_manager_get_display_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_DISPLAY, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_playback_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_PLAYBACK); }
const char * plugin_manager_get_playback_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_PLAYBACK, index); }
void plugin_manager_playback_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_PLAYBACK, index); }
void plugin_manager_get_playback_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_PLAYBACK, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_music_audio_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MUSIC_AUDIO); }
const char * plugin_manager_get_music_audio_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MUSIC_AUDIO, index); }
void plugin_manager_music_audio_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MUSIC_AUDIO, index); }
void plugin_manager_get_music_audio_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MUSIC_AUDIO, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_music_controls_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MUSIC_CONTROLS); }
const char * plugin_manager_get_music_controls_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MUSIC_CONTROLS, index); }
void plugin_manager_music_controls_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MUSIC_CONTROLS, index); }
void plugin_manager_get_music_controls_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MUSIC_CONTROLS, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_music_timers_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MUSIC_TIMERS); }
const char * plugin_manager_get_music_timers_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MUSIC_TIMERS, index); }
void plugin_manager_music_timers_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MUSIC_TIMERS, index); }
void plugin_manager_get_music_timers_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MUSIC_TIMERS, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_music_library_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MUSIC_LIBRARY); }
const char * plugin_manager_get_music_library_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MUSIC_LIBRARY, index); }
void plugin_manager_music_library_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MUSIC_LIBRARY, index); }
void plugin_manager_get_music_library_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MUSIC_LIBRARY, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_power_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_POWER); }
const char * plugin_manager_get_power_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_POWER, index); }
void plugin_manager_power_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_POWER, index); }
void plugin_manager_get_power_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_POWER, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_system_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_SYSTEM); }
const char * plugin_manager_get_system_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_SYSTEM, index); }
void plugin_manager_system_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_SYSTEM, index); }
void plugin_manager_get_system_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_SYSTEM, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_more_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_MORE); }
const char * plugin_manager_get_more_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_MORE, index); }
void plugin_manager_more_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_MORE, index); }
void plugin_manager_get_more_list_item_options(int index, const char ** out_icon, int32_t * out_height, int32_t * out_width, const char ** out_text_size) { generic_list_item_options(PLUGIN_LIST_TARGET_MORE, index, out_icon, out_height, out_width, out_text_size); }

int plugin_manager_get_player_list_item_count(void) { return generic_list_item_count(PLUGIN_LIST_TARGET_PLAYER); }
const char * plugin_manager_get_player_list_item_label(int index) { return generic_list_item_label(PLUGIN_LIST_TARGET_PLAYER, index); }
void plugin_manager_player_list_item_clicked(int index) { generic_list_item_clicked(PLUGIN_LIST_TARGET_PLAYER, index); }
uint32_t plugin_manager_get_list_registry_generation(void) { return plugin_list_registry_generation; }

/* Shared by plugin_manager_stream_tile_clicked() below (the only remaining
 * caller now that plugin_manager_tile_clicked() -- the old, single-slot
 * "Audio Books" dispatch -- is gone, replaced by the books-list-item
 * registry above). */
static void dispatch_tile_open(plugin_tile_t * t) {
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->open_ref);
    if (plugin_call_user_action(t->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] tile '%s' on_open error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
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
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->change_ref);
    lua_pushboolean(t->L, new_value);
    if (plugin_call(t->L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] quick toggle '%s' on_change error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
}

bool plugin_manager_quick_toggle_open_settings(int index) {
    if (index < 0 || index >= plugin_quick_toggle_count) return false;
    plugin_quick_toggle_t * t = &plugin_quick_toggles[index];
    if (!t->L || t->on_hold_ref == LUA_NOREF) return false;
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->on_hold_ref);
    if (plugin_call(t->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] quick toggle '%s' on_hold error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
    return true;
}

void plugin_manager_list_item_selected(int slot, int index) {
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) return;
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (!cb->L || cb->select_ref == LUA_NOREF) return;

    lua_rawgeti(cb->L, LUA_REGISTRYINDEX, cb->select_ref);
    lua_pushinteger(cb->L, index + 1);
    if (plugin_call_user_action(cb->L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(cb->L, -1);
        fprintf(stderr, "[plugins] show_list on_select error: %s\n", err ? err : "unknown error");
        lua_pop(cb->L, 1);
    }
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

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (plugin_call_user_action(L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_select error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_settings_list_toggled(int slot, int row, bool new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushboolean(L, new_value);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_change error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_settings_list_slid(int slot, int row, int new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, new_value);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_change error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_order_editor_changed(int slot, const char * const * keys,
                                         const char * const * labels, const bool * visible, int count) {
    if (slot != 0 || !plugin_order_editor_callback.L ||
        plugin_order_editor_callback.callback_ref == LUA_NOREF ||
        !keys || !labels || !visible || count < 1 || count > PLUGIN_ORDER_EDITOR_MAX_ITEMS) return;
    lua_State * L = plugin_order_editor_callback.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, plugin_order_editor_callback.callback_ref);
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
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_order_editor on_change error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_order_editor_closed(int slot) {
    if (slot != 0) return;
    if (plugin_order_editor_callback.L && plugin_order_editor_callback.callback_ref != LUA_NOREF)
        luaL_unref(plugin_order_editor_callback.L, LUA_REGISTRYINDEX,
                   plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = NULL;
    plugin_order_editor_callback.callback_ref = LUA_NOREF;
}

void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                          double duration_seconds, const char * provider, const char * track_id) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_TRACK_STARTED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_TRACK_STARTED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushstring(sub->L, title ? title : "");
        lua_pushstring(sub->L, artist ? artist : "");
        lua_pushstring(sub->L, album ? album : "");
        lua_pushnumber(sub->L, duration_seconds);
        lua_pushstring(sub->L, provider ? provider : "");
        lua_pushstring(sub->L, track_id ? track_id : "");
        if (plugin_call(sub->L, 6, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] track_started handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

/* Shared by plugin_manager_notify_paused()/_resumed()/_stopped() below --
 * the three zero-argument events. */
static void notify_event_no_args(plugin_event_t idx, const char * kind) {
    for (int i = 0; i < plugin_event_subscriber_count[idx]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        if (plugin_call(sub->L, 0, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] %s handler error: %s\n", kind, err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_paused(void) {
    notify_event_no_args(PLUGIN_EVENT_PAUSED, "paused");
}

void plugin_manager_notify_resumed(void) {
    notify_event_no_args(PLUGIN_EVENT_RESUMED, "resumed");
}

void plugin_manager_notify_stopped(void) {
    notify_event_no_args(PLUGIN_EVENT_STOPPED, "stopped");
}

void plugin_manager_notify_screen_woke(void) {
    notify_event_no_args(PLUGIN_EVENT_SCREEN_WOKE, "screen_woke");
}

void plugin_manager_notify_button_lock_changed(bool locked) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_BUTTON_LOCK_CHANGED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_BUTTON_LOCK_CHANGED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushboolean(sub->L, locked);
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] button_lock_changed handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_queue_exhausted(int direction) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_QUEUE_EXHAUSTED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_QUEUE_EXHAUSTED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushnumber(sub->L, direction);
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] queue_exhausted handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_volume_changed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == plugin_last_volume_percent) return;
    plugin_last_volume_percent = percent;

    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_VOLUME_CHANGED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_VOLUME_CHANGED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushinteger(sub->L, percent);
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] volume_changed handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

static bool battery_info_equal(const plugin_battery_info_t * a, const plugin_battery_info_t * b) {
    return a->level == b->level && a->charging == b->charging && a->full == b->full &&
           a->external_power == b->external_power;
}

void plugin_manager_poll_battery(void) {
    plugin_battery_info_t current = plugin_read_battery_info();
    if (!plugin_battery_snapshot_valid) {
        plugin_last_battery_info = current;
        plugin_battery_snapshot_valid = true;
        return;
    }
    if (battery_info_equal(&current, &plugin_last_battery_info)) return;
    plugin_last_battery_info = current;

    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_BATTERY_CHANGED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_BATTERY_CHANGED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        push_battery_info(sub->L, &current);
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] battery_changed handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

/* Cached Wi-Fi state, written only by the UI thread through
 * plugin_manager_set_network_state(); the getter reads it without any
 * subprocess or socket access. */
static bool plugin_network_connected;
static bool plugin_network_wifi_enabled;
static bool plugin_network_valid;

static void push_network_info(lua_State * L) {
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, plugin_network_connected);
    lua_setfield(L, -2, "connected");
    lua_pushboolean(L, plugin_network_wifi_enabled);
    lua_setfield(L, -2, "wifi_enabled");
}

static int l_plugin_network_status(lua_State * L) {
    push_network_info(L);
    return 1;
}

bool plugin_manager_has_network_subscribers(void) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_NETWORK_CHANGED]; i++) {
        plugin_instance_t * inst = plugin_instance_for_state(
            plugin_event_subscribers[PLUGIN_EVENT_NETWORK_CHANGED][i].L);
        if (!inst || !inst->aborted) return true;
    }
    return false;
}

void plugin_manager_set_network_state(bool connected, bool wifi_enabled) {
    if (!wifi_enabled) connected = false;
    if (plugin_network_valid && connected == plugin_network_connected &&
        wifi_enabled == plugin_network_wifi_enabled) return;
    bool first = !plugin_network_valid;
    plugin_network_connected = connected;
    plugin_network_wifi_enabled = wifi_enabled;
    plugin_network_valid = true;
    /* The first sample is only the baseline. It is always {false, enabled}
     * because it is taken before any query returns; the first query reporting
     * connected then arrives as a normal change event. */
    if (first && !connected) return;

    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_NETWORK_CHANGED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_NETWORK_CHANGED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        push_network_info(sub->L);
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] network_changed handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_suspending(void) {
    notify_event_no_args(PLUGIN_EVENT_SUSPENDING, "suspending");
}

void plugin_manager_notify_system_resumed(void) {
    notify_event_no_args(PLUGIN_EVENT_SYSTEM_RESUMED, "system_resumed");
}

static void notify_event_string(plugin_event_t idx, const char * kind, const char * value) {
    for (int i = 0; i < plugin_event_subscriber_count[idx]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushstring(sub->L, value ? value : "");
        if (plugin_call(sub->L, 1, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] %s handler error: %s\n", kind, err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_screenshot_saved(const char * path) {
    notify_event_string(PLUGIN_EVENT_SCREENSHOT_SAVED, "screenshot_saved", path);
}

void plugin_manager_notify_screenshot_failed(const char * reason) {
    notify_event_string(PLUGIN_EVENT_SCREENSHOT_FAILED, "screenshot_failed", reason);
}

void plugin_manager_interval_fired(int slot) {
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return;

    lua_State * L = plugin_intervals[slot].L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, plugin_intervals[slot].ref);
    if (plugin_call(L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] set_interval callback error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_text_input_submitted(const char * text) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;

    lua_State * L = pending_text_input_L;
    int ref = pending_text_input_ref;
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, text ? text : "");
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_text_input on_submit error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

void plugin_manager_text_input_cancelled(void) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;
    luaL_unref(pending_text_input_L, LUA_REGISTRYINDEX, pending_text_input_ref);
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;
}

void plugin_manager_confirmation_confirmed(void) {
    if (!pending_confirmation_L || pending_confirmation_ref == LUA_NOREF) return;
    lua_State * L = pending_confirmation_L;
    int ref = pending_confirmation_ref;
    /* Clear first so a callback can safely open another confirmation. */
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (plugin_call_user_action(L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_confirmation callback error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

void plugin_manager_confirmation_cancelled(void) {
    if (!pending_confirmation_L || pending_confirmation_ref == LUA_NOREF) return;
    luaL_unref(pending_confirmation_L, LUA_REGISTRYINDEX, pending_confirmation_ref);
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
}
