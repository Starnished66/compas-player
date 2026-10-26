#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${EMULATOR_TEST_BUILD_DIR:-$repo_root/build_ui_test/emulators}"
mkdir -p "$build_dir"

gcc -std=c99 -D_GNU_SOURCE -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1 -O1 -Wall -Wextra \
    -ffunction-sections -fdata-sections \
    -I"$repo_root" -I"$repo_root/src/network" -I"$repo_root/src/core" -I"$repo_root/src/ui" -I"$repo_root/src/audio" -I"$repo_root/src/library" -I"$repo_root/src/hardware" \
    -I"$repo_root/mbedtls/include" -I"$repo_root/cJSON" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$repo_root/lua/src" \
    "$repo_root/src/network/store/catalog/emulator_store_test.c" "$repo_root/src/network/store/catalog/emulator_store.c" \
    "$repo_root/src/network/store/plugins/store_common.c" "$repo_root/src/core/device/emulator_registry.c" \
    "$repo_root/cJSON/cJSON.c" "$repo_root/src/ui/text/i18n/i18n.c" "$repo_root/src/ui/text/i18n/i18n_catalog.c" \
    -Wl,--gc-sections -lpthread -lm -o "$build_dir/emulator_store_test"
"$build_dir/emulator_store_test"
