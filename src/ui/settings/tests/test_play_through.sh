#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_play_through"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers \
    -ffunction-sections -fdata-sections -D_GNU_SOURCE -DHOST_BUILD=1 -DBOARD_R1 \
    -DLV_CONF_INCLUDE_SIMPLE=1 -I"$repo" -I"$repo/lvgl" -I"$repo/src/audio" \
    -I"$repo/cJSON" -I"$repo/lua/src" -I"$repo/mbedtls/include" \
    "$repo/src/ui/settings/tests/test_play_through.c" -Wl,--gc-sections -lm -pthread -o "$out"
"$out"
