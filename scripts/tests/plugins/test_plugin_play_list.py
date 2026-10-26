#!/usr/bin/env python3
"""Run the production plugin.play_list binding against real Lua."""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[3]
baseline = len(sys.argv) == 2 and sys.argv[1] == '--baseline'
if len(sys.argv) > 2 or (len(sys.argv) == 2 and not baseline):
    raise SystemExit('usage: test_plugin_play_list.py [--baseline]')
if baseline:
    src = subprocess.run(['git', 'show', 'HEAD:src/plugins/manager/plugin_manager.c'], cwd=root,
                         check=True, text=True, capture_output=True).stdout
else:
    src = (root / 'src/plugins/manager/plugin_manager.c').read_text()

def function(name):
    match = re.search(r'^static (?:int|bool) ' + name + r'\([^\n]*\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()

body = r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PLUGIN_MAX_LIST_ITEMS 4
static int calls, last_count, last_start;
static bool last_open;
static char played[PLUGIN_MAX_LIST_ITEMS][512];
static bool plugin_storage_path_is_reserved(const char *path) {
    return path && (strcmp(path, ".plugins/secret") == 0 || strncmp(path, ".plugins/secrets/", 17) == 0);
}
static void gui_plugin_play_paths(const char *const *paths, int count, int start, bool open) {
    assert(count >= 0 && count <= PLUGIN_MAX_LIST_ITEMS);
    calls++;
    last_count = count;
    last_start = start;
    last_open = open;
    for (int i = 0; i < count; i++) snprintf(played[i], sizeof(played[i]), "%s", paths[i]);
}
''' + function('play_opens_player') + '\n' + function('l_plugin_play_list') + r'''
static void bind(lua_State *L) {
    lua_pushcfunction(L, l_plugin_play_list);
    lua_setglobal(L, "play_list");
}
static void run(lua_State *L, const char *code) {
    int rc = luaL_dostring(L, code);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(rc == LUA_OK);
}
int main(void) {
    lua_State *L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    bind(L);

#ifdef TEST_BASELINE
    /* The pre-fix two-argument call mistakes userdata at stack index 3 for
     * the absent options argument and errors before native playback. */
    int baseline_rc = luaL_dostring(L, "play_list({'/sd/Audiobooks/Book/01.mp3'}, 1)");
    assert(baseline_rc != LUA_OK && calls == 0);
    assert(strstr(lua_tostring(L, -1), "table expected") != NULL);
    lua_close(L);
    puts("pre-fix plugin.play_list failure reproduced");
    return 0;
#endif

    /* Audiobooks' exact two-argument call must reach native playback. */
    run(L, "play_list({'/sd/Audiobooks/Book/01.mp3','/sd/Audiobooks/Book/02.mp3'}, 2)");
    assert(calls == 1 && last_count == 2 && last_start == 1 && last_open);
    assert(strcmp(played[0], "/sd/Audiobooks/Book/01.mp3") == 0);
    assert(strcmp(played[1], "/sd/Audiobooks/Book/02.mp3") == 0);

    /* One argument and explicit nil preserve the default start and UI behavior. */
    run(L, "play_list({'one'})");
    assert(calls == 2 && last_count == 1 && last_start == 0 && last_open);
    run(L, "play_list({'one'}, nil, nil)");
    assert(calls == 3 && last_count == 1 && last_start == 0 && last_open);

    /* The optional options table controls whether Now Playing opens. */
    run(L, "play_list({'one'}, 1, {open_player=false})");
    assert(calls == 4 && last_open == false);
    run(L, "play_list({'one'}, 1, {open_player=true})");
    assert(calls == 5 && last_open == true);

    /* Invalid options fail before native dispatch, including when buffers are needed. */
    run(L, "assert(not pcall(play_list, {'one'}, 1, false))");
    assert(calls == 5);

    /* Reserved storage paths remain rejected and never dispatch. */
    run(L, "assert(not pcall(play_list, {'.plugins/secrets/private'}, 1))");
    assert(calls == 5);

    /* Preserve path-count limiting and start-index clamping. */
    run(L, "play_list({'a','b','c','d','e'}, 99)");
    assert(calls == 6 && last_count == PLUGIN_MAX_LIST_ITEMS && last_start == PLUGIN_MAX_LIST_ITEMS - 1);
    run(L, "play_list({'a','b'}, 0)");
    assert(calls == 7 && last_start == 0);

    lua_close(L);
    puts("plugin.play_list binding passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='compas-play-list-') as folder:
    folder = pathlib.Path(folder)
    c_file = folder / 'test.c'
    binary = folder / 'test'
    c_file.write_text(body)
    lua = [str(p) for p in (root / 'lua/src').glob('*.c') if p.name not in ('lua.c', 'luac.c')]
    flags = ['-DTEST_BASELINE'] if baseline else []
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DLUA_USE_LINUX', *flags,
                    '-I' + str(root / 'lua/src'), str(c_file), *lua, '-lm', '-ldl', '-o', str(binary)],
                   check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
