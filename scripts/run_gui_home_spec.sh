#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_HOME_BASELINE_SOURCE:-}"
build_dir="${GUI_HOME_SPEC_BUILD_DIR:-$repo_root/build_ui_test/home}"
baseline_trace="${GUI_HOME_BASELINE_TRACE:-$build_dir/home_baseline.trace}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    echo "GUI Home spec requires cached build_host/lvgl and build_host/ui/ui_fonts.o" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/gui_home.c" && -f "$repo_root/src/ui/gui_home.h" && \
      -f "$repo_root/src/ui/home_background.c" && -f "$repo_root/src/ui/home_background.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "Home modules are not present; set GUI_HOME_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi

if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen Home baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/home_baseline_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")


def matching_brace(text, opening):
    depth, state, escaped, i = 0, "code", False, opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif ch == "/" and nxt == "/": state = "line"; i += 1
        elif ch == "/" and nxt == "*": state = "block"; i += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced function body")


def function(name):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", source):
        opening = source.find("(", match.start())
        depth, i = 1, opening + 1
        while i < len(source) and depth:
            if source[i] == "(": depth += 1
            elif source[i] == ")": depth -= 1
            i += 1
        while i < len(source) and source[i].isspace(): i += 1
        if i < len(source) and source[i] == "{":
            start = source.rfind("\n", 0, match.start()) + 1
            end = matching_brace(source, i)
            return source[start:end + 1] + "\n"
    raise RuntimeError(f"function definition not found: {name}")


start = source.index("/* Published order for plugin.set_home_layout()")
end = source.index("/* ---- Reset PEQ to defaults", start)
home_body = source[start:end]
callbacks = "\n".join(function(name) for name in (
    "music_tile_cb", "stream_media_tile_cb", "wireless_tile_cb",
    "settings_tile_cb", "dac_home_tile_cb"))
fixture = (
    "/* Extracted verbatim from frozen gui_settings.c for characterization. */\n"
    + callbacks + "\n" + home_body
)
Path(output_path).write_text(fixture, encoding="utf-8")
PY
fi

if [[ "$final_available" == 1 ]]; then
    python3 - "$repo_root/src/ui/gui_settings.c" "$repo_root/src/ui/home_background.c" \
        "$repo_root/src/ui/gui_home.c" "$build_dir/home_final_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

settings_path, background_path, home_path, output_path = map(Path, sys.argv[1:])
settings = settings_path.read_text(encoding="utf-8")


def matching_brace(text, opening):
    depth, state, escaped, i = 0, "code", False, opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif ch == "/" and nxt == "/": state = "line"; i += 1
        elif ch == "/" and nxt == "*": state = "block"; i += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced live function")


def function(name):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", settings):
        opening = settings.find("(", match.start())
        depth, i = 1, opening + 1
        while i < len(settings) and depth:
            if settings[i] == "(": depth += 1
            elif settings[i] == ")": depth -= 1
            i += 1
        while i < len(settings) and settings[i].isspace(): i += 1
        if i < len(settings) and settings[i] == "{":
            start = settings.rfind("\n", 0, match.start()) + 1
            end = matching_brace(settings, i)
            return settings[start:end + 1] + "\n"
    raise RuntimeError(f"live function definition not found: {name}")


callbacks = "\n".join(function(name) for name in ("settings_tile_cb", "stream_media_tile_cb"))
wrapper = function("build_home_screen")
fixture = (
    "/* Live compatibility wrapper/callbacks plus the actual extracted modules. */\n"
    + callbacks + "\n" + wrapper + "\n"
    + "#define calloc spec_calloc\n#define malloc spec_malloc\n#define free spec_free\n"
    + f'#include "{background_path}"\n#include "{home_path}"\n'
    + "#undef calloc\n#undef malloc\n#undef free\n"
)
output_path.write_text(fixture, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and Home modules; cached LVGL/font objects are uninstrumented."
fi

build_run() {
    local mode="$1" exe="$build_dir/home_${1}_spec" source_path="$repo_root/src/ui/gui_home_spec.c"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DHOME_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$source_path" "${lvgl_objects[@]}" \
        "$repo_root/build_host/ui/ui_fonts.o" -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && "$exe") > "$build_dir/home_${mode}_spec.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    mkdir -p "$(dirname "$baseline_trace")"
    cp "$build_dir/home_baseline_spec.trace" "$baseline_trace"
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$baseline_trace" "$build_dir/home_final_spec.trace"
        echo "GUI Home characterization matches frozen baseline"
        cat "$build_dir/home_final_spec.trace"
    else
        echo "Frozen baseline Home characterization completed: $baseline_trace"
        cat "$build_dir/home_baseline_spec.trace"
    fi
else
    build_run final
    echo "GUI Home final characterization passed"
    cat "$build_dir/home_final_spec.trace"
fi
