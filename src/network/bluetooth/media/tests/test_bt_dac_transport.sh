#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_bt_dac_transport"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -pthread -I"$repo" \
    -I"$repo/dbus_vendor_config" -I"$repo/dbus" "$repo/src/network/bluetooth/media/tests/test_bt_dac_transport.c" -o "$out"
"$out"
