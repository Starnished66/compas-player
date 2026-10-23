#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${UI_SNAPSHOT_PREFETCH_TEST_BUILD_DIR:-$repo_root/build_ui_test/snapshot_prefetch}"
mkdir -p "$build_dir"
cc -std=gnu11 -Wall -Wextra -Werror -pthread -O2 -DUI_SNAPSHOT_PREFETCH_TEST=1 \
    -I"$repo_root/src/ui" \
    "$repo_root/src/ui/ui_snapshot_prefetch.c" \
    "$repo_root/src/ui/ui_snapshot_prefetch_spec.c" \
    -o "$build_dir/ui_snapshot_prefetch_test"
for run in $(seq 1 50); do
    "$build_dir/ui_snapshot_prefetch_test"
done
