#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_UPDATES_BASELINE_SOURCE:-}"
build_dir="${GUI_UPDATES_SPEC_BUILD_DIR:-$repo_root/build_ui_test/updates}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    echo "GUI updates spec requires cached build_host/lvgl and build_host/ui/ui_fonts.o" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/gui_updates.c" && -f "$repo_root/src/ui/gui_updates.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "GUI updates module is not present; set GUI_UPDATES_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen update baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/update_baseline_fixture.c" <<'PY'
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
    raise RuntimeError("unbalanced C block in frozen baseline")


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
            return source[start:matching_brace(source, i) + 1] + "\n"
    raise RuntimeError(f"frozen function not found: {name}")


start = source.index("static gui_popup_t firmware_update_popup;")
end = source.index("static lv_obj_t * adb_switch = NULL;", start)
update_state_and_functions = source[start:end]

init = function("gui_settings_init")
calls = [line.strip() for line in init.splitlines()
         if line.strip() in {"build_firmware_update_popup();", "build_firmware_ota_popups();",
                             "build_player_update_popups();"}]
expected = ["build_firmware_update_popup();", "build_firmware_ota_popups();",
            "build_player_update_popups();"]
if calls != expected:
    raise RuntimeError(f"unexpected frozen update popup init sequence: {calls}")

teardown = function("gui_settings_teardown")
start = teardown.index("    if (player_update_busy) {")
end = teardown.index("    gui_popup_teardown(&button_mapping_reset_popup);", start)
cleanup = teardown[start:end]
required_end = "if (firmware_source_backdrop) { lv_obj_delete(firmware_source_backdrop); firmware_source_backdrop = NULL; }"
if not cleanup.rstrip().endswith(required_end):
    raise RuntimeError("frozen update teardown slice does not end at the firmware backdrop cleanup")

fixture = (
    "/* Extracted exact state and update UI implementation from frozen gui_settings.c. */\n"
    + update_state_and_functions
    + "\nvoid update_test_build_popups(void) {\n"
    + "\n".join("    " + call for call in calls) + "\n}\n"
    + "void update_test_teardown(void) {\n" + cleanup + "}\n"
    + "void update_test_player_row_cb(lv_event_t * event) { player_update_row_cb(event); }\n"
)
output_path.write_text(fixture, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/src/network" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included update UI code; cached LVGL/font objects are uninstrumented."
fi

build_run() {
    local mode="$1"
    local exe="$build_dir/updates_${mode}_spec"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DUPDATE_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$repo_root/src/ui/gui_updates_spec.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && "$exe") > "$build_dir/updates_${mode}_spec.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$build_dir/updates_baseline_spec.trace" "$build_dir/updates_final_spec.trace"
        echo "GUI updates characterization matches frozen baseline"
        cat "$build_dir/updates_final_spec.trace"
    else
        echo "Frozen baseline GUI updates characterization passed"
        cat "$build_dir/updates_baseline_spec.trace"
    fi
else
    build_run final
    echo "GUI updates final characterization passed"
    cat "$build_dir/updates_final_spec.trace"
fi
