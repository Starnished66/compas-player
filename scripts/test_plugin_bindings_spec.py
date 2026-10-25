#!/usr/bin/env python3
"""Characterize the plugin info and synchronous library Lua bindings."""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
mode = sys.argv[1] if len(sys.argv) == 2 else "--current"
if mode not in ("--baseline", "--current", "--compare"):
    raise SystemExit("usage: test_plugin_bindings_spec.py [--baseline|--current|--compare]")

baseline_name = os.environ.get("PLUGIN_BINDINGS_BASELINE_SOURCE", "")
if mode in ("--baseline", "--compare") and not baseline_name:
    raise SystemExit("set PLUGIN_BINDINGS_BASELINE_SOURCE to the frozen plugin_manager.c source")
baseline_path = Path(baseline_name) if baseline_name else None
if baseline_path and not baseline_path.is_file():
    raise SystemExit(f"frozen plugin binding baseline does not exist: {baseline_path}")

build_dir = Path(os.environ.get("PLUGIN_BINDINGS_SPEC_BUILD_DIR", root / "build_ui_test/plugin-bindings"))
build_dir.mkdir(parents=True, exist_ok=True)


def mask_comments_and_literals(text: str) -> str:
    """Blank comments and quoted literals without changing offsets/newlines."""
    result = list(text)
    state = "code"
    escaped = False
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n":
                state = "code"
            elif ch != "\r":
                result[i] = " "
        elif state == "block":
            if ch == "*" and nxt == "/":
                result[i] = result[i + 1] = " "
                state = "code"
                i += 1
            elif ch not in "\r\n":
                result[i] = " "
        elif state in ("string", "char"):
            if ch not in "\r\n":
                result[i] = " "
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif ch == "/" and nxt == "/":
            result[i] = result[i + 1] = " "
            state = "line"
            i += 1
        elif ch == "/" and nxt == "*":
            result[i] = result[i + 1] = " "
            state = "block"
            i += 1
        elif ch == '"':
            result[i] = " "
            state = "string"
        elif ch == "'":
            result[i] = " "
            state = "char"
        i += 1
    return "".join(result)


def function_definition(text: str, name: str) -> str:
    masked = mask_comments_and_literals(text)
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", masked):
        opening = masked.find("(", match.start())
        depth = 1
        i = opening + 1
        while i < len(masked) and depth:
            if masked[i] == "(":
                depth += 1
            elif masked[i] == ")":
                depth -= 1
            i += 1
        if depth:
            continue
        while i < len(masked) and masked[i].isspace():
            i += 1
        if i >= len(masked) or masked[i] != "{":
            continue
        start = text.rfind("\n", 0, match.start()) + 1
        brace_depth = 1
        end = i + 1
        while end < len(masked) and brace_depth:
            if masked[end] == "{":
                brace_depth += 1
            elif masked[end] == "}":
                brace_depth -= 1
            end += 1
        if brace_depth:
            raise RuntimeError(f"unterminated function body: {name}")
        return text[start:end].rstrip() + "\n"
    raise RuntimeError(f"missing function definition: {name}")


def check_registration(manager: str) -> None:
    names = {
        "api_version": "l_plugin_api_version",
        "has_capability": "l_plugin_has_capability",
        "get_app_info": "l_plugin_get_app_info",
        "media_capabilities": "l_plugin_media_capabilities",
        "get_artist_albums": "l_plugin_get_artist_albums",
        "get_album_tracks": "l_plugin_get_album_tracks",
        "get_next_album_tracks": "l_plugin_get_next_album_tracks",
        "library_song_count": "l_plugin_library_song_count",
        "library_get_songs": "l_plugin_library_get_songs",
        "library_search": "l_plugin_library_search",
        "library_get_song": "l_plugin_library_get_song",
        "library_get_artists": "l_plugin_library_get_artists",
        "library_get_albums": "l_plugin_library_get_albums",
    }
    table_start = manager.index("static const luaL_Reg plugin_funcs[] = {")
    table_end = manager.index("\n};", table_start) + 3
    table = manager[table_start:table_end]
    for public_name, function_name in names.items():
        expected = f'{{ "{public_name}"'
        if table.count(expected) != 1 or function_name not in table:
            raise RuntimeError(f"plugin_funcs[] registration missing/duplicate: {public_name} -> {function_name}")
    register = function_definition(manager, "register_plugin_api")
    if register.count("register_plugin_lib(L, plugin_funcs)") != 1:
        raise RuntimeError("register_plugin_api no longer registers the single guarded plugin_funcs table")
    guarded = function_definition(manager, "register_plugin_lib")
    if "lua_pushcclosure(L, l_plugin_api_guard, 1)" not in guarded:
        raise RuntimeError("plugin function registration bypasses l_plugin_api_guard")


