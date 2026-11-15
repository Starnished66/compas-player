#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_usb_adb_cleanup"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" "$repo/src/hardware/usb/tests/test_usb_adb_cleanup.c" \
    -Wl,--gc-sections -pthread -o "$out"
"$out"
