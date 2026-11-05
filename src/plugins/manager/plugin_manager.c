#include "../../ui/application/gui.h"
#include "../../ui/player/layout/player_layouts.h"
#include "../../audio/effects/mseb.h"
#include "../../audio/effects/soundstage.h"
#include "../../hardware/indicators/led_control.h"
#include "../json/plugin_json.h"
#include "../bindings/ui/plugin_info_bindings.h"
#include "../bindings/library/plugin_library_bindings.h"
#include "../bindings/audio/plugin_equalizer_bindings.h"
#include "../bindings/io/plugin_http_bindings.h"
#include "../bindings/ui/plugin_text_view_bindings.h"
#include "../bindings/io/plugin_archive_bindings.h"
#include "../bindings/io/plugin_apps_bindings.h"
#include "../lua/plugin_lua_runtime.h"
#include "plugin_internal.h"
#include "../lifecycle/plugin_disabled_list.h"
#include "../../hardware/input/hw_buttons.h"
#include "../../ui/appearance/gui_themes.h"
#include "../../core/system/logging/db_log.h"

#include "lualib.h"

#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "plugin_manager_internal.h"

plugin_instance_t plugin_instances[PLUGIN_MAX_FILES];
int plugin_instance_count = 0;
int loading_plugin_slot = -1;

/* plugin.storage and plugin.secrets need to know WHICH plugin is calling
 * at arbitrary runtime (not just at plugin.define() time, unlike
 * loading_plugin_slot above) -- each plugin owns exactly one lua_State for
 * its whole lifetime (this file's own top comment), so a linear scan
 * matching L is enough; PLUGIN_MAX_FILES is small and this only runs on
 * an explicit plugin.storage/secrets call, not per frame. */
