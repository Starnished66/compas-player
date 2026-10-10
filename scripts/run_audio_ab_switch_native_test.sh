#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build_test/audio_ab_switch_native"
mkdir -p "$build_dir"

if [[ "${ABX_SKIP_HOST_BUILD:-0}" != "1" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

# Reuse the exact HOST_BUILD flags used for audio.c so the harness can include
# it and call its real static decoder/adopt/mix helpers.
python3 - "$repo_root/compile_commands.json" "$repo_root" "$build_dir" <<'PY'
import json, pathlib, shlex, subprocess, sys
commands = json.loads(pathlib.Path(sys.argv[1]).read_text())
repo = pathlib.Path(sys.argv[2])
build = pathlib.Path(sys.argv[3])
entry = next(x for x in commands if x["file"].endswith("src/audio/audio.c"))
args = entry["arguments"]
cut = args.index("-c")
flags = args[1:cut]
clean = []
i = 0
while i < len(flags):
    if flags[i] in ("-MMD", "-MP"):
        i += 1
        continue
    clean.append(flags[i]); i += 1
src = repo / "tests/audio_ab_switch_native_test.c"
obj = build / "audio_ab_switch_native_test.o"
subprocess.run(["gcc", *clean, "-ffunction-sections", "-fdata-sections", str(src), "-c", "-o", str(obj)], cwd=repo, check=True)
objects = []
for item in commands:
    out = item["arguments"][item["arguments"].index("-o") + 1]
    path = pathlib.Path(out)
    if "build_host" in path.parts and path.suffix == ".o" and path.name != "main.o" and not (path.parent.name == "audio" and path.name == "audio.o"):
        objects.append(str(repo / path))
libs = [repo / "build_host/libwavpack/libwavpack.a", repo / "build_host/libsndfile/libsndfile.a"]
cmd = ["g++", "-pthread", "-Wl,--gc-sections", str(obj), *objects, *(str(x) for x in libs if x.exists()), "-lSDL2", "-lm", "-o", str(build / "audio_ab_switch_native_test")]
subprocess.run(cmd, cwd=repo, check=True)
PY

"$build_dir/audio_ab_switch_native_test" "$build_dir"
