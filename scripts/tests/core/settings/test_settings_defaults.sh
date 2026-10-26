#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${SETTINGS_TEST_BUILD_DIR:-$repo_root/build_test/settings-defaults}"
mkdir -p "$build_dir/run"

cc -std=gnu11 -D_GNU_SOURCE -DHOST_BUILD=1 -O1 -Wall -Wextra \
    -ffunction-sections -fdata-sections -pthread \
    -I"$repo_root" -I"$repo_root/src/core" -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
    "$repo_root/src/core/persistence/settings_defaults_test.c" "$repo_root/src/core/persistence/settings.c" "$repo_root/src/core/text/utf8_util.c" \
    "$repo_root/src/core/themes/ui_customization.c" \
    -Wl,--gc-sections -lm -o "$build_dir/settings_defaults_test"
(cd "$build_dir/run" && ../settings_defaults_test)
