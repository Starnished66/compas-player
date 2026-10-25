#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_SHELL_SETTINGS_BASELINE_SOURCE:-}"
build_dir="${GUI_SHELL_SETTINGS_SPEC_BUILD_DIR:-$repo_root/build_ui_test/shell-settings}"
baseline_trace="${GUI_SHELL_SETTINGS_BASELINE_TRACE:-$build_dir/baseline.trace}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "GUI Shell Settings spec requires cached build_host/lvgl" >&2
    exit 2
fi
final_available=0
if [[ -f "$repo_root/src/ui/gui_shell_settings.c" && -f "$repo_root/src/ui/gui_shell_settings.h" ]]; then
    final_available=1
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "GUI Shell Settings module is not present; set GUI_SHELL_SETTINGS_BASELINE_SOURCE for baseline characterization" >&2
    exit 2
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen shell settings baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/shell_settings_baseline_fixture.c" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")


def function_body(name):
    match = re.search(r"(?m)^\s*(?:static\s+)?[^\n;]*\b" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise RuntimeError(f"baseline function not found: {name}")
    opening = source.find("{", match.start())
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
                return source[match.start():i + 1]
        i += 1
    raise RuntimeError(f"unbalanced body: {name}")


# Copy real module state declarations from the frozen coordinator. Keep all
# other state and callback implementations out of the test translation unit.
state_names = {
    "ui_order_screen", "ui_order_list", "ui_order_title", "ui_order_rows",
    "ui_order_drag_handles", "ui_order_visible_buttons", "ui_order_kind",
    "ui_order_drag_state", "quick_drawer_settings_screen",
    "quick_drawer_settings_list", "settings_topbar_screen", "settings_topbar_list",
    "settings_topbar_clock_position_screen", "settings_topbar_clock_position_list",
}
state = []
for line in source.splitlines():
    if re.match(r"^static\s+", line) and any(re.search(r"\b" + name + r"\b", line) for name in state_names):
        state.append(line)
if len(state) != len(state_names):
    raise RuntimeError(f"expected {len(state_names)} state declarations, found {len(state)}")

# These two forward declarations precede their first use in the source slices.
prototypes = []
for name in ("topbar_set_clicked_row_switch", "populate_topbar_settings_screen",
             "populate_topbar_clock_position_screen"):
    match = re.search(r"(?m)^static\s+[^\n;]*\b" + re.escape(name) + r"\s*\([^;]*;", source)
    if not match:
        raise RuntimeError(f"baseline forward declaration missing: {name}")
    prototypes.append(match.group(0))

order_start = source.index("static ui_item_order_t * ui_saved_order(void)")
order_end = source.index("static void quick_drawer_setting_toggle_cb", order_start)
order_code = source[order_start:order_end]
quick_start = order_end
quick_end = source.index("static void artist_images_switch_event_cb", quick_start)
quick_code = source[quick_start:quick_end]
topbar_start = source.index("typedef struct {\n    const char * label;\n    uint32_t hidden_bit;")
topbar_end = source.index("static lv_obj_t * build_settings_appearance_screen", topbar_start)
topbar_code = source[topbar_start:topbar_end]

teardown = function_body("gui_settings_teardown")
phase1_start = teardown.index("gui_list_reorder_finish(&ui_order_drag_state, false);")
phase1_end = teardown.index("gui_plugin_order_editor_teardown();", phase1_start)
phase1 = teardown[phase1_start:phase1_end]
phase2_start = teardown.index("if (quick_drawer_settings_screen)", phase1_end)
phase2_end = teardown.index("if (settings_player_layout_screen)", phase2_start)
phase2 = teardown[phase2_start:phase2_end]
if "unregister_swipe_dead_zone(ui_order_drag_handles[id])" not in phase1:
    raise RuntimeError("frozen order teardown does not unregister its actual drag handles")

fixture = (
    "/* Generated from the frozen full gui_settings.c for characterization. */\n"
    + "\n".join(state) + "\n" + "\n".join(prototypes) + "\n"
    + order_code + quick_code + topbar_code
    + "\nvoid shell_settings_test_quick_drawer_row_cb(lv_event_t * e) { quick_drawer_settings_row_cb(e); }\n"
    + "void shell_settings_test_music_order_row_cb(lv_event_t * e) { ui_order_kind_row_cb(e); }\n"
    + "void shell_settings_test_topbar_row_cb(lv_event_t * e) { topbar_settings_row_cb(e); }\n"
    + "void gui_shell_settings_teardown_order(void) {\n" + phase1 + "\n}\n"
    + "void gui_shell_settings_teardown_after_plugin_order(void) {\n" + phase2 + "\n}\n"
)
output_path.write_text(fixture, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/src/network" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec, gui_list_reorder.c and ui_customization.c; cached LVGL objects are uninstrumented."
fi

build_run() {
    local mode="$1"
    local exe="$build_dir/${mode}_spec" defs=()
    [[ "$mode" == final ]] && defs+=(-DSHELL_SETTINGS_SPEC_FINAL)
    "$cc" "${common_flags[@]}" "${defs[@]}" \
        "$repo_root/src/ui/gui_shell_settings_spec.c" \
        "$repo_root/src/ui/gui_list_reorder.c" "$repo_root/src/core/ui_customization.c" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" \
        "$repo_root/build_host/mbedtls/sha256.o" "$repo_root/build_host/mbedtls/platform_util.o" \
        -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"
    rm -rf "$run_dir"
    mkdir -p "$run_dir"
    (cd "$run_dir" && "$exe") > "$build_dir/${mode}.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline
    mkdir -p "$(dirname "$baseline_trace")"
    if [[ "$baseline_trace" != "$build_dir/baseline.trace" ]]; then
        cp "$build_dir/baseline.trace" "$baseline_trace"
    fi
    if [[ "$final_available" == 1 ]]; then
        build_run final
        cmp "$baseline_trace" "$build_dir/final.trace"
        echo "GUI Shell Settings characterization matches frozen baseline"
        cat "$build_dir/final.trace"
    else
        echo "Frozen baseline GUI Shell Settings characterization passed: $baseline_trace"
        cat "$baseline_trace"
    fi
else
    build_run final
    echo "GUI Shell Settings final characterization passed"
    cat "$build_dir/final.trace"
fi
