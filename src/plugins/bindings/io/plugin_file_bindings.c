/* plugin.* bindings for directory listing, file operations, pickers, mkdir and playlists. */
#include "../../../ui/application/gui.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../library/browser/files/file_ops.h"
#include "../../../library/playlists/playlist_files.h"
#include "../library/plugin_library_bindings_internal.h"
#include "../../storage/plugin_reserved_dirs.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../storage/plugin_storage.h"
#include "../../../ui/appearance/gui_color_picker.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "../../manager/plugin_manager_internal.h"

static int l_plugin_list_dir(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.list_dir");
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
    plugin_lua_check_external_path(L, 1, "plugin.realpath");
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return plugin_lua_push_error(L, strerror(errno));
    lua_pushstring(L, resolved);
    plugin_lua_check_external_path(L, -1, "plugin.realpath");
    return 1;
}

/* ---- File Manager for plugins (API 16) ----
 * plugin.file_copy/file_move/file_delete run the File Manager's engine
 * (file_ops.c): one operation at a time, shared with the File Manager,
 * never overwriting, and limited to the SD card because it recurses into
 * folders. The player's own folders cannot be deleted or moved here; the
 * File Manager asks the user instead. */
#define PLUGIN_FILE_OP_MAX_PATHS 4096

plugin_file_op_t plugin_file_op = { .callback_ref = LUA_NOREF };
plugin_picker_job_t plugin_file_picker = { .callback_ref = LUA_NOREF, .cancel_ui = gui_plugin_cancel_file_picker };
plugin_picker_job_t plugin_color_picker = { .callback_ref = LUA_NOREF, .cancel_ui = gui_plugin_cancel_color_picker };

/* A canonical path on the SD card (aliases such as "sd_0/." or "sd_0//"
 * would reach the whole card): the card root itself only when allow_root
 * (a destination folder). */
static bool plugin_sd_path_ok(const char * path, bool allow_root) {
    return file_op_path_in_root(path, MUSIC_ROOT_DIR) && (allow_root || strcmp(path, MUSIC_ROOT_DIR) != 0);
}

/* Canonicalizes both root and path (closing symlink escapes, unlike a lexical
 * prefix test) and checks that path is inside root. On success path's
 * canonical form is left in real and the part below the root is returned
 * ("" for the root itself when allow_root); NULL otherwise. */
static const char * plugin_path_under_root(const char * root, const char * path, bool allow_root,
                                           char real[PATH_MAX]) {
    char root_real[PATH_MAX];
    if (!realpath(root, root_real) || !realpath(path, real)) return NULL;
    size_t root_len = strlen(root_real);
    if (strncmp(real, root_real, root_len) != 0) return NULL;
    if (real[root_len] == '/') return real + root_len + 1;
    return allow_root && real[root_len] == '\0' ? real + root_len : NULL;
}