def make_fixture(source: str, fixture_path: Path) -> None:
    info = [function_definition(source, n) for n in (
        "l_plugin_api_version", "l_plugin_has_capability", "l_plugin_media_capabilities", "l_plugin_get_app_info")]
    cap_start = source.index("static const char * const plugin_capabilities[] = {")
    cap_end = source.index("/* plugin.media_capabilities()", cap_start)
    capability_block = source[cap_start:cap_end]
    strings_start = source.index("static int push_string_array_result(lua_State * L, char ** items, int count) {")
    strings_end = source.index("/* ---- plugin.playlist_*", strings_start)
    strings_block = source[strings_start:strings_end]
    query_start = source.index("/* ---- plugin.library_*", strings_end)
    query_end = source.index("typedef struct {\n    lua_State * L;\n    int callback_ref;\n    int64_t song_id;", query_start)
    query_block = source[query_start:query_end]

    fixture = "/* Exact Lua binding bodies sliced from the frozen plugin_manager.c. */\n"
    # The original static helper body is sliced byte-for-byte. In current
    # mode the implementation TU declares this exact helper in its private
    # sibling header for the remaining playlist callers.
    fixture += "\n".join(info[:1]) + "\n"
    fixture += capability_block + "\n"
    fixture += info[2] + "\n" + info[3] + "\n"
    fixture += strings_block
    fixture += query_block
    fixture_path.write_text(fixture, encoding="utf-8")


def build_and_run(selected_mode: str, manager_source: str) -> str:
    check_registration(manager_source)
    if selected_mode == "--baseline":
        assert baseline_path
        fixture = build_dir / "plugin_bindings_baseline_fixture.c"
        make_fixture(manager_source, fixture)
        defines = ["-DPLUGIN_BINDINGS_BASELINE=1"]
    else:
        modules = (root / "src/plugins/bindings/plugin_info_bindings.c",
                   root / "src/plugins/bindings/plugin_library_bindings.c")
        if not all(path.is_file() for path in modules):
            raise SystemExit("current plugin info/library binding modules are not present yet")
        defines = []

    binary = build_dir / ("plugin_bindings_baseline_spec" if selected_mode == "--baseline" else "plugin_bindings_current_spec")
    lua_sources = sorted(
        path for path in (root / "lua/src").glob("*.c") if path.name not in ("lua.c", "luac.c")
    )
    include_dirs = [root, root / "lua/src", root / "src/plugins", root / "src/plugins/bindings",
                    root / "src/ui", root / "src/audio",
                    root / "src/library", root / "src/core", root / "src/hardware",
                    root / "lvgl", root / "lvgl/src", build_dir]
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1",
               "-DLUA_USE_LINUX", "-DHOST_BUILD=1", *defines]
    command.extend("-I" + str(path) for path in include_dirs)
    command.extend([str(root / "tests/plugin_bindings_spec.c"), *map(str, lua_sources), "-lm", "-ldl", "-o", str(binary)])
    subprocess.run(command, check=True, cwd=root)
    completed = subprocess.run([str(binary)], check=True, text=True, capture_output=True, cwd=build_dir)
    return completed.stdout.strip()


if mode == "--compare":
    assert baseline_path
    baseline_source = baseline_path.read_text(encoding="utf-8")
    current_source = (root / "src/plugins/plugin_manager.c").read_text(encoding="utf-8")
    baseline_out = build_and_run("--baseline", baseline_source)
    current_out = build_and_run("--current", current_source)
    if baseline_out != current_out:
        raise SystemExit(f"baseline/current plugin bindings differ:\nBASELINE: {baseline_out}\nCURRENT: {current_out}")
    print("plugin info/library bindings match frozen baseline")
    print(current_out)
elif mode == "--baseline":
    assert baseline_path
    print(build_and_run("--baseline", baseline_path.read_text(encoding="utf-8")))
else:
    print(build_and_run("--current", (root / "src/plugins/plugin_manager.c").read_text(encoding="utf-8")))
