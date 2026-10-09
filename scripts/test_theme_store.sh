#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${THEME_STORE_TEST_BUILD_DIR:-$repo_root/build_ui_test/theme-store}"
mkdir -p "$build_dir"

for board in BOARD_R1 BOARD_R3PROII BOARD_R3II_2025; do
    gcc -std=c99 -D_GNU_SOURCE -DHOST_BUILD=1 -D"$board" -DLV_CONF_INCLUDE_SIMPLE=1 -O1 -Wall \
        -ffunction-sections -fdata-sections \
        -I"$repo_root" -I"$repo_root/src/network" -I"$repo_root/src/plugins" \
        -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio" \
        -I"$repo_root/src/library" -I"$repo_root/src/hardware" \
        -I"$repo_root/mbedtls/include" -I"$repo_root/cJSON" -I"$repo_root/lua/src" \
        -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
        "$repo_root/src/network/plugin_store_theme_test.c" \
        "$repo_root/cJSON/cJSON.c" "$repo_root/src/ui/i18n.c" "$repo_root/src/ui/i18n_catalog.c" \
        -Wl,--gc-sections -lpthread -lm -o "$build_dir/theme_store_test_$board"
    "$build_dir/theme_store_test_$board"
done
