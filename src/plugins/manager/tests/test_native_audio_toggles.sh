#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_native_audio_toggles"
mkdir -p "$(dirname "$out")"
cd "$repo"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I. -Isrc -Ilvgl -Ilua/src -IcJSON -I/usr/include/SDL2 \
    -DLV_CONF_INCLUDE_SIMPLE=1 -DHOST_BUILD=1 -DBOARD_R1 \
    src/plugins/manager/tests/test_native_audio_toggles.c src/core/themes/ui_customization.c -o "$out"
"$out"
