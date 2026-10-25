#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${PLUGIN_ORDER_BASELINE_SOURCE:-}"
build_dir="${PLUGIN_ORDER_SPEC_BUILD_DIR:-$repo_root/build_ui_test/plugin-order-editor}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi
if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    echo "plugin order editor spec requires build_host/lvgl and build_host/ui/ui_fonts.o" >&2
    exit 2
fi

final_module=0
if [[ -f "$repo_root/src/ui/gui_plugin_order_editor.c" && -f "$repo_root/src/ui/gui_plugin_order_editor.h" ]]; then
    final_module=1
fi
if [[ -z "$baseline_source" && "$final_module" == 0 ]]; then
    if rg -q '^int gui_plugin_show_order_editor\(' "$repo_root/src/ui/gui_settings.c"; then
        baseline_source="$repo_root/src/ui/gui_settings.c"
    else
        echo "set PLUGIN_ORDER_BASELINE_SOURCE to characterize the pre-move implementation" >&2
        exit 2
    fi
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested plugin-order baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" || "$final_module" == 1 ]]; then
python3 - "$baseline_source" "$build_dir/baseline_fixture.c" "$repo_root/src/ui/gui_settings.c" "$repo_root/src/ui/gui_plugin_order_editor.c" "$final_module" <<'PY'
from pathlib import Path
import re
import sys

source_name, output_path, live_settings_path, final_module_path = sys.argv[1:5]
source_path = Path(source_name) if source_name else None
output_path, live_settings_path, final_module_path = map(Path, (output_path, live_settings_path, final_module_path))
final_module = sys.argv[5] == "1"
source = source_path.read_text(encoding="utf-8") if source_path else ""


def matching_brace(text: str, opening: int) -> int:
    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in {"string", "char"}:
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        else:
            if ch == "/" and nxt == "/": state = "line"; i += 1
            elif ch == "/" and nxt == "*": state = "block"; i += 1
            elif ch == '"': state = "string"
            elif ch == "'": state = "char"
            elif ch == "{": depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced C braces in frozen baseline")


