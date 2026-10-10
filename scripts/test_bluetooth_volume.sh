#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build_test/bluetooth_volume"
mkdir -p "$build_dir"

python3 - "$repo_root/compile_commands.json" "$repo_root" "$build_dir" <<'PY'
import json, pathlib, subprocess, sys

commands = json.loads(pathlib.Path(sys.argv[1]).read_text())
repo = pathlib.Path(sys.argv[2])
build = pathlib.Path(sys.argv[3])
entry = next(x for x in commands if x["file"].endswith("src/network/bluetooth_control.c"))
args = entry["arguments"]
flags = args[1:args.index("-c")]
flags = [x for x in flags if x not in ("-MMD", "-MP")]

for name in ("bluetooth_monitor_test", "bluetooth_codec_test"):
    source = repo / "src/network" / f"{name}.c"
    binary = build / name
    command = ["gcc", *flags, "-ffunction-sections", "-fdata-sections", str(source),
               "-pthread", "-Wl,--gc-sections"]
    if name == "bluetooth_codec_test":
        command += ["-Wl,--wrap=opendir", "-Wl,--wrap=readdir", "-Wl,--wrap=closedir",
                    "-Wl,--wrap=fopen", "-Wl,--wrap=pthread_create"]
    command += ["-o", str(binary)]
    subprocess.run(command, cwd=repo, check=True)
    subprocess.run([str(binary)], cwd=repo, check=True)
PY
