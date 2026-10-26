#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}"
build_dir="${ATOMIC_PERSISTENCE_TEST_BUILD_DIR:-$cache_root/compas-player/qa/atomic-persistence}"
mkdir -p "$build_dir/run"
ulimit -c 0
cleanup_fixture() { rm -f "$build_dir/run/.compas/settings.txt" "$build_dir/run/.compas/settings.txt.tmp" "$build_dir/run/queue.bin" "$build_dir/run/".queue.bin.*.tmp; rmdir "$build_dir/run/.compas" 2>/dev/null || true; }
trap cleanup_fixture EXIT
cc="${CC:-cc}"
"$cc" -std=gnu11 -D_GNU_SOURCE -DHOST_BUILD=1 -O1 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread \
  -I"$repo_root" -I"$repo_root/src/core" -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
  "$repo_root/src/core/persistence/atomic_persistence_powerloss_spec.c" "$repo_root/src/core/persistence/settings.c" "$repo_root/src/core/text/utf8_util.c" "$repo_root/src/core/themes/ui_customization.c" "$repo_root/src/library/playlists/queue_resume.c" \
  -Wl,--gc-sections -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=renameat -Wl,--wrap=fflush -Wl,--wrap=fclose -lm -o "$build_dir/powerloss_spec"
(cd "$build_dir/run" && timeout --signal=TERM --kill-after=2s 30s ../powerloss_spec)
