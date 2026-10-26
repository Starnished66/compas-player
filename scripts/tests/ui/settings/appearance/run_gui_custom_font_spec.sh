#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
baseline_source="${GUI_CUSTOM_FONT_BASELINE_SOURCE:-}"
build_dir="${GUI_CUSTOM_FONT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/custom_font}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/text/fonts/ui_fonts.o" ]]; then
    echo "GUI custom font spec requires cached build_host/lvgl and build_host/ui/text/fonts/ui_fonts.o" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/appearance/fonts/gui_custom_font.c" && -f "$repo_root/src/ui/appearance/fonts/gui_custom_font.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "GUI custom font module is not present; set GUI_CUSTOM_FONT_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen custom font baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/custom_font_baseline_fixture.c" <<'PY'
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
    raise RuntimeError("unbalanced C body in frozen custom-font baseline")


def function(text, name):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        opening = text.find("(", match.start())
        depth, i = 1, opening + 1
        while i < len(text) and depth:
            if text[i] == "(": depth += 1
            elif text[i] == ")": depth -= 1
            i += 1
        while i < len(text) and text[i].isspace(): i += 1
        if i < len(text) and text[i] == "{":
            start = text.rfind("\n", 0, match.start()) + 1
            return text[start:matching_brace(text, i) + 1] + "\n"
    raise RuntimeError(f"frozen custom-font function not found: {name}")


state_match = re.search(r"^static lv_obj_t \* custom_font_screen;\s*$", source, re.M)
if not state_match:
    raise RuntimeError("frozen custom-font screen state declaration not found")
state = state_match.group(0) + "\n"
start = source.index("static lv_obj_t * custom_font_list = NULL;")
end_marker = "/* \"45s\" below 1 minute"
end = source.index(end_marker, start)
block = source[start:end]
getter = function(source, "gui_settings_get_custom_font_screen")
output_path.write_text(
    "/* Exact picker state/functions and compatibility getter sliced from frozen gui_settings.c. */\n"
    + state + block + getter,
    encoding="utf-8")
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
    echo "Sanitizers cover the spec and included picker code; cached LVGL/font objects are uninstrumented."
fi

build_run() {
    local mode="$1"
    local exe="$build_dir/custom_font_${mode}_spec"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DCUSTOM_FONT_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$repo_root/src/ui/appearance/fonts/tests/gui_custom_font_spec.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/text/fonts/ui_fonts.o" \
        "$repo_root/build_host/mbedtls/sha256.o" \
        "$repo_root/build_host/mbedtls/platform_util.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$exe") \
        > "$build_dir/custom_font_${mode}_spec.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$build_dir/custom_font_baseline_spec.trace" "$build_dir/custom_font_final_spec.trace"
        echo "GUI custom font characterization matches frozen baseline"
        cat "$build_dir/custom_font_final_spec.trace"
    else
        echo "Frozen baseline GUI custom font characterization passed"
        cat "$build_dir/custom_font_baseline_spec.trace"
    fi
else
    build_run final
    echo "GUI custom font final characterization passed"
    cat "$build_dir/custom_font_final_spec.trace"
fi
