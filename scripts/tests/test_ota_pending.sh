#!/usr/bin/env bash
# Host test: a stale or invalid OTA pending record is discarded for an SD
# install, the "rejected" marker and a valid record are kept.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_ota_pending"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}

mbed="$repo/mbedtls/library"
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-format-truncation -Wno-unused-parameter -DHOST_BUILD=1 -DBOARD_R1 -pthread \
    -I"$repo" -I"$repo/src/core" -I"$repo/cJSON" -I"$repo/mbedtls/include" -I"$repo/src/network" -I"$repo/lvgl" \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    "$repo/scripts/tests/test_ota_pending.c" "$repo/src/network/firmware/firmware_ota.c" \
    "$repo/src/ui/text/i18n/i18n.c" "$repo/src/ui/text/i18n/i18n_catalog.c" "$repo/cJSON/cJSON.c" \
    "$mbed/sha256.c" "$mbed/platform_util.c" \
    -o "$out"
"$out"
