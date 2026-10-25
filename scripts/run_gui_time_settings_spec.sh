#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
settings_source="${GUI_TIME_SETTINGS_BASELINE_SETTINGS_SOURCE:-}"
player_source="${GUI_TIME_SETTINGS_BASELINE_PLAYER_SOURCE:-}"
build_dir="${GUI_TIME_SETTINGS_SPEC_BUILD_DIR:-$repo_root/build_ui_test/gui-time-settings}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/core/timezone_data.o" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi
final_available=0
if [[ -f "$repo_root/src/ui/gui_time_settings.c" && -f "$repo_root/src/ui/gui_time_settings.h" ]]; then
    final_available=1
fi
if [[ -n "$settings_source" && ( -z "$player_source" || ! -f "$settings_source" || ! -f "$player_source" ) ]]; then
    echo "set both GUI_TIME_SETTINGS_BASELINE_SETTINGS_SOURCE and GUI_TIME_SETTINGS_BASELINE_PLAYER_SOURCE to frozen sources" >&2
    exit 2
fi
if [[ -z "$settings_source" && "$final_available" == 0 ]]; then
    echo "set frozen baseline source variables or add gui_time_settings.c/.h for final mode" >&2
    exit 2
fi

if [[ -n "$settings_source" ]]; then
python3 - "$settings_source" "$player_source" "$build_dir/gui_time_settings_baseline_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

settings_path, player_path, output_path = map(Path, sys.argv[1:])
settings = settings_path.read_text(encoding="utf-8")
player = player_path.read_text(encoding="utf-8")

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
    raise RuntimeError("unbalanced C body in frozen baseline")

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
            return text[start:brace_end(text, i) + 1] + "\n"
    raise RuntimeError(f"missing frozen function: {name}")

state_names = ("clock_screen", "clock_set_time_screen", "clock_hour_roller",
               "clock_minute_roller", "clock_ampm_roller", "clock_set_time_row",
               "clock_timezone_row", "clock_timezone_value_label", "timezone_region_screen",
               "timezone_city_screen", "timezone_city_indices", "timezone_city_count")
state_lines = []
for name in state_names:
    match = re.search(r"^static [^\n;]+\b" + re.escape(name) + r"(?:\[[^]]+\])?;\s*$", settings, re.M)
    if not match: raise RuntimeError(f"missing frozen state: {name}")
    state_lines.append(match.group(0))

tz_start = settings.index("/* ---- Time Zone picker")
tz_end = settings.index("/* Defined later, alongside its confirmation popup", tz_start)
clock_start = settings.index("static void clock_persist(void)")
clock_end = settings.index("static lv_obj_t * build_settings_maintenance_screen", clock_start)
callback_name = "clock_24h_switch_event_cb"
callback_comment = re.search(r"/\* refresh_clock_label\(\) re-syncs.*?(?=void " + callback_name + r"\s*\()", player, re.S)
if not callback_comment: raise RuntimeError("missing frozen 24-hour callback comment")
callback = callback_comment.group(0) + function(player, callback_name)

fixture = """/* Exact clock/timezone code sliced from frozen Settings and Player sources. */
""" + callback + "\n" + "\n".join(state_lines) + "\n\n"
fixture += settings[tz_start:tz_end].rstrip() + "\n\n"
fixture += settings[clock_start:clock_end].rstrip() + "\n\n"
fixture += """void gui_time_spec_build_screens(void) {
    timezone_region_screen = build_timezone_region_screen();
    clock_set_time_screen = build_clock_set_time_screen();
    clock_screen = build_clock_screen();
}
void gui_time_spec_teardown(void) {
    if (timezone_region_screen) { lv_obj_delete(timezone_region_screen); timezone_region_screen = NULL; }
    if (clock_screen) { lv_obj_delete(clock_screen); clock_screen = NULL; }
    if (clock_set_time_screen) { lv_obj_delete(clock_set_time_screen); clock_set_time_screen = NULL; }
    clock_hour_roller = clock_minute_roller = clock_ampm_roller = NULL;
    clock_set_time_row = NULL;
    clock_timezone_row = clock_timezone_value_label = NULL;
    if (timezone_city_screen) { lv_obj_delete(timezone_city_screen); timezone_city_screen = NULL; }
}
void gui_time_spec_clock_row_cb(lv_event_t * e) { clock_settings_row_cb(e); }
"""
output_path.write_text(fixture, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included clock/timezone code; cached LVGL objects are uninstrumented."
fi
build_run() {
    local mode="$1"
    local exe="$build_dir/gui_time_settings_${mode}_spec"
    local defs=()
    [[ "$mode" == final ]] && defs+=(-DGUI_TIME_SETTINGS_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" "$repo_root/tests/gui_time_settings_spec.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/core/timezone_data.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$exe") \
        > "$build_dir/gui_time_settings_${mode}_spec.trace"
}

if [[ -n "$settings_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$build_dir/gui_time_settings_baseline_spec.trace" \
            "$build_dir/gui_time_settings_final_spec.trace"
        echo "GUI time settings characterization matches frozen baseline"
        cat "$build_dir/gui_time_settings_final_spec.trace"
    else
        echo "Frozen baseline GUI time settings characterization passed"
        cat "$build_dir/gui_time_settings_baseline_spec.trace"
    fi
else
    build_run final
    echo "Final GUI time settings characterization passed"
    cat "$build_dir/gui_time_settings_final_spec.trace"
fi
