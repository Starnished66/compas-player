#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_bluetooth_readiness"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=c11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
    -ffunction-sections -fdata-sections -pthread -I"$repo" \
    "$repo/scripts/tests/test_bluetooth_readiness.c" \
    "$repo/src/network/bluetooth/control/bt_ssc_policy.c" \
    -Wl,--gc-sections -o "$out"
"$out"
