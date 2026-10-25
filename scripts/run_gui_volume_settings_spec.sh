#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_VOLUME_SETTINGS_BASELINE_SOURCE:-}"
build_dir="${GUI_VOLUME_SETTINGS_SPEC_BUILD_DIR:-$repo_root/build_ui_test/volume-settings}"
mkdir -p "$build_dir"
if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi
final_available=0
if [[ -f "$repo_root/src/ui/gui_volume_settings.c" && -f "$repo_root/src/ui/gui_volume_settings.h" ]]; then final_available=1; fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then echo "frozen baseline source missing: $baseline_source" >&2; exit 2; fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then echo "set GUI_VOLUME_SETTINGS_BASELINE_SOURCE or provide gui_volume_settings.c/.h" >&2; exit 2; fi
if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/gui_volume_settings_baseline_fixture.c" <<'PY'
from pathlib import Path
import re, sys
src = Path(sys.argv[1]).read_text(encoding="utf-8")
out = Path(sys.argv[2])
def brace_end(text, pos):
    depth=0; state="code"; escaped=False; i=pos
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ""
        if state=="line":
            if c=="\n": state="code"
        elif state=="block":
            if c=="*" and n=="/": state="code"; i+=1
        elif state in ("string","char"):
            if escaped: escaped=False
            elif c=="\\": escaped=True
            elif (state=="string" and c=='"') or (state=="char" and c=="'"): state="code"
        elif c=="/" and n=="/": state="line"; i+=1
        elif c=="/" and n=="*": state="block"; i+=1
        elif c=='"': state="string"
        elif c=="'": state="char"
        elif c=="{": depth+=1
        elif c=="}":
            depth-=1
            if depth==0: return i
        i+=1
    raise RuntimeError("unbalanced baseline C block")
def function(name):
    for m in re.finditer(r"\b"+re.escape(name)+r"\s*\(",src):
        p=src.find("(",m.start()); d=1; i=p+1
        while i<len(src) and d:
            if src[i]=="(": d+=1
            elif src[i]==")": d-=1
            i+=1
        while i<len(src) and src[i].isspace(): i+=1
        if i<len(src) and src[i]=="{":
            start=src.rfind("\n",0,m.start())+1
            return src[start:brace_end(src,i)+1]+"\n"
    raise RuntimeError("missing baseline function: "+name)
state_names=("startup_volume_screen","max_volume_screen","startup_volume_switch","startup_volume_slider_card",
"startup_volume_value_label","startup_volume_slider","max_volume_value_label","max_volume_slider",
"car_mode_volume_value_label","car_mode_volume_slider","car_mode_hint_label","car_mode_screen",
"car_mode_enable_switch","car_mode_autoresume_switch","car_mode_gain_row","car_mode_gain_dropdown")
states=[]
for name in state_names:
    m=re.search(r"^static [^\n;]+\b"+re.escape(name)+r";\s*$",src,re.M)
    if not m: raise RuntimeError("missing state: "+name)
    states.append(m.group(0))
start=src.index("static void startup_volume_switch_event_cb")
end=src.index("void gui_settings_open_sleep_timer",start)
block=src[start:end].rstrip()+"\n"
sync=function("gui_settings_sync_car_mode")
init=function("gui_settings_init")
init_needles=("startup_volume_screen = build_startup_volume_screen();",
              "max_volume_screen = build_max_volume_screen();",
              "music_controls_screen = NULL; /* built on first open of Playback & Controls > Buttons & Remote */",
              "car_mode_screen = build_car_mode_screen();")
positions=[init.find(item) for item in init_needles]
if any(pos < 0 for pos in positions) or positions != sorted(positions):
    raise RuntimeError("frozen initialization order changed for Startup/Max/Car Mode screens")
fixture="/* Exact frozen Startup/Max/Car Mode state and callbacks. */\n"+"\n".join(states)+"\n\n"+block+"\n"+sync
teardown=function("gui_settings_teardown")
start=teardown.index("    if (startup_volume_screen)")
end=teardown.index("    if (sleep_timer_screen)",start)
startup_cleanup=teardown[start:end]
start=teardown.index("    if (car_mode_screen)")
end=teardown.index("    if (animation_speed_screen)",start)
car_cleanup=teardown[start:end]
fixture += "\nvoid volume_spec_build_startup_max(void) { startup_volume_screen=build_startup_volume_screen(); max_volume_screen=build_max_volume_screen(); }\n"
fixture += "void volume_spec_build_car(void) { car_mode_screen=build_car_mode_screen(); }\n"
fixture += "void volume_spec_teardown_startup_max(void) {\n"+startup_cleanup+"}\n"
fixture += "void volume_spec_teardown_car(void) {\n"+car_cleanup+"}\n"
fixture += "void volume_spec_startup_row_cb(lv_event_t *e){startup_volume_row_cb(e);}\nvoid volume_spec_max_row_cb(lv_event_t *e){max_volume_row_cb(e);}\nvoid volume_spec_car_row_cb(lv_event_t *e){car_mode_settings_row_cb(e);}\n"
out.write_text(fixture,encoding="utf-8")
PY
fi
cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio" -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
build_run() {
    local mode="$1" exe="$build_dir/volume_${1}_spec" defs=()
    [[ "$mode" == final ]] && defs+=(-DVOLUME_SPEC_FINAL)
    "$cc" "${flags[@]}" "${defs[@]}" "$repo_root/tests/gui_volume_settings_spec.c" "${lvgl_objects[@]}" -Wl,--gc-sections -lpthread -lm -o "$exe"
    local run_dir="$build_dir/run_$mode"; rm -rf "$run_dir"; mkdir -p "$run_dir"
    (cd "$run_dir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$exe") > "$build_dir/volume_${mode}_spec.trace"
}
if [[ -n "$baseline_source" ]]; then
    build_run baseline
    if [[ "$final_available" == 1 ]]; then build_run final; cmp "$build_dir/volume_baseline_spec.trace" "$build_dir/volume_final_spec.trace"; echo "Volume settings characterization matches frozen baseline"; cat "$build_dir/volume_final_spec.trace"
    else echo "Frozen baseline Volume settings characterization passed"; cat "$build_dir/volume_baseline_spec.trace"; fi
else build_run final; echo "Final Volume settings characterization passed"; cat "$build_dir/volume_final_spec.trace"; fi