def function(name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?[^\n;]*\b" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise RuntimeError(f"baseline function marker missing: {name}")
    opening = source.find("{", match.start())
    if opening < 0:
        raise RuntimeError(f"baseline function body missing: {name}")
    closing = matching_brace(source, opening)
    return source[match.start():closing + 1] + "\n"


def function_from(text: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?[^\n;]*\b" + re.escape(name) + r"\s*\(", text)
    if not match:
        raise RuntimeError(f"function marker missing: {name}")
    opening = text.find("{", match.start())
    closing = matching_brace(text, opening)
    return text[match.start():closing + 1]


cleanup = None
if source_path:
    state_start = source.index("/* The plugin editor shares the reorder primitive")
    state_end = source.index("static lv_obj_t * quick_drawer_settings_screen;", state_start)
    state = source[state_start:state_end]

    names = [
        "plugin_order_editor_emit_change",
        "plugin_order_editor_populate",
        "plugin_order_editor_prepare_drag",
        "plugin_order_editor_commit_drag",
        "plugin_order_editor_restore_drag",
        "plugin_order_editor_toggle_cb",
        "plugin_order_editor_drag_handle_cb",
        "plugin_order_editor_screen_unloaded_cb",
        "plugin_order_editor_build_rows",
        "gui_plugin_show_order_editor",
    ]
    functions = "\n".join(function(name) for name in names)

    teardown = function("gui_settings_teardown")
    start_marker = "gui_list_reorder_finish(&plugin_order_editor_drag_state, false);"
    end_marker = "if (quick_drawer_settings_screen)"
    start = teardown.index(start_marker)
    end = teardown.index(end_marker, start)
    cleanup = teardown[start:end]
    if not teardown[:start].rstrip().endswith("ui_order_drag_state.drag_id = -1;"):
        raise RuntimeError("teardown slice does not begin after the ui-order cleanup anchor")
    if matching_brace("{\n" + cleanup + "}\n", 0) < len("{\n" + cleanup + "}\n") - 2:
        raise RuntimeError("extracted teardown block failed brace balance")
    if not cleanup.lstrip().startswith(start_marker) or not cleanup.rstrip().endswith("plugin_order_editor_drag_state.drag_id = -1;"):
        raise RuntimeError("teardown slice does not include the full plugin editor cleanup")
    if end_marker not in teardown[end:]:
        raise RuntimeError("teardown quick-drawer boundary missing")

    fixture = (
        "/* Generated from the frozen complete gui_settings.c baseline by the runner. */\n"
        + state + "\n" + functions
        + "\nvoid gui_plugin_order_editor_teardown(void) {\n"
        + cleanup + "}\n"
    )
    output_path.write_text(fixture, encoding="utf-8")

if final_module:
    live_settings = live_settings_path.read_text(encoding="utf-8")
    settings_teardown = function_from(live_settings, "gui_settings_teardown")
    before = "gui_shell_settings_teardown_order();"
    after = "gui_shell_settings_teardown_after_plugin_order();"
    if settings_teardown.count(before) != 1 or settings_teardown.count(after) != 1:
        raise RuntimeError("live settings teardown boundary anchors are missing or ambiguous")
    region = settings_teardown.split(before, 1)[1].split(after, 1)[0]
    if re.sub(r"\s+", "", region) != "gui_plugin_order_editor_teardown();":
        raise RuntimeError("gui_settings teardown no longer delegates only the editor cleanup")
    if cleanup is not None and final_module_path.exists():
        module_source = final_module_path.read_text(encoding="utf-8")
        final_teardown = function_from(module_source, "gui_plugin_order_editor_teardown")
        final_body = final_teardown[final_teardown.index("{") + 1:final_teardown.rindex("}")]
        norm = lambda text: [line.strip() for line in text.splitlines() if line.strip()]
        if norm(final_body) != norm(cleanup):
            raise RuntimeError("new module teardown body differs from the frozen cleanup statements")
PY
fi

cc="${CC:-cc}"
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -fwrapv -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core"
    -I"$build_dir"
    -I"$repo_root/src/plugins" -I"$repo_root/src/library" -I"$repo_root/src/hardware"
    -I"$repo_root/src/audio" -I"$repo_root/src/network"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and gui_list_reorder.c; cached LVGL/font objects are uninstrumented."
fi
lvgl_objects=( $(find "$repo_root/build_host/lvgl" -name '*.o' -print) )

if [[ -n "$baseline_source" ]]; then
    "$cc" "${common_flags[@]}" -DEDITOR_BASELINE \
        "$repo_root/src/ui/gui_plugin_order_editor_spec.c" \
        "$repo_root/src/ui/gui_list_reorder.c" "${lvgl_objects[@]}" \
        "$repo_root/build_host/ui/ui_fonts.o" \
        -Wl,--gc-sections -lpthread -lm -o "$build_dir/baseline_spec"
    "$build_dir/baseline_spec" > "$build_dir/baseline.trace"
fi

if [[ "$final_module" == 1 ]]; then
    "$cc" "${common_flags[@]}" \
        "$repo_root/src/ui/gui_plugin_order_editor_spec.c" \
        "$repo_root/src/ui/gui_list_reorder.c" "${lvgl_objects[@]}" \
        "$repo_root/build_host/ui/ui_fonts.o" \
        -Wl,--gc-sections -lpthread -lm -o "$build_dir/final_spec"
    "$build_dir/final_spec" > "$build_dir/final.trace"
    if [[ -n "$baseline_source" ]]; then
        cmp "$build_dir/baseline.trace" "$build_dir/final.trace"
        echo "Plugin order editor characterization matches frozen baseline"
    else
        echo "Plugin order editor final characterization passed"
    fi
else
    echo "Frozen baseline characterization passed; final module is not present yet"
fi
