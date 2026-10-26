#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
baseline_source="${GUI_ACCENT_BASELINE_SOURCE:-}"
build_dir="${GUI_ACCENT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/accent}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/text/fonts/ui_fonts.o" ]]; then
    echo "GUI accent spec requires cached build_host/lvgl and build_host/ui/text/fonts/ui_fonts.o" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/appearance/gui_accent.c" && -f "$repo_root/src/ui/appearance/gui_accent.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "GUI accent module is not present; set GUI_ACCENT_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen accent baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/accent_baseline_fixture.c" <<'PY'
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
    raise RuntimeError("unbalanced C body in frozen accent baseline")


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
    raise RuntimeError(f"frozen accent function not found: {name}")


state = "static lv_obj_t * accent_color_screen;\nstatic lv_obj_t * accent_options_screen;\n"
block_start = source.index("/* ---- Accent Color ----")
block_end = source.index("static lv_obj_t * custom_font_list", block_start)
block = source[block_start:block_end]

init = function(source, "gui_settings_init")
init_calls = [line.strip() for line in init.splitlines()
              if line.strip() in {"accent_options_screen = build_accent_options_screen();",
                                  "accent_color_screen = build_accent_color_screen();"}]
if init_calls != ["accent_options_screen = build_accent_options_screen();",
                  "accent_color_screen = build_accent_color_screen();"]:
    raise RuntimeError(f"unexpected accent screen build order: {init_calls}")

teardown = function(source, "gui_settings_teardown")
start = teardown.index("    if (accent_color_screen) { lv_obj_delete(accent_color_screen); accent_color_screen = NULL; }")
end = teardown.index("    if (custom_font_screen)", start)
cleanup = teardown[start:end]
tail = "accent_sv_area = NULL;"
if not cleanup.rstrip().endswith(tail):
    raise RuntimeError("accent teardown fixture does not end at the SV-area cleanup")

getter = function(source, "gui_settings_get_accent_screen")
fixture = (
    "/* Baseline accent implementation sliced from frozen gui_settings.c. */\n"
    + state + block + getter
    + "\nvoid accent_test_build_screens(void) {\n"
    + "\n".join("    " + call for call in init_calls) + "\n}\n"
    + "void accent_test_teardown(void) {\n" + cleanup + "}\n"
    + "void accent_test_row_cb(lv_event_t * event) { accent_color_row_cb(event); }\n"
)
output_path.write_text(fixture, encoding="utf-8")
PY
fi

if [[ "$final_available" == 1 ]]; then
    python3 - "$repo_root/src/ui/settings/gui_settings.c" "$repo_root/src/ui/appearance/gui_accent.c" \
        "$build_dir/accent_final_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

settings_path, module_path, output_path = map(Path, sys.argv[1:])
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
    raise RuntimeError("unbalanced live accent coordinator")


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
            return settings[start:matching_brace(settings, i) + 1] + "\n"
    raise RuntimeError(f"live parent function not found: {name}")


coordinator = function("gui_settings_accent_changed")
fixture = (
    "/* Exact live parent coordinator plus the extracted accent implementation. */\n"
    + coordinator + "\n"
    + f'#include "{module_path}"\n'
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
    echo "Sanitizers cover the spec and included accent code; cached LVGL/font objects are uninstrumented."
fi

build_run() {
    local mode="$1"
    local exe="$build_dir/accent_${mode}_spec"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DACCENT_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$repo_root/src/ui/appearance/tests/gui_accent_spec.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/text/fonts/ui_fonts.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && "$exe") > "$build_dir/accent_${mode}_spec.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$build_dir/accent_baseline_spec.trace" "$build_dir/accent_final_spec.trace"
        echo "GUI accent characterization matches frozen baseline"
        cat "$build_dir/accent_final_spec.trace"
    else
        echo "Frozen baseline GUI accent characterization passed"
        cat "$build_dir/accent_baseline_spec.trace"
    fi
else
    build_run final
    echo "GUI accent final characterization passed"
    cat "$build_dir/accent_final_spec.trace"
fi