plugin_instance_t * plugin_instance_for_state(lua_State * L) {
    if (!L) return NULL;
    /* Main states match directly, without touching a possibly frozen
     * (aborted) state; a coroutine falls back to its main thread. */
    for (int i = 0; i < PLUGIN_MAX_FILES; i++) {
        if (plugin_instances[i].L == L) return &plugin_instances[i];
    }
    lua_State * main_L = plugin_lua_main_state(L);
    if (main_L == L) return NULL;
    for (int i = 0; i < PLUGIN_MAX_FILES; i++) {
        if (plugin_instances[i].L == main_L) return &plugin_instances[i];
    }
    return NULL;
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
    if (!plugin_id_charset_ok(id) || strlen(id) >= sizeof(inst->id)) {
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

static const luaL_Reg plugin_funcs[] = {
    { "define",                    l_plugin_define },
    { "api_version",               l_plugin_api_version },
    { "has_capability",            l_plugin_has_capability },
    { "language",                  l_plugin_language },
    { "tr",                        l_plugin_tr },
    { "get_app_info",              l_plugin_get_app_info },
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
    { "http_request",              l_plugin_http_request },
    { "download_file_async",       l_plugin_download_file_async },
    { "get_download_progress",     l_plugin_get_download_progress },
    { "cancel",                    l_plugin_cancel },
    { "json_decode",               l_plugin_json_decode },
    { "json_encode",               l_plugin_json_encode },
    { "media_capabilities",        l_plugin_media_capabilities },
    { "get_artist_albums",         l_plugin_get_artist_albums },
    { "get_album_tracks",          l_plugin_get_album_tracks },
    { "get_next_album_tracks",     l_plugin_get_next_album_tracks },
    { "library_song_count",        l_plugin_library_song_count },
    { "library_get_songs",         l_plugin_library_get_songs },
    { "library_search",            l_plugin_library_search },
    { "library_get_song",          l_plugin_library_get_song },
    { "library_get_artists",       l_plugin_library_get_artists },
    { "library_get_albums",        l_plugin_library_get_albums },
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
    { NULL, NULL }
};

static void register_plugin_api(lua_State * L) {
    lua_newtable(L);
    plugin_lib_add(L, plugin_funcs);
    plugin_lib_add(L, plugin_system_funcs);
    plugin_lib_add(L, plugin_files_funcs);
    plugin_lib_add(L, plugin_playback_funcs);
    plugin_lib_add(L, plugin_soundstage_funcs);
    plugin_lib_add(L, plugin_ui_funcs);
    plugin_lib_add(L, plugin_registries_funcs);
    plugin_lib_add(L, plugin_list_screens_funcs);
    plugin_lib_add(L, plugin_events_funcs);
    plugin_lib_add(L, plugin_layout_funcs);
    register_plugin_lib(L, plugin_now_playing_funcs);
    lua_setfield(L, -2, "now_playing");
    register_plugin_lib(L, plugin_storage_funcs);
    lua_setfield(L, -2, "storage");
    register_plugin_lib(L, plugin_secrets_funcs);
    lua_setfield(L, -2, "secrets");
    lua_setglobal(L, "plugin");
}

/* Remove every callback/resource that can retain a failed load's Lua state.
 * A top-level script may have published callbacks before raising an error;
 * lua_close() alone leaves those native registries pointing into freed Lua. */
bool failed_load_owner_matches(lua_State * owner, lua_State * L) {
    /* Every retained owner is normalized when the resource is registered. */
    return owner && owner == L;
}

static void discard_failed_plugin_resources(lua_State * L, bool aborted) {
    if (failed_load_owner_matches(button_lock_owner, L)) {
        button_lock_owner = NULL;
        hw_buttons_set_button_lock_enabled(false);
    }
    plugin_instance_t * failed_inst = plugin_instance_for_state(L);
    plugin_player_actions_clear_owner(L, failed_inst && failed_inst->defined ? failed_inst->id : NULL, aborted);
    if (failed_load_owner_matches(plugin_order_editor_callback.L, L)) {
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, plugin_order_editor_callback.callback_ref);
        plugin_order_editor_callback.L = NULL;
        plugin_order_editor_callback.callback_ref = LUA_NOREF;
    }
    if (failed_load_owner_matches(mseb_owner, L)) {
        mseb_reset();
        mseb_owner = NULL;
    }
    if (failed_load_owner_matches(soundstage_owner, L)) {
        soundstage_reset();
        soundstage_owner = NULL;
    }
    for (int i = 0; i < PLUGIN_MAX_FILES; ++i) {
        plugin_instance_t *inst = &plugin_instances[i];
        if (!failed_load_owner_matches(inst->L, L)) continue;
        memset(&inst->ui_override, 0, sizeof(inst->ui_override));
        inst->music_layout_sequence = inst->drawer_layout_sequence = 0;
    }
    refresh_plugin_ui_customization();
    if (failed_load_owner_matches(plugin_progress_owner, L))
        plugin_progress_reset();
    bool removed_list_item = false;
    for (int target = 0; target < PLUGIN_LIST_TARGET_COUNT; target++) {
        int out = 0;
        for (int i = 0; i < plugin_list_item_counts[target]; i++) {
            plugin_list_item_t item = plugin_list_items[target][i];
            if (failed_load_owner_matches(item.L, L)) {
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
        if (failed_load_owner_matches(item.L, L)) { \
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
        if (failed_load_owner_matches(item.L, L)) {
            if (!aborted) {
                luaL_unref(L, LUA_REGISTRYINDEX, item.change_ref);
                luaL_unref(L, LUA_REGISTRYINDEX, item.on_hold_ref);
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
        if (!failed_load_owner_matches(cb->L, L)) continue;
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, cb->select_ref);
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, cb->hold_ref);
        memset(cb, 0, sizeof(*cb));
        cb->select_ref = LUA_NOREF;
        cb->hold_ref = LUA_NOREF;
    }
    for (int slot = 0; slot < PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE; slot++) {
        for (int row = 0; row < plugin_settings_list_row_counts[slot]; row++) {
            plugin_settings_list_row_ref_t * ref = &plugin_settings_list_rows[slot][row];
            if (!failed_load_owner_matches(ref->L, L)) continue;
            if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, ref->callback_ref);
            ref->L = NULL;
            ref->callback_ref = LUA_NOREF;
        }
        while (plugin_settings_list_row_counts[slot] > 0 &&
               !plugin_settings_list_rows[slot][plugin_settings_list_row_counts[slot] - 1].L)
            plugin_settings_list_row_counts[slot]--;
    }

    if (failed_load_owner_matches(pending_text_input_L, L)) {
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, pending_text_input_ref);
        pending_text_input_L = NULL;
        pending_text_input_ref = LUA_NOREF;
    }
    discard_pending_confirmation(L, aborted);
    discard_pending_menu(L, aborted);
    plugin_text_view_bindings_clear_owner(L, aborted);

    plugin_http_bindings_clear_owner(L, aborted);
    plugin_archive_bindings_clear_owner(L, aborted);
    plugin_album_cover_drop_owner(L, aborted);
    if (failed_load_owner_matches(plugin_file_op.L, L)) plugin_file_op_release();
    if (failed_load_owner_matches(plugin_file_picker.L, L)) plugin_picker_job_release(&plugin_file_picker, true);
    if (failed_load_owner_matches(plugin_color_picker.L, L)) plugin_picker_job_release(&plugin_color_picker, true);
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
    plugin_lua_register_main_state(L); /* the only Lua states in the process are plugin states */
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
        const char * err = plugin_state_frozen(L) ? "plugin call hard-aborted after exceeding its time budget"
                                                  : lua_tostring(L, -1);
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
            uint64_t h = plugin_filename_hash(path);
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

void plugin_manager_init(void) {
    ui_order_set_extras(UI_ORDER_MUSIC, plugin_manager_get_music_list_item_count,
                        plugin_manager_get_music_list_item_key, plugin_manager_get_music_list_item_label);
    ui_order_set_extras(UI_ORDER_DRAWER, plugin_manager_get_quick_toggle_count,
                        plugin_manager_get_quick_toggle_key, plugin_manager_get_quick_toggle_label);
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
void plugin_manager_deinit(void) {
    remote_track_stream_cancel_all();
    for (int i = 0; i < PLUGIN_MAX_FILES; i++)
        if (plugin_instances[i].L) remote_track_stream_cancel_owner((uintptr_t) plugin_instances[i].L);
    advance_list_registry_generation();
    /* Block finalizers from publishing new native resources during lua_close. */
    for (int i = 0; i < plugin_instance_count; i++)
        plugin_instances[i].disposing = true;
    /* Layout events can be queued while plugin Lua states are being closed. */
    for (int i = 0; i < plugin_instance_count; i++)
        if (plugin_instances[i].defined) player_layouts_clear_plugin_owner(plugin_instances[i].id);
    plugin_player_actions_clear_all();
    plugin_unref(plugin_order_editor_callback.L, plugin_order_editor_callback.callback_ref);
    plugin_order_editor_callback.L = NULL;
    plugin_order_editor_callback.callback_ref = LUA_NOREF;
    mseb_reset();
    mseb_owner = NULL;
    soundstage_reset();
    soundstage_owner = NULL;
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
    /* Async HTTP/archive requests are cancelled without invoking their Lua
     * callbacks: the callback's L is about to be closed anyway and a
     * cancelled request has nothing meaningful to report. */
    deinit_diag("plugin_manager_deinit: cancel_all_async_http before");
    plugin_http_bindings_cancel_all(plugin_state_frozen);
    plugin_archive_bindings_cancel_all(plugin_state_frozen);
    plugin_album_cover_cancel_all();
    plugin_file_op_release();
    plugin_picker_job_release(&plugin_file_picker, true);
    plugin_picker_job_release(&plugin_color_picker, true);
    deinit_diag("plugin_manager_deinit: cancel_all_async_http after");

    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active) continue;
        char msg[96];
        snprintf(msg, sizeof(msg), "plugin_manager_deinit: clearing interval slot %d before", i);
        deinit_diag(msg);
        plugin_unref(plugin_intervals[i].L, plugin_intervals[i].ref);
        plugin_intervals[i].active = false;
        plugin_intervals[i].L = NULL;
        gui_plugin_clear_interval(i); /* deletes the backing lv_timer_t -- see its own comment */
    }

    deinit_diag("plugin_manager_deinit: text_input_cancelled before");
    plugin_manager_text_input_cancelled();
    gui_plugin_close_confirmation();
    plugin_manager_confirmation_cancelled();
    gui_plugin_close_menu();
    plugin_manager_menu_cancelled();

    /* Deinit drops Lua refs without calling Lua callbacks; UI teardown
     * already ran and discarded any pending on_close. */
    plugin_text_view_bindings_clear_all();

    for (int i = 0; i < plugin_instance_count; i++) {
        /* An aborted state stays frozen and leaked, as in discard_failed_plugin_load(). */
        if (plugin_instances[i].L && !plugin_instances[i].aborted) {
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
    plugin_archive_bindings_poll(plugin_call, plugin_state_frozen);
    plugin_album_cover_poll();
    plugin_file_op_poll();
    plugin_http_bindings_poll(plugin_call, plugin_state_frozen);
}

bool plugin_manager_has_background_work(void) {
    return plugin_http_bindings_has_work() || plugin_archive_bindings_has_work();
}
