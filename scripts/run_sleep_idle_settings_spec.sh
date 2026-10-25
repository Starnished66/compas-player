#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}"
build_dir="${SLEEP_IDLE_SETTINGS_SPEC_BUILD_DIR:-$cache_root/compas-player/qa/sleep-idle-settings}"
mkdir -p "$build_dir"
if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "Sleep/idle settings spec requires cached build_host/lvgl; refusing common graph build" >&2
    exit 2
fi
python3 - "$repo_root" "$build_dir/sleep_idle_production_extract.inc" <<'PY'
from pathlib import Path
import re, sys
root, out = Path(sys.argv[1]), Path(sys.argv[2])

def body(source, name, signature):
    m = re.search(r"(?m)^" + re.escape(signature) + r"\s*\([^;]*?\)\s*\{", source)
    if not m: raise SystemExit(f"missing production function: {name}")
    start = m.start(); opening = source.find("{", m.start()); depth=0; state="code"; esc=False; i=opening
    while i < len(source):
        c=source[i]; n=source[i+1] if i+1<len(source) else ""
        if state=="line":
            if c=="\n": state="code"
        elif state=="block":
            if c=="*" and n=="/": state="code"; i+=1
        elif state in ("string","char"):
            if esc: esc=False
            elif c=="\\": esc=True
            elif (state=="string" and c=='"') or (state=="char" and c=="'"): state="code"
        elif c=="/" and n=="/": state="line"; i+=1
        elif c=="/" and n=="*": state="block"; i+=1
        elif c=='"': state="string"
        elif c=="'": state="char"
        elif c=="{": depth+=1
        elif c=="}":
            depth-=1
            if depth==0: return source[start:i+1]
        i+=1
    raise SystemExit(f"unbalanced production function: {name}")

settings=(root/"src/ui/gui_settings.c").read_text()
shell=(root/"src/ui/gui_shell.c").read_text()
settings_data=(root/"src/core/settings.c").read_text()
state_names=["sleep_timer_switch","sleep_timer_slider_card","sleep_timer_remaining_btn","sleep_timer_value_label","sleep_timer_slider","idle_shutdown_switch","idle_shutdown_slider_card","idle_shutdown_value_label","idle_shutdown_slider","idle_action_section","idle_action_poweroff_row","idle_action_suspend_row","idle_action_poweroff_check","idle_action_suspend_check"]
states=[]
for name in state_names:
    m=re.search(r"(?m)^static lv_obj_t \*\s*"+name+r"\s*;",settings)
    if not m: raise SystemExit(f"missing production widget state: {name}")
    states.append(m.group(0))
arrays=[]
for name in ("IDLE_SHUTDOWN_STEPS","SLEEP_TIMER_STEPS"):
    m=re.search(r"(?m)^const int "+name+r"\[[^\]]+\]\s*=\s*\{[^}]+\};",settings_data)
    if not m: raise SystemExit(f"missing production option array: {name}")
    arrays.append(m.group(0))
enum=re.search(r"typedef enum \{\s*QD_TOGGLE_WIFI\s*=\s*0,.*?\} qd_toggle_t;",shell,re.S)
if not enum: raise SystemExit("missing production quick drawer toggle enum")
shell_state=[]
for decl in ("static bool sleep_timer_active = false;","static uint32_t sleep_timer_start_tick = 0;","static lv_obj_t * quick_drawer_sleep_icon;","static lv_obj_t * quick_drawer_sleep_label;"):
    if decl not in shell: raise SystemExit(f"missing shell timer state: {decl}")
    shell_state.append(decl)
funcs=[]
for name,sig,src in [
 ("format_sleep_timer_duration","static void format_sleep_timer_duration",settings),
 ("sleep_timer_switch_event_cb","static void sleep_timer_switch_event_cb",settings),
 ("sleep_timer_slider_event_cb","static void sleep_timer_slider_event_cb",settings),
 ("sleep_timer_show_remaining_cb","static void sleep_timer_show_remaining_cb",settings),
 ("gui_settings_sync_sleep_timer_toggle","void gui_settings_sync_sleep_timer_toggle",settings),
 ("format_idle_shutdown","static void format_idle_shutdown",settings),
 ("idle_shutdown_switch_event_cb","static void idle_shutdown_switch_event_cb",settings),
 ("idle_action_choice_cb","static void idle_action_choice_cb",settings),
 ("idle_shutdown_slider_event_cb","static void idle_shutdown_slider_event_cb",settings),
 ("apply_sleep_timer_active","static void apply_sleep_timer_active",shell),
 ("quick_drawer_sleep_timer_is_active","bool quick_drawer_sleep_timer_is_active",shell),
 ("quick_drawer_sleep_timer_remaining_seconds","int quick_drawer_sleep_timer_remaining_seconds",shell),
 ("quick_drawer_sleep_timer_set_active","void quick_drawer_sleep_timer_set_active",shell),
 ("poll_sleep_timer","static void poll_sleep_timer",shell),
]: funcs.append(body(src,name,sig))
header="/* Extracted verbatim from production gui_settings.c/gui_shell.c/settings.c by the focused runner. */\n"
out.write_text(header+"\n".join(arrays)+"\n"+enum.group(0)+"\n"+"\n".join(states+shell_state)+"\n"+"\n".join(funcs)+"\n",encoding="utf-8")
PY
cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$cc" "${flags[@]}" "$repo_root/src/ui/sleep_idle_settings_spec.c" "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" -Wl,--gc-sections -lpthread -lm -o "$build_dir/sleep_idle_settings_spec"
run_dir="$build_dir/run"
mkdir -p "$run_dir"
(cd "$run_dir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" timeout --signal=TERM --kill-after=2s 30s "$build_dir/sleep_idle_settings_spec")
