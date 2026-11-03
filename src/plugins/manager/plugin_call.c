/* Plugin call budget, hard-abort handling, callback dispatch helpers and the Lua sandbox. */
#include "../../ui/application/gui.h"
#include "../storage/plugin_reserved_dirs.h"
#include "../lua/plugin_lua_runtime.h"
#include "../storage/plugin_storage.h"
#include <string.h>
#include <setjmp.h>
#include "plugin_manager_internal.h"
/* Lifetime of an aborted plugin: its lua_State is frozen mid-execution after
 * a hard time-budget abort (plugin_call()) and is never run, inspected,
 * unref'd or lua_close()d again -- it is leaked on purpose, so every ref held
 * into it simply dies with the leak. Every Lua-registry release below goes
 * through plugin_unref(), and every callback delivery checks
 * plugin_state_frozen() first. */
bool plugin_state_frozen(lua_State * L) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    return inst && inst->aborted;
}

void plugin_unref(lua_State * L, int ref) {
    if (L && !plugin_state_frozen(L)) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

/* Callback dispatch gate: pushes the callback and returns true, or returns
 * false without touching the state when its plugin is frozen. Every dispatch
 * pairs this with plugin_call_report() below. */
bool plugin_push_callback(lua_State * L, int ref) {
    if (plugin_state_frozen(L)) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return true;
}
bool plugin_call_report(lua_State * L, int nargs, bool user_action, const char * fmt, ...) {
    int rc = user_action ? plugin_call_user_action(L, nargs, 0, 0) : plugin_call(L, nargs, 0, 0);
    if (rc == LUA_OK) return true;
    if (plugin_state_frozen(L)) return false;
    const char * err = lua_tostring(L, -1);
    fputs("[plugins] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", err ? err : "unknown error");
    lua_pop(L, 1);
    return false;
}

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

void plugin_lib_add(lua_State * L, const luaL_Reg * regs) {
    for (; regs->name != NULL; regs++) {
        lua_pushcfunction(L, regs->func);
        lua_pushcclosure(L, l_plugin_api_guard, 1);
        lua_setfield(L, -2, regs->name);
    }
}

void register_plugin_lib(lua_State * L, const luaL_Reg * regs) {
    lua_newtable(L);
    plugin_lib_add(L, regs);
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

void sandbox_plugin_lua_state(lua_State * L) {
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
int plugin_call(lua_State * L, int nargs, int nresults, int errfunc) {
    plugin_instance_t * inst = plugin_instance_for_state(L);
    /* A frozen state is never touched, not even to push an error: callers
     * read an error only when plugin_state_frozen() is false. */
    if (inst && inst->aborted) return LUA_ERRRUN;

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
        /* Hard-aborted from the hook. Mark the plugin dead and never touch
         * this state again: no settop/pop/push (lua_settop would run the
         * __close handlers of interrupted <close> locals with the abort
         * protection already disarmed), no lua_close(). It is frozen and
         * leaked, the same lifetime every other plugin state already has in
         * this file; callers see plugin_state_frozen() and skip the error. */
        if (inst) {
            inst->aborted = true;
            fprintf(stderr, "[plugins] %s: stopped, a call ran past its time limit\n",
                    inst->id[0] ? inst->id : "plugin");
            remote_track_stream_cancel_owner((uintptr_t) L);
        }
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

int plugin_call_user_action(lua_State * L, int nargs, int nresults, int errfunc) {
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
