#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out="$root/build/wifi-ssid-escape-test"
mkdir -p "$root/build"
cc -std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 \
  -I"$root/src" -I"$root/src/core" -I"$root/mbedtls/include" \
  "$root/scripts/tests/network/wifi_ssid_escape_test.c" -o "$out"
(cd "$root" && "$out")
rm -f "$out"
