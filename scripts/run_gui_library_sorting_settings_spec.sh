#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GUI_LIBRARY_SORTING_SETTINGS_BUILD_DIR:-$HOME/.cache/compas-theme-review/release-qa-20261025/gui-library-sorting}"
mkdir -p "$build_dir"
if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "library sorting settings spec requires cached build_host/lvgl; run make host BOARD=r1 first" >&2
    exit 2
fi

python3 - "$repo_root/src/ui/gui_settings.c" "$build_dir/library_sorting_settings_fixture.inc" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")

def brace_end(text, opening):
    depth, state, escaped, i = 0, "code", False, opening
    while i < len(text):
        ch = text[i]; nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"): state = "code"
        elif ch == "/" and nxt == "/": state = "line"; i += 1
        elif ch == "/" and nxt == "*": state = "block"; i += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced source function")

def function(name):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", source):
        opening = source.find("(", match.start()); depth, i = 1, opening + 1
        while i < len(source) and depth:
            if source[i] == "(": depth += 1
            elif source[i] == ")": depth -= 1
            i += 1
        while i < len(source) and source[i].isspace(): i += 1
        if i < len(source) and source[i] == "{":
            start = source.rfind("\n", 0, match.start()) + 1
            return source[start:brace_end(source, i) + 1] + "\n"
    raise RuntimeError("missing production function: " + name)

enum_start = source.index("enum { SORTING_FILE_OPTION_COUNT")
enum_end = source.index("static lv_obj_t * settings_sound_effects_screen", enum_start)
state = re.search(r"^static lv_obj_t \* settings_sorting_screen;\s*$", source, re.M)
if not state: raise RuntimeError("missing sorting screen state")
parts = ["/* Exact sorting UI state/constants and callbacks sliced from gui_settings.c. */\n",
         state.group(0) + "\n", source[enum_start:enum_end].rstrip() + "\n"]
parts += [function(name) for name in ("refresh_sorting_options", "sorting_option_cb",
                                        "build_sorting_screen", "sorting_row_cb")]
output_path.write_text("\n".join(parts), encoding="utf-8")
PY

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
exe="$build_dir/gui_library_sorting_settings_spec"
"$cc" "${flags[@]}" "$repo_root/tests/gui_library_sorting_settings_spec.c" \
    "${lvgl_objects[@]}" -Wl,--gc-sections -lpthread -lm -o "$exe"
run_dir="$build_dir/run"
mkdir -p "$run_dir"
timeout --signal=TERM --kill-after=2s 30s \
    bash -c 'cd "$1" && ASAN_OPTIONS="$2" "$3"' _ "$run_dir" \
    "${ASAN_OPTIONS:-detect_leaks=0}" "$exe"
