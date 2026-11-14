#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_usb_dac_lifecycle"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -DHOST_BUILD -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" -I"$repo/lvgl" "$repo/src/ui/player/transport/tests/test_usb_dac_lifecycle.c" \
    -Wl,--gc-sections -pthread -o "$out"
"$out"
