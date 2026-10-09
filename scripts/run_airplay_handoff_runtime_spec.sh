#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${AIRPLAY_HANDOFF_SPEC_BUILD_DIR:-$repo_root/build_ui_test/airplay_handoff}"
mkdir -p "$build_dir"

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root/src/network" -I"$repo_root/src/audio" -I"$repo_root/src/core" \
    -I"$repo_root/src/library" -I"$repo_root/src" \
    "$repo_root/src/network/airplay_handoff_runtime_spec.c" \
    -Wl,--wrap=mkfifo -Wl,--wrap=unlink -Wl,--wrap=open -Wl,--wrap=poll \
    -Wl,--wrap=read -Wl,--wrap=close -Wl,--wrap=usleep -lpthread \
    -o "$build_dir/airplay_handoff_runtime_spec"

"$build_dir/airplay_handoff_runtime_spec"
