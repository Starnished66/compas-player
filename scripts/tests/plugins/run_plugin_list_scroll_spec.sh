#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${PLUGIN_LIST_SCROLL_SPEC_BUILD_DIR:-$repo_root/build_ui_test/plugin-list-scroll}"
mkdir -p "$build_dir"

python3 - "$repo_root" "$build_dir" <<'PY'
import json
import pathlib
import re
import shlex
import subprocess
import sys

root, build = map(pathlib.Path, sys.argv[1:])
source = (root / "src/ui/plugins/gui_plugins.c").read_text()
(build / "gui_plugins_scroll_changed.c").write_text(source)
old_capture = "    int32_t keep_scroll_y = replace_slot >= 0 ? lv_obj_get_scroll_y(list) : 0;\n"
old_reset = "    if (replace_slot < 0) lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);\n"
new_reset = "    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);\n"
source = source.replace(old_capture, "", 1).replace(old_reset, new_reset, 1)
source, count = re.subn(r"(?m)^\s*if \(keep_scroll_y > 0\) \{\n\s*lv_obj_update_layout\(list\);\n\s*lv_obj_scroll_to_y\(list, keep_scroll_y, LV_ANIM_OFF\);\n\s*\}\n", "", source)
if count != 2:
    raise SystemExit("cannot reconstruct pre-PR baseline from the current gui_plugins.c")
baseline = build / "gui_plugins_scroll_baseline.c"
baseline.write_text(source)

compile_db = json.loads((root / "compile_commands.json").read_text())
entry = next((item for item in compile_db if item["file"] == "src/ui/plugins/gui_plugins.c"), None)
if not entry:
    raise SystemExit("compile_commands.json has no gui_plugins.c entry")
base_args = list(entry["arguments"])
input_index = next(i for i, arg in enumerate(base_args) if arg.endswith("src/ui/plugins/gui_plugins.c"))
out_index = base_args.index("-o") + 1
base_args[input_index] = str(root / "tests/plugins/plugin_list_scroll_spec.c")
base_args[out_index] = str(build / "plugin_list_scroll_spec.o")
base_args.extend(["-ffunction-sections", "-fdata-sections", "-I" + str(root)])
base_args.insert(input_index, "-DGUI_PLUGINS_IMPLEMENTATION=\"" + str(root / "src/ui/plugins/gui_plugins.c") + "\"")
subprocess.run(base_args, cwd=root, check=True)

baseline_args = list(base_args)
baseline_args[baseline_args.index("-DGUI_PLUGINS_IMPLEMENTATION=\"" + str(root / "src/ui/plugins/gui_plugins.c") + "\"")] = \
    "-DGUI_PLUGINS_IMPLEMENTATION=\"" + str(baseline) + "\""
baseline_args[baseline_args.index(str(build / "plugin_list_scroll_spec.o"))] = str(build / "plugin_list_scroll_baseline.o")
baseline_args.insert(1, "-DEXPECT_BASELINE=1")
subprocess.run(baseline_args, cwd=root, check=True)

link_args = shlex.split(subprocess.run(
    ["make", "-s", "--no-print-directory", "print-host-link", "BOARD=r1"],
    cwd=root, text=True, capture_output=True, check=True).stdout)
skip = {"build_host/app/main.o", "build_host/ui/plugins/gui_plugins.o"}
if not skip.issubset(link_args):
    raise SystemExit("host link graph does not contain app/main.o and gui_plugins.o")
main_input = root / "build_host/app/main.o"
if not main_input.exists():
    raise SystemExit("cached host backend is incomplete: build_host/app/main.o is missing")
main_obj = build / "main_without_entry.o"
subprocess.run(["objcopy", "--redefine-sym", "main=compas_player_main", str(main_input), str(main_obj)], check=True)
objects, flags, missing = [str(main_obj)], [], []
for arg in link_args:
    if arg in skip:
        continue
    if arg.endswith((".o", ".a")):
        path = root / arg
        if not path.is_file(): missing.append(arg)
        else: objects.append(str(path))
    else:
        flags.append(arg)
if missing:
    raise SystemExit("cached host backend dependencies are missing: " + ", ".join(missing[:12]))

def link(obj, name):
    wraps = ["-Wl,--wrap=" + sym for sym in ("nav_push", "gui_navigation_is_top", "gui_navigation_contains")]
    subprocess.run(["g++", "-Wl,--gc-sections", *wraps, "-o", str(build / name),
                    str(build / obj), *objects, *flags, "-pthread"], cwd=root, check=True)

link("plugin_list_scroll_spec.o", "plugin_list_scroll_spec")
link("plugin_list_scroll_baseline.o", "plugin_list_scroll_baseline")
PY

(cd "$repo_root" && COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$build_dir/plugin_list_scroll_baseline")
(cd "$repo_root" && COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$build_dir/plugin_list_scroll_spec")
