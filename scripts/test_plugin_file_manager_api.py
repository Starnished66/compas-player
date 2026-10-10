#!/usr/bin/env python3
"""Run the plugin File Manager bindings against the real file_ops.c engine,
with the UI and plugin-instance edges replaced by test doubles."""
import pathlib, re, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'src/plugins/plugin_manager.c').read_text()
match = re.search(r'^/\* ---- File Manager for plugins \(API 16\) ----.*?(?=^/\* plugin\.mkdir\(path\))', source, re.M | re.S)
if not match:
    raise RuntimeError('missing production File Manager bindings')
bindings = match.group(0)
names = ['l_plugin_file_copy', 'l_plugin_file_move', 'l_plugin_file_delete', 'l_plugin_file_operation_status',
         'l_plugin_cancel_file_operation', 'l_plugin_open_file_manager']
prefix = r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "file_ops.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct { bool aborted; } plugin_instance_t;
static bool usb_blocked;
static int refreshes;
static char opened[512] = "unset";
static plugin_instance_t *plugin_instance_for_state(lua_State *L) { (void)L; return NULL; }
static lua_State *plugin_main_state(lua_State *L) { return L; }
static int plugin_call(lua_State *L, int nargs, int nresults, int errfunc) {
    int status = lua_pcall(L, nargs, nresults, errfunc);
    if (status != LUA_OK) { fprintf(stderr, "callback failed: %s\n", lua_tostring(L, -1)); assert(!"callback failed"); }
    return status;
}
static int push_plugin_error(lua_State *L, const char *message) { lua_pushnil(L); lua_pushstring(L, message); return 2; }
bool usb_mode_control_storage_write_blocked(void) { return usb_blocked; }
bool usb_mode_control_storage_write_begin(void) { return true; }
void usb_mode_control_storage_write_end(void) {}
bool file_browser_is_playable_name(const char *name) { const char *e = strrchr(name, '.'); return e && !strcmp(e, ".flac"); }
bool gui_plugin_open_file_manager(const char *folder) { snprintf(opened, sizeof(opened), "%s", folder ? folder : "(none)"); return true; }
void gui_plugin_refresh_file_browsers(void) { refreshes++; }
'''
helpers = r'''
/* Lua: wait() polls until this plugin's operation has delivered. */
static int l_wait(lua_State *L) {
    for (int i = 0; i < 5000 && plugin_file_op.active; i++) {
        plugin_file_op_poll();
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }
    lua_pushboolean(L, !plugin_file_op.active);
    return 1;
}
static int l_set_usb_blocked(lua_State *L) { usb_blocked = lua_toboolean(L, 1); return 0; }
static int l_opened(lua_State *L) { lua_pushstring(L, opened); return 1; }
static int l_refreshes(lua_State *L) { lua_pushinteger(L, refreshes); return 1; }
'''
script = r'''
local root = ROOT
local function write(path, text) local f = assert(io.open(path, "w")); f:write(text); f:close() end
local function read(path) local f = io.open(path, "r"); if not f then return nil end local t = f:read("a"); f:close(); return t end
os.execute("mkdir -p '" .. root .. "/src/sub' '" .. root .. "/dst' '" .. root .. "/.compas'")
write(root .. "/src/a.txt", "alpha")
write(root .. "/src/sub/b.flac", "beta")

local got
assert(file_copy({ root .. "/src/a.txt", root .. "/src/sub" }, root .. "/dst", function(r) got = r end) == true)
assert(wait() and got and got.done == 2 and got.total == 2 and got.failed == 0 and not got.stopped and not got.running)
assert(read(root .. "/dst/a.txt") == "alpha" and read(root .. "/dst/sub/b.flac") == "beta")
assert(refreshes() == 1)

got = nil
assert(file_copy(root .. "/src/a.txt", root .. "/dst", function(r) got = r end))
assert(wait() and got.failed == 0 and read(root .. "/dst/a (2).txt") == "alpha")

got = nil
assert(file_move(root .. "/dst/a (2).txt", root, function(r) got = r end))
assert(wait() and got.failed == 0 and read(root .. "/a (2).txt") == "alpha" and not read(root .. "/dst/a (2).txt"))

