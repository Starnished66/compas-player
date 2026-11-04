#ifndef PLUGIN_MANAGER_INTERNAL_H
#define PLUGIN_MANAGER_INTERNAL_H

/* Shared declarations of the plugin manager translation units. Only what more
 * than one of them needs is here; everything else stays static in its file. */

#include "plugin_manager.h"
#include "../../ui/text/rendering/fallback_font.h"
#include "../../core/persistence/settings.h"
#include "../../core/themes/ui_customization.h"
#include "lua.h"
#include "lauxlib.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
/* Enabled files the last init left unloaded because PLUGIN_MAX_FILES was
 * reached, kept as filename hashes for plugin_manager_scan_available(). */
#define PLUGIN_OVER_LIMIT_MAX 64
/* Truncates src to fit dst on a UTF-8 boundary, then sanitizes it. */
static inline void plugin_copy_label(char * dst, size_t n, const char * src) {
    utf8_truncate_safe(dst, src, n);
    utf8_sanitize(dst);
}

/* Calls the callback pushed by plugin_push_callback() (plus nargs arguments)
 * with no results and logs "[plugins] <fmt>: <error>" on failure. A call that
 * hard-aborted the plugin leaves its state frozen, so the error is neither
 * read nor popped. Returns true on success. */
bool plugin_call_report(lua_State * L, int nargs, bool user_action, const char * fmt, ...)
    __attribute__((format(printf, 4, 5)));

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

/* Keep physical registry storage at least as large as every per-list limit.
 * Compile-time checks prevent a future cap increase from accepting writes
 * beyond a target's row. */
#define PLUGIN_LIST_REGISTRY_CAPACITY 16

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

/* Each reusable plugin.show_list() screen owns its on_select callback. This
 * matters after Back returns from a nested list: the older screen must route
 * through its original callback, not whichever list was opened most recently. */
typedef struct {
    lua_State * L;
    int select_ref;
    int hold_ref; /* options.on_hold, or LUA_NOREF */
    lua_Integer handle; /* show_list()'s return value for the list now in this slot */
} plugin_list_callback_t;

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

typedef struct {
    lua_State * L;
    int callback_ref;
} plugin_order_editor_callback_t;

typedef struct {
    bool active;
    unsigned id; /* file_op_status_t.id of the plugin's operation */
    lua_State * L;
    int callback_ref;
} plugin_file_op_t;

/* One pending native picker request (pick_file, pick_color) per kind. */
typedef struct { bool active; lua_State * L; int callback_ref; void (*cancel_ui)(void); } plugin_picker_job_t;

typedef struct {
    int level;
    bool charging;
    bool full;
    bool external_power;
} plugin_battery_info_t;

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

/* ---- plugin.set_interval(seconds, callback) -> handle /
 * plugin.clear_interval(handle) -- see plugin_manager.h's own
 * PLUGIN_MAX_INTERVALS/PLUGIN_INTERVAL_MIN_MS comments. handle is the pool
 * slot + 1 (0 reserved as "invalid"/never returned). ---- */

typedef struct {
    lua_State * L;
    int ref;
    bool active;
} plugin_interval_t;

/* plugin_call.c */
bool plugin_state_frozen(lua_State * L);
void plugin_unref(lua_State * L, int ref);
bool plugin_push_callback(lua_State * L, int ref);
void register_plugin_lib(lua_State * L, const luaL_Reg * regs);
void sandbox_plugin_lua_state(lua_State * L);
int plugin_call(lua_State * L, int nargs, int nresults, int errfunc);
int plugin_call_user_action(lua_State * L, int nargs, int nresults, int errfunc);
void plugin_lib_add(lua_State * L, const luaL_Reg * regs);

/* plugin_scan.c */
extern uint64_t plugin_over_limit[PLUGIN_OVER_LIMIT_MAX];
extern int plugin_over_limit_count;
uint64_t plugin_filename_hash(const char * name);
char (*scan_plugin_dir_sorted_names(char dir_path_out[600], int * out_count))[256];

/* plugin_system_bindings.c */
extern lua_State * plugin_led_override_owner;
void release_plugin_led_override(lua_State * L);
void plugin_album_cover_drop_owner(lua_State * owner, bool aborted);
void plugin_album_cover_cancel_all(void);
void plugin_album_cover_poll(void);
extern lua_State * button_lock_owner;
extern const luaL_Reg plugin_storage_funcs[];
extern const luaL_Reg plugin_secrets_funcs[];
extern const luaL_Reg plugin_system_funcs[];

