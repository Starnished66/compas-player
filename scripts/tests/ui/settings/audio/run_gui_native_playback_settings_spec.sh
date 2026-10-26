#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
build_dir="${GUI_NATIVE_PLAYBACK_SETTINGS_BUILD_DIR:-$repo_root/build_ui_test/native-playback-settings}"
mkdir -p "$build_dir"
if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "native playback settings spec requires cached build_host/lvgl; run make host BOARD=r1 first" >&2
    exit 2
fi

python3 - "$repo_root/src/ui/settings/gui_settings.c" "$repo_root/src/ui/player/screen/gui_player.c" \
    "$repo_root/src/ui/network/gui_network.c" "$build_dir/native_playback_settings_fixture.inc" <<'PY'
from pathlib import Path
import re
import sys
settings_path, player_path, network_path, output_path = map(Path, sys.argv[1:])
settings, player, network = (p.read_text(encoding="utf-8") for p in (settings_path, player_path, network_path))

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
    raise RuntimeError("unbalanced C function")

def function(text, name):
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        paren = text.find("(", m.start()); depth, i = 1, paren + 1
        while i < len(text) and depth:
            if text[i] == "(": depth += 1
            elif text[i] == ")": depth -= 1
            i += 1
        while i < len(text) and text[i].isspace(): i += 1
        if i < len(text) and text[i] == "{":
            start = text.rfind("\n", 0, m.start()) + 1
            return text[start:brace_end(text, i) + 1] + "\n"
    raise RuntimeError("function not found: " + name)

parts = ["/* Exact callback and screen-builder source slices from the three UI owners. */\n"]
parts += [function(player, n) for n in (
    "gui_player_set_crossfade_enabled", "crossfade_switch_event_cb",
    "gui_player_set_gapless_enabled", "gapless_switch_event_cb", "inline_remote_switch_event_cb")]
parts += [function(settings, n) for n in (
    "volume_lock_switch_event_cb", "plugin_playback_list_item_click_cb",
    "plugin_music_controls_list_item_click_cb", "build_music_playback_screen",
    "build_music_controls_screen", "gui_settings_sync_gapless_toggle",
    "gui_settings_sync_crossfade_toggle")]
start = network.index("typedef struct {\n    int mode; /* matches player_settings_t.resume_mode */")
end = network.index("/* ---- Font size selection screen", start)
parts.append(network[start:end])
Path(output_path).write_text("\n".join(parts), encoding="utf-8")
PY

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers
       -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
       -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
       -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
       -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$cc" "${flags[@]}" "$repo_root/tests/ui/gui_native_playback_settings_spec.c" \
    "${lvgl_objects[@]}" -Wl,--gc-sections -lpthread -lm -o "$build_dir/gui_native_playback_settings_spec"
run_dir="$build_dir/run"
mkdir -p "$run_dir"
timeout --signal=TERM --kill-after=2s 30s \
    bash -c 'cd "$1" && ASAN_OPTIONS="$2" "$3"' _ "$run_dir" \
    "${ASAN_OPTIONS:-detect_leaks=0}" "$build_dir/gui_native_playback_settings_spec"
