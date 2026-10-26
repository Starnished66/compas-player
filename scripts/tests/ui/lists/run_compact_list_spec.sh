#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
test_dir="$repo_root/build_ui_test/compact-list-spec"
work_dir="$test_dir/run-cwd-$$"
mkdir -p "$test_dir"

make -s -C "$repo_root" host BOARD=r1
rm -rf "$work_dir"
mkdir -p "$work_dir/.compas"
ln -sfn "$repo_root/assets" "$work_dir/assets"

python3 - "$repo_root" "$test_dir" "${COMPACT_LIST_BASELINE_SOURCE:-}" <<'PY'
import json
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
test_dir = pathlib.Path(sys.argv[2])
source = root / "tests/ui/compact_list_spec.c"
baseline_source = sys.argv[3]
entry = next(e for e in json.loads((root / "compile_commands.json").read_text())
             if e["file"] == "src/ui/widgets/builders/screen_builders.c")
args = list(entry["arguments"])
args[args.index("src/ui/widgets/builders/screen_builders.c")] = str(source)
args[args.index("build_host/ui/widgets/builders/screen_builders.o")] = str(test_dir / "compact_list_spec.o")
if baseline_source:
    args.append(f'-DCOMPACT_LIST_BASELINE_SOURCE="{pathlib.Path(baseline_source).resolve()}"')
args.extend(["-ffunction-sections", "-fdata-sections"])
subprocess.run(args, cwd=root, check=True)

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
    "build_host/app/main.o", "build_host/ui/widgets/builders/screen_builders.o", "build_host/ui/widgets/lists/compact_list.o"
}]
subprocess.run(["g++", "-Wl,--gc-sections", "-o", str(test_dir / "compact_list_spec"),
                str(test_dir / "compact_list_spec.o"), str(renamed_main), *host_objects],
               cwd=root, check=True)
PY

(cd "$work_dir" && SDL_VIDEODRIVER=dummy "$test_dir/compact_list_spec") | tee "$test_dir/run.log"