static bool plugin_picker_folder_valid(const char * folder) {
    if (!plugin_sd_path_ok(folder, true)) return false;
    char folder_real[PATH_MAX];
    if (!plugin_path_under_root(MUSIC_ROOT_DIR, folder, true, folder_real)) return false;
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

static void plugin_picker_job_begin(plugin_picker_job_t * job, lua_State * L, int callback_index) {
    job->active = true;
    job->L = plugin_lua_main_state(L);
    lua_pushvalue(L, callback_index);
    job->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void plugin_picker_job_release(plugin_picker_job_t * job, bool cancel_ui) {
    lua_State * L = job->L;
    int ref = job->callback_ref;
    job->active = false;
    job->L = NULL;
    job->callback_ref = LUA_NOREF;
    plugin_unref(L, ref);
    if (cancel_ui) job->cancel_ui();
}

/* Releases the job and leaves its callback on the stack of the returned
 * state, ready for the caller to push arguments and plugin_picker_job_call();
 * NULL (nothing pushed) when the job is idle or its plugin is gone. */
static lua_State * plugin_picker_job_take(plugin_picker_job_t * job) {
    if (!job->active) return NULL;
    lua_State * L = job->L;
    int ref = job->callback_ref;
    if (!L || ref == LUA_NOREF || plugin_state_frozen(L)) {
        plugin_picker_job_release(job, false);
        return NULL;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    plugin_picker_job_release(job, false);
    return L;
}

static void plugin_picker_job_call(lua_State * L, const char * what) {
    plugin_call_report(L, 1, false, "%s picker callback error", what);
}

/* plugin.pick_file([folder], callback) -> true | nil, error. */
static int l_plugin_pick_file(lua_State * L) {
    const char * folder = luaL_optstring(L, 1, NULL);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (folder && !plugin_picker_folder_valid(folder))
        return luaL_error(L, "plugin.pick_file: folder must be on the SD card");
    if (plugin_file_picker.active) return plugin_lua_push_error(L, "another file picker is already open");
    if (!gui_plugin_pick_file(folder)) return plugin_lua_push_error(L, "could not open that folder");
    plugin_picker_job_begin(&plugin_file_picker, L, 2);
    lua_pushboolean(L, true);
    return 1;
}

void plugin_manager_file_picker_complete(const char * path) {
    lua_State * L = plugin_picker_job_take(&plugin_file_picker);
    if (!L) return;
    if (path && plugin_song_path_valid(path)) lua_pushstring(L, path);
    else lua_pushnil(L);
    plugin_picker_job_call(L, "file");
}

/* A color as 0xRRGGBB, "#RRGGBB" or "RRGGBB". */
static bool plugin_color_arg(lua_State * L, int idx, uint32_t * rgb) {
    if (lua_type(L, idx) == LUA_TSTRING) return gui_color_parse_hex(lua_tostring(L, idx), rgb);
    int isnum = 0;
    lua_Integer v = lua_tointegerx(L, idx, &isnum);
    if (!isnum || v < 0 || v > 0xFFFFFF) return false;
    *rgb = (uint32_t) v;
    return true;
}

#define PLUGIN_COLOR_PRESETS_MAX 12

/* plugin.pick_color([default], [presets], callback) -> true | nil, error. */
static int l_plugin_pick_color(lua_State * L) {
    uint32_t rgb = 0xFFFFFF;
    uint32_t presets[PLUGIN_COLOR_PRESETS_MAX];
    int count = 0;
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!lua_isnil(L, 1) && !plugin_color_arg(L, 1, &rgb))
        return luaL_error(L, "plugin.pick_color: default must be 0xRRGGBB or \"#RRGGBB\"");
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_Integer len = luaL_len(L, 2);
        if (len < 0 || len > PLUGIN_COLOR_PRESETS_MAX)
            return luaL_error(L, "plugin.pick_color: at most %d presets", PLUGIN_COLOR_PRESETS_MAX);
        count = (int) len;
        for (int i = 0; i < count; i++) {
            lua_rawgeti(L, 2, i + 1);
            bool ok = plugin_color_arg(L, -1, &presets[i]);
            lua_pop(L, 1);
            if (!ok) return luaL_error(L, "plugin.pick_color: preset %d must be 0xRRGGBB or \"#RRGGBB\"", i + 1);
        }
    }
    if (plugin_color_picker.active) return plugin_lua_push_error(L, "another color picker is already open");
    if (!gui_plugin_pick_color(rgb, count ? presets : NULL, count)) return plugin_lua_push_error(L, "could not open the color picker");
    plugin_picker_job_begin(&plugin_color_picker, L, 3);
    lua_pushboolean(L, true);
    return 1;
}

void plugin_manager_color_picker_complete(int32_t rgb) {
    lua_State * L = plugin_picker_job_take(&plugin_color_picker);
    if (!L) return;
    if (rgb >= 0) lua_pushinteger(L, rgb);
    else lua_pushnil(L);
    plugin_picker_job_call(L, "color");
}

/* Forgets the callback without stopping the operation, which the user may
 * still want to finish (plugin unload, failed load, deinit). */
void plugin_file_op_release(void) {
    plugin_file_op_t * job = &plugin_file_op;
    plugin_unref(job->L, job->callback_ref);
    job->callback_ref = LUA_NOREF;
    job->L = NULL;
}

/* Delivers the callback once the plugin's operation has ended. */
void plugin_file_op_poll(void) {
    plugin_file_op_t * job = &plugin_file_op;
    if (!job->active) return;
    file_op_status_t status;
    file_op_get_status(&status);
    if (status.running && status.id == job->id) return;
    job->active = false;
    gui_plugin_refresh_file_browsers();
    lua_State * L = job->L;
    int ref = job->callback_ref;
    if (!L || ref == LUA_NOREF || plugin_state_frozen(L)) {
        plugin_file_op_release();
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    plugin_file_op_release();
    /* A newer operation already replaced the result (it ended and another
     * began between two polls). */
    if (status.id == job->id) push_file_op_status(L, &status);
    else lua_pushnil(L);
    plugin_call_report(L, 1, false, "file operation callback error");
}

/* plugin.mkdir(path) -> true | nil, error. mkdir -p semantics: existing
 * directories are success, while an existing non-directory remains an
 * error. This intentionally follows Lua's existing unrestricted path-based
 * file operations; plugins are not confined to sd_root() today. */
static int l_plugin_mkdir(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.mkdir");
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
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.play_file");
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
    char path_real[PATH_MAX];
    const char * base = plugin_path_under_root(PLAYLISTS_DIR, path, false, path_real);
    if (!base || !base[0] || strchr(base, '/')) return false;
    const char * ext = strrchr(base, '.');
    if (!ext || (strcasecmp(ext, ".m3u") != 0 && strcasecmp(ext, ".m3u8") != 0)) return false;
    struct stat st;
    return stat(path_real, &st) == 0 && S_ISREG(st.st_mode);
}

bool plugin_song_path_valid(const char * path) {
    if (!path || plugin_storage_path_is_reserved(path)) return false;
    char path_real[PATH_MAX];
    if (!plugin_path_under_root(MUSIC_ROOT_DIR, path, false, path_real)) return false;
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

const luaL_Reg plugin_files_funcs[] = {
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
    { "pick_color",                l_plugin_pick_color },
    { "play_file",                 l_plugin_play_file },
    { "play_list",                 l_plugin_play_list },
    { NULL, NULL }
};
