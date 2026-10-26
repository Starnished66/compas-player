#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
baseline_source="${GUI_DISPLAY_TIMING_BASELINE_SOURCE:-}"
build_dir="${GUI_DISPLAY_TIMING_SPEC_BUILD_DIR:-$repo_root/build_ui_test/display_timing}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/text/fonts/ui_fonts.o" ]]; then
    echo "Display timing spec requires cached build_host/lvgl and build_host/ui/text/fonts/ui_fonts.o" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/settings/display/gui_display_timing_settings.c" && -f "$repo_root/src/ui/settings/display/gui_display_timing_settings.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "Display timing module is not present; set GUI_DISPLAY_TIMING_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen display timing baseline does not exist: $baseline_source" >&2
    exit 2
fi

python3 - "$repo_root/src/ui/widgets/builders/screen_builders.c" "$build_dir/screen_builders_nearest_step_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")
match = re.search(r"\bint\s+find_nearest_step_index\s*\(", source)
if not match:
    raise RuntimeError("screen_builders.c is missing find_nearest_step_index")
opening = source.find("{", match.end())
depth, state, escaped, i = 0, "code", False, opening
while i < len(source):
    ch = source[i]
    nxt = source[i + 1] if i + 1 < len(source) else ""
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
        if depth == 0:
            begin = source.rfind("\n", 0, match.start()) + 1
            output_path.write_text(source[begin:i + 1] + "\n", encoding="utf-8")
            break
    i += 1
else:
    raise RuntimeError("unbalanced find_nearest_step_index in screen_builders.c")
PY

python3 - "$repo_root/src/core/persistence/settings.c" "$build_dir/display_timing_settings_steps_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")
definitions = []
for name in ("SCREEN_TIMEOUT_STEPS", "SCREEN_DIM_DELAY_STEPS"):
    match = re.search(r"^const int " + name + r"\[[^\]]+\]\s*=\s*\{[^}]+\};",
                      source, re.M)
    if not match:
        raise RuntimeError(f"settings.c is missing {name} initializer")
    definitions.append(match.group(0))
output_path.write_text("\n".join(definitions) + "\n", encoding="utf-8")
PY

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$repo_root/src/core/persistence/settings.c" "$build_dir/display_timing_baseline_fixture.c" \
    "$build_dir/display_timing_settings_steps_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

settings_path, data_path, fixture_path, steps_path = map(Path, sys.argv[1:])
source = settings_path.read_text(encoding="utf-8")
data_source = data_path.read_text(encoding="utf-8")


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
    raise RuntimeError("unbalanced C body in frozen display timing baseline")


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
            begin = text.rfind("\n", 0, match.start()) + 1
            return text[begin:matching_brace(text, i) + 1]
    raise RuntimeError(f"frozen display timing function not found: {name}")


state_lines = [line for line in source.splitlines()
               if re.match(r"^static lv_obj_t \* (?:screen_timeout|screen_dimming)_\w+;?$", line)]
if len(state_lines) != 10:
    raise RuntimeError(f"expected ten timeout/dimming widget declarations, found {len(state_lines)}")
start = source.index('/* "45s" below 1 minute')
end = source.index("static void startup_volume_switch_event_cb", start)
block = source[start:end]

init = function(source, "gui_settings_init")
build_calls = [line.strip() for line in init.splitlines()
               if line.strip() in {"screen_timeout_screen = build_screen_timeout_screen();",
                                   "screen_dimming_screen = build_screen_dimming_screen();"}]
if build_calls != ["screen_timeout_screen = build_screen_timeout_screen();",
                   "screen_dimming_screen = build_screen_dimming_screen();"]:
    raise RuntimeError(f"unexpected timeout/dimming build order: {build_calls}")

teardown = function(source, "gui_settings_teardown")
delete_lines = [line for line in teardown.splitlines()
                if "if (screen_timeout_screen)" in line or "if (screen_dimming_screen)" in line]
expected = ["    if (screen_timeout_screen) { lv_obj_delete(screen_timeout_screen); screen_timeout_screen = NULL; }",
            "    if (screen_dimming_screen) { lv_obj_delete(screen_dimming_screen); screen_dimming_screen = NULL; }"]
if delete_lines != expected:
    raise RuntimeError(f"unexpected timeout/dimming teardown: {delete_lines}")

fixture = (
    "/* Exact screen state/functions sliced from frozen gui_settings.c. */\n"
    + "\n".join(state_lines) + "\n"
    + block
    + "void display_timing_test_build_screens(void) {\n"
    + "\n".join("    " + call for call in build_calls) + "\n}\n"
    + "void display_timing_test_teardown(void) {\n"
    + "\n".join(delete_lines) + "\n}\n"
)
fixture_path.write_text(fixture, encoding="utf-8")

step_defs = []
for name in ("SCREEN_TIMEOUT_STEPS", "SCREEN_DIM_DELAY_STEPS"):
    match = re.search(r"^const int " + name + r"\[[^\]]+\]\s*=\s*\{[^}]+\};",
                      data_source, re.M)
    if not match:
        raise RuntimeError(f"settings.c is missing {name} initializer")
    step_defs.append(match.group(0))
steps_path.write_text("\n".join(step_defs) + "\n", encoding="utf-8")
PY
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
# screen_dimming_screen_loaded_cb intentionally ignores its LVGL event argument.
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included timing UI; cached LVGL/font objects are uninstrumented."
fi

build_run() {
    local mode="$1"
    local exe="$build_dir/display_timing_${mode}_spec"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DDISPLAY_TIMING_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$repo_root/src/ui/settings/display/gui_display_timing_settings_spec.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/text/fonts/ui_fonts.o" \
        "$repo_root/build_host/mbedtls/sha256.o" \
        "$repo_root/build_host/mbedtls/platform_util.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$exe") \
        > "$build_dir/display_timing_${mode}_spec.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$build_dir/display_timing_baseline_spec.trace" "$build_dir/display_timing_final_spec.trace"
        echo "Display timing characterization matches frozen baseline"
        cat "$build_dir/display_timing_final_spec.trace"
    else
        echo "Frozen baseline display timing characterization passed"
        cat "$build_dir/display_timing_baseline_spec.trace"
    fi
else
    build_run final
    echo "Display timing final characterization passed"
    cat "$build_dir/display_timing_final_spec.trace"
fi
