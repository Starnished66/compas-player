#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
out_dir="$repo/build_target/tmp/cctmp"
mkdir -p "$out_dir"
bin="$out_dir/test_usb_dac_bridge_ownership"

"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-unused-function -Wno-unused-variable -pthread \
    -ffunction-sections -fdata-sections -I"$repo" \
    "$repo/src/hardware/usb/tests/test_usb_dac_bridge_ownership.c" \
    -Wl,--gc-sections -o "$bin"
"$bin"
echo "USB DAC bridge ownership tests passed"
