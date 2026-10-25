#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${REMOTE_CONTROL_STATUS_BUILD_DIR:-$HOME/.cache/compas-theme-review/remote-control-status}"
mkdir -p "$build_dir"

"${CC:-cc}" -std=gnu11 -DHOST_BUILD -DBOARD_R1 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-function -Wno-unused-variable -ffunction-sections -fdata-sections \
    -I"$repo_root" -I"$repo_root/src/network" -I"$repo_root/src/core" \
    -I"$repo_root/src/library" -I"$repo_root/src/ui" -I"$repo_root/src/audio" \
    -I"$repo_root/src/hardware" -I"$repo_root/src/plugins" \
    "$repo_root/src/network/remote_control_status_spec.c" \
    -Wl,--gc-sections -pthread -o "$build_dir/remote_control_status_spec"

python3 - "$build_dir/remote_control_status_spec" <<'PY'
import json
import subprocess
import sys

binary = sys.argv[1]
special = 'catalog"\\\nrevision'
for mode, expected in (
    ("--special", special),
    ("--max", "\x01" * 63),
    ("--empty", ""),
    ("--unavailable", None),
):
    result = subprocess.run([binary, mode], check=True, text=True, capture_output=True)
    status = json.loads(result.stdout)
    assert status["catalog_revision"] == expected, (mode, status.get("catalog_revision"))
print("remote control status catalog revision: PASS")
PY
