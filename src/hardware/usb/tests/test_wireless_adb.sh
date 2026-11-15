#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_wireless_adb"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -pthread \
    "$repo/src/hardware/usb/tests/test_wireless_adb.c" -o "$out"
timeout 10 "$out"
