#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=${TMPDIR:-/tmp}/compas-device-info.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
cc -std=gnu11 -O2 -Wall -Wextra -DHOST_BUILD=1 -DBOARD_R1 \
  -I"$root/src/core" -I"$root/src/library" -I"$root/src/plugins" -I"$root/src/ui" \
  "$root/src/core/device/device_info_test.c" "$root/src/core/device/device_info.c" "$root/src/core/system/runtime/app_version.c" \
  -o "$out/device-info-test"
"$out/device-info-test" "$out"
