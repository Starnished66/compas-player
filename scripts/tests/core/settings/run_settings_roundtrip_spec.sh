#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${SETTINGS_ROUNDTRIP_TEST_BUILD_DIR:-$repo_root/build_test/settings-roundtrip}"
mkdir -p "$build_dir/run"
ulimit -c 0
cleanup_fixture() {
    rm -f "$build_dir/run/.compas/settings.txt" "$build_dir/run/.compas/settings.txt.tmp"
    rmdir "$build_dir/run/.compas" 2>/dev/null || true
}
trap cleanup_fixture EXIT

cc="${CC:-cc}"
"$cc" -std=gnu11 -D_GNU_SOURCE -DHOST_BUILD=1 -O1 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -pthread \
    -I"$repo_root" -I"$repo_root/src/core" -I"$repo_root/src/library" \
    -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
    "$repo_root/src/core/persistence/settings_roundtrip_spec.c" \
    "$repo_root/src/core/persistence/settings.c" "$repo_root/src/core/text/utf8_util.c" \
    "$repo_root/src/core/themes/ui_customization.c" \
    -Wl,--gc-sections -lm -o "$build_dir/settings_roundtrip_spec"

(cd "$build_dir/run" && ../settings_roundtrip_spec)
