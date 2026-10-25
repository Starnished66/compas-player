#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_BUTTON_MAPPING_BASELINE_SOURCE:-}"
build_dir="${GUI_BUTTON_MAPPING_SPEC_BUILD_DIR:-$repo_root/build_ui_test/button-mapping}"
mode="${1:---current}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "button mapping characterization requires build_host/lvgl; run make host BOARD=r1 first" >&2
    exit 2
fi
if [[ "$mode" != --current && "$mode" != --baseline ]]; then
    echo "usage: $0 [--current|--baseline]" >&2
    exit 2
fi

if [[ "$mode" == --baseline ]]; then
    if [[ -z "$baseline_source" || ! -f "$baseline_source" ]]; then
        echo "set GUI_BUTTON_MAPPING_BASELINE_SOURCE to the frozen gui_settings.c source" >&2
        exit 2
    fi
    python3 - "$baseline_source" "$repo_root/src/ui/gui.c" "$build_dir/button_mapping_baseline_fixture.c" "$build_dir/button_mapping_baseline_teardown.c" "$build_dir/button_mapping_wake_helpers.c" <<'PY'
from pathlib import Path
import re
import sys

settings_path, gui_path, fixture_path, teardown_path, wake_path = map(Path, sys.argv[1:])
settings = settings_path.read_text(encoding="utf-8")
gui = gui_path.read_text(encoding="utf-8")

def brace_end(text, opening):
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
    raise RuntimeError("unterminated C body")

def function(text, name):
    # Find definitions only: balance the parameter list, then require a body.
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        paren = text.find("(", match.start())
        depth, i = 1, paren + 1
        while i < len(text) and depth:
            if text[i] == "(": depth += 1
            elif text[i] == ")": depth -= 1
            i += 1
        while i < len(text) and text[i].isspace(): i += 1
        if i < len(text) and text[i] == "{":
            start = text.rfind("\n", 0, match.start()) + 1
            return text[start:brace_end(text, i) + 1] + "\n"
    raise RuntimeError(f"missing definition: {name}")

state_start = settings.index("static lv_obj_t * button_mapping_screen;")
state_end_marker = "static lv_obj_t * settings_display_screen;"
state_end = settings.index(state_end_marker, state_start)
state = settings[state_start:state_end]
body_start = settings.index("static const char * button_mapping_button_label(int button)")
body_end = settings.index("static lv_obj_t * build_music_controls_screen(void)", body_start)
body = settings[body_start:body_end].rstrip() + "\n"
fixture_path.write_text("/* Exact button mapping block sliced from frozen gui_settings.c. */\n" + state + "\n" + body, encoding="utf-8")

teardown = function(settings, "gui_settings_teardown")
start = teardown.index("    settings_button_mapping_summary = NULL;")
end = teardown.index("    if (music_controls_screen)", start)
cleanup = teardown[start:end]
if "button_mapping_screen" not in cleanup or "button_detail_screens" not in cleanup:
    raise RuntimeError("button mapping teardown slice incomplete")
teardown_path.write_text("static void test_button_mapping_teardown_screens(void) {\n" + cleanup + "}\n", encoding="utf-8")

helpers = "#include <stdbool.h>\n#include \"button_mapping.h\"\n#include \"board_config.h\"\n\n"
helpers += function(gui, "physical_button_default_action")
helpers += function(gui, "gui_button_bindings_allow_wake")
wake_path.write_text(helpers, encoding="utf-8")
PY
    define=(-DBUTTON_MAPPING_SPEC_BASELINE)
else
    if [[ ! -f "$repo_root/src/ui/gui_button_mapping.c" || ! -f "$repo_root/src/ui/gui_button_mapping.h" ]]; then
        echo "current gui_button_mapping.c/.h are not present yet" >&2
        exit 2
    fi
    python3 - "$repo_root/src/ui/gui.c" "$build_dir/button_mapping_wake_helpers.c" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
def extract(name):
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        p = text.find("(", m.start()); d = 1; i = p + 1
        while i < len(text) and d:
            if text[i] == "(": d += 1
            elif text[i] == ")": d -= 1
            i += 1
        while i < len(text) and text[i].isspace(): i += 1
        if i < len(text) and text[i] == "{":
            start = text.rfind("\n", 0, m.start()) + 1
            depth = 0; state = "code"; esc = False
            for j in range(i, len(text)):
                c = text[j]; n = text[j+1] if j+1 < len(text) else ""
                if state == "line":
                    if c == "\n": state = "code"
                elif state == "block":
                    if c == "*" and n == "/": state = "code"
                elif state in ("string", "char"):
                    if esc: esc = False
                    elif c == "\\": esc = True
                    elif (state == "string" and c == '"') or (state == "char" and c == "'"): state = "code"
                elif c == "/" and n == "/": state = "line"
                elif c == "/" and n == "*": state = "block"
                elif c == '"': state = "string"
                elif c == "'": state = "char"
                elif c == "{": depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0: return text[start:j+1] + "\n"
    raise RuntimeError("missing function " + name)
Path(sys.argv[2]).write_text("#include <stdbool.h>\n#include \"button_mapping.h\"\n#include \"board_config.h\"\n\n" + extract("physical_button_default_action") + extract("gui_button_bindings_allow_wake"))
PY
    define=()
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/hardware"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src"
    -I"$build_dir")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included Button Mapping code; cached LVGL objects are uninstrumented."
fi
exe="$build_dir/${mode#--}_spec"
"$cc" "${flags[@]}" "${define[@]}" "$repo_root/src/ui/gui_button_mapping_spec.c" \
    "$build_dir/button_mapping_wake_helpers.c" "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" \
    -Wl,--gc-sections -lpthread -lm -o "$exe"
run_dir="$build_dir/run_${mode#--}"
rm -rf "$run_dir" && mkdir -p "$run_dir"
(cd "$run_dir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$exe")