assert(file_delete({ root .. "/a (2).txt", root .. "/dst" }))  -- no callback
assert(wait() and not read(root .. "/a (2).txt") and not read(root .. "/dst/a.txt"))
local status = file_operation_status()
assert(status.done == 2 and status.failed == 0 and status.running == false)

-- The player's own folders cannot be deleted or moved, only copied.
local ok, err = file_delete(root .. "/.compas")
assert(ok == nil and err:find("player"))
ok, err = file_move({ root .. "/src/a.txt", root .. "/.compas" }, root .. "/src/sub")
assert(ok == nil and err:find("player"))

-- Paths must stay on the card; destinations may be the card root.
assert(not pcall(file_delete, "/etc/passwd"))
assert(not pcall(file_delete, root))
assert(not pcall(file_delete, root .. "/src/../src"))
-- Aliases of the card root or of a folder would widen what is touched.
for _, alias in ipairs({ root .. "/.", root .. "//", root .. "/./src", root .. "/src/", root .. "//src" }) do
    assert(not pcall(file_delete, alias), alias)
end
assert(not pcall(file_copy, root .. "/src/", root .. "/src/sub"))
assert(not pcall(open_file_manager, root .. "/."))
ok, err = file_delete(root .. "/.Compas")
assert(ok == nil and err:find("player"))
assert(not pcall(file_copy, { root .. "/src/a.txt", 5 }, root))
assert(not pcall(file_copy, root .. "/src/a.txt", "/tmp"))
assert(not pcall(file_copy, {}, root))

set_usb_blocked(true)
ok, err = file_copy(root .. "/src/a.txt", root)
assert(ok == nil and err:find("USB"))
set_usb_blocked(false)

assert(cancel_file_operation() == false)
assert(open_file_manager(root .. "/src") == true and opened() == root .. "/src")
assert(open_file_manager() == true and opened() == "(none)")
assert(not pcall(open_file_manager, "/usr/data"))

assert(file_delete(root .. "/src"))
assert(wait())
'''
def run(card_root, cwd, folder):
    """card_root is MUSIC_ROOT_DIR: absolute like the device, or relative like
    the simulator's "./music"."""
    c = folder / 'bindings.c'
    register = '\n'.join(f'    lua_pushcfunction(L, {n}); lua_setglobal(L, "{n.removeprefix("l_plugin_")}");' for n in names)
    lua_script = script.replace('ROOT', '"' + card_root + '"')
    c.write_text(prefix + bindings + helpers + '\nint main(void) {\n    lua_State *L = luaL_newstate(); assert(L); luaL_openlibs(L);\n'
                 + register + '''
    lua_register(L, "wait", l_wait);
    lua_register(L, "set_usb_blocked", l_set_usb_blocked);
    lua_register(L, "opened", l_opened);
    lua_register(L, "refreshes", l_refreshes);
    if (luaL_dostring(L, ''' + '"' + lua_script.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '''")) {
        fprintf(stderr, "%s\\n", lua_tostring(L, -1));
        return 1;
    }
    lua_close(L);
    return 0;
}
''')
    lua = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    cmd = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-pthread', '-DLUA_USE_LINUX',
           '-DMUSIC_ROOT_DIR="' + card_root + '"',
           '-I' + str(root), '-I' + str(root / 'lua/src'), '-I' + str(root / 'src/library'), '-I' + str(root / 'src/hardware'),
           str(c), str(root / 'src/library/file_ops.c'), *lua, '-lm', '-ldl', '-o', str(folder / 'test')]
    subprocess.run(cmd, check=True, cwd=root)
    subprocess.run([str(folder / 'test')], check=True, cwd=cwd)

with tempfile.TemporaryDirectory(prefix='compas-plugin-files-') as folder:
    folder = pathlib.Path(folder)
    (folder / 'card').mkdir()
    run(str(folder / 'card'), root, folder)
    run('./card', folder, folder)
    print('Lua File Manager API binding tests passed')
