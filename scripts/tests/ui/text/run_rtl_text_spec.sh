#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
test_dir="$repo_root/build_ui_test/rtl-text-spec"
work_dir="$test_dir/run-cwd-$$"
mkdir -p "$test_dir"

# Rebuild against the current LVGL configuration and production fallback-font
# chain before linking the test. Device state is isolated in a fresh host CWD.
make -s -C "$repo_root" host BOARD=r1
rm -rf "$work_dir"
mkdir -p "$work_dir/.compas" "$work_dir/screenshots"
ln -sfn "$repo_root/assets" "$work_dir/assets"

python3 - "$repo_root" "$test_dir" <<'PY'
import json
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
test_dir = pathlib.Path(sys.argv[2])
test_source = root / "tests/text/rtl_text_spec.c"
entry = next(e for e in json.loads((root / "compile_commands.json").read_text())
             if e["file"] == "src/ui/widgets/builders/screen_builders.c")
args = list(entry["arguments"])
args[args.index("src/ui/widgets/builders/screen_builders.c")] = str(test_source)
args[args.index("build_host/ui/widgets/builders/screen_builders.o")] = str(test_dir / "rtl_text_spec.o")
args.extend(["-ffunction-sections", "-fdata-sections"])
subprocess.run(args, cwd=root, check=True)

library_entry = next(e for e in json.loads((root / "compile_commands.json").read_text())
                     if e["file"] == "src/ui/library/browse/gui_library.c")
library_args = list(library_entry["arguments"])
library_source = root / "tests/text/rtl_text_library_tu.c"
library_args[library_args.index("src/ui/library/browse/gui_library.c")] = str(library_source)
library_args[library_args.index("build_host/ui/library/browse/gui_library.o")] = str(test_dir / "rtl_text_library_tu.o")
subprocess.run(library_args, cwd=root, check=True)

timeline_entry = next(e for e in json.loads((root / "compile_commands.json").read_text())
                      if e["file"] == "src/ui/player/lyrics/player_timeline_labels.c")
timeline_args = list(timeline_entry["arguments"])
timeline_source = root / "tests/text/rtl_text_timeline_tu.c"
timeline_args[timeline_args.index("src/ui/player/lyrics/player_timeline_labels.c")] = str(timeline_source)
timeline_args[timeline_args.index("build_host/ui/player/lyrics/player_timeline_labels.o")] = str(test_dir / "rtl_text_timeline_tu.o")
subprocess.run(timeline_args, cwd=root, check=True)

make_output = subprocess.run(["make", "-Bn", "-C", str(root),
                              "compas_player_host", "BOARD=r1"],
                             check=True, text=True, capture_output=True).stdout
link_line = next(line for line in make_output.splitlines()
                 if line.startswith("g++ -o compas_player_host "))
link_args = shlex.split(link_line)
renamed_main = test_dir / "main_without_entry.o"
subprocess.run(["objcopy", "--redefine-sym", "main=compas_player_main",
                str(root / "build_host/app/main.o"), str(renamed_main)], check=True)
host_objects = [arg for arg in link_args[3:] if arg not in {
    "build_host/app/main.o", "build_host/ui/widgets/builders/screen_builders.o", "build_host/ui/widgets/lists/compact_list.o", "build_host/ui/library/browse/gui_library.o",
    "build_host/ui/player/lyrics/player_timeline_labels.o"
}]
binary = test_dir / "rtl_text_spec"
subprocess.run(["g++", "-Wl,--gc-sections", "-o", str(binary),
                str(test_dir / "rtl_text_spec.o"), str(test_dir / "rtl_text_library_tu.o"),
                str(test_dir / "rtl_text_timeline_tu.o"), str(renamed_main), *host_objects],
               cwd=root, check=True)
PY

set +e
(cd "$work_dir" && SDL_VIDEODRIVER=dummy stdbuf -oL "$test_dir/rtl_text_spec") | tee "$test_dir/run.log"
test_status=${PIPESTATUS[0]}
set -e

python3 - "$work_dir/screenshots" "$test_dir/run.log" <<'PY'
from pathlib import Path
import re
import sys
from PIL import Image

directory = Path(sys.argv[1])
log = Path(sys.argv[2]).read_text()
for name, width, height, stride in re.findall(r"SNAPSHOT (\S+) (\d+) (\d+) (\d+)", log):
    raw = directory.parent / name
    data = raw.read_bytes()
    width, height, stride = int(width), int(height), int(stride)
    rows = b"".join(data[y * stride:y * stride + width * 3] for y in range(height))
    png = raw.with_suffix(".png")
    Image.frombytes("RGB", (width, height), rows, "raw", "BGR").save(png)
    raw.unlink()
    print(f"PNG {png} {width}x{height}")
PY

exit "$test_status"