/* plugin_file_bindings.c */
extern plugin_file_op_t plugin_file_op;
extern plugin_picker_job_t plugin_file_picker;
extern plugin_picker_job_t plugin_color_picker;
void plugin_picker_job_release(plugin_picker_job_t * job, bool cancel_ui);
void plugin_file_op_release(void);
void plugin_file_op_poll(void);
bool plugin_song_path_valid(const char * path);
extern const luaL_Reg plugin_files_funcs[];

/* plugin_playback_bindings.c */
extern lua_State * mseb_owner;
extern const char * const plugin_route_names[];
plugin_battery_info_t plugin_read_battery_info(void);
void push_battery_info(lua_State * L, const plugin_battery_info_t * info);
int current_volume_percent(void);
extern const luaL_Reg plugin_now_playing_funcs[];
extern const luaL_Reg plugin_playback_funcs[];

/* plugin_soundstage_bindings.c */
extern lua_State * soundstage_owner;
extern const luaL_Reg plugin_soundstage_funcs[];

/* plugin_ui_bindings.c */
extern lua_State * plugin_progress_owner;
void plugin_progress_reset(void);
void get_opt_color_field(lua_State * L, int idx, const char * field, bool * out_has, uint32_t * out_value);
void get_opt_bool_field(lua_State * L, int idx, const char * field, bool * out_has, bool * out_value);
int32_t check_int32_field(lua_State * L, lua_Integer value, const char * fn_name, const char * field);
extern lua_State * pending_text_input_L;
extern int pending_text_input_ref;
void discard_pending_confirmation(lua_State * L, bool aborted);
void discard_pending_menu(lua_State * L, bool aborted);
extern const luaL_Reg plugin_ui_funcs[];

/* plugin_registries.c */
extern plugin_list_item_t plugin_list_items[PLUGIN_LIST_TARGET_COUNT][PLUGIN_LIST_REGISTRY_CAPACITY];
extern int plugin_list_item_counts[PLUGIN_LIST_TARGET_COUNT];
void advance_list_registry_generation(void);
extern plugin_tile_t plugin_stream_tiles[PLUGIN_MAX_STREAM_TILES];
extern int plugin_stream_tile_count;
extern plugin_tile_t plugin_home_tiles[PLUGIN_MAX_HOME_TILES];
extern int plugin_home_tile_count;
extern plugin_quick_toggle_t plugin_quick_toggles[PLUGIN_MAX_QUICK_TOGGLES];
extern int plugin_quick_toggle_count;
bool is_valid_text_size(const char * text_size);
extern const luaL_Reg plugin_registries_funcs[];

/* plugin_list_screens.c */
extern plugin_list_callback_t plugin_list_callbacks[PLUGIN_LIST_SCREEN_POOL_SIZE];
extern plugin_settings_list_row_ref_t plugin_settings_list_rows[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_ROWS];
extern int plugin_settings_list_row_counts[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];
extern plugin_order_editor_callback_t plugin_order_editor_callback;
extern const luaL_Reg plugin_list_screens_funcs[];

/* plugin_events.c */
extern int plugin_event_subscriber_count[PLUGIN_EVENT_COUNT];
bool plugin_remote_resolver_registered(lua_State * L);
void plugin_remote_stream_poll(void);
extern int plugin_last_volume_percent;
extern bool plugin_output_snapshot_valid;
void plugin_output_poll(void);
extern plugin_interval_t plugin_intervals[PLUGIN_MAX_INTERVALS];
void discard_failed_plugin_events(lua_State * L, bool aborted);
void discard_failed_plugin_intervals(lua_State * L, bool aborted);
int l_plugin_network_status(lua_State * L);
extern const luaL_Reg plugin_events_funcs[];

/* plugin_layout.c */
extern uint64_t ui_layout_sequence;
void plugin_player_actions_clear_owner(lua_State * L, const char * owner, bool aborted);
void plugin_player_actions_clear_all(void);
void refresh_plugin_ui_customization(void);
extern const luaL_Reg plugin_layout_funcs[];

/* plugin_manager.c */
extern plugin_instance_t plugin_instances[PLUGIN_MAX_FILES];
extern int plugin_instance_count;
extern int loading_plugin_slot;
plugin_instance_t * plugin_instance_for_state(lua_State * L);
bool failed_load_owner_matches(lua_State * owner, lua_State * L);

#endif /* PLUGIN_MANAGER_INTERNAL_H */
