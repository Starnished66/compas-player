#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_bluetooth_dac_route"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=c11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
    -ffunction-sections -fdata-sections -pthread -I"$repo" \
    "$repo/src/network/bluetooth/control/tests/test_bt_dac_route.c" -Wl,--gc-sections -o "$out"
"$out"
