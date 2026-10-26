#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${USB_DSD_ROUTE_SPEC_BUILD_DIR:-$repo_root/build_test/usb_dsd_route}"
mkdir -p "$build_dir"

cc -std=gnu11 -D_GNU_SOURCE -DHOST_BUILD=1 -O1 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -pthread \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/core" \
    -I"$repo_root/src/hardware" -I"$repo_root/src/network" -I"$repo_root/tinyalsa/include" \
    "$repo_root/src/audio/output/usb_dsd_route_spec.c" \
    -Wl,--gc-sections -pthread -o "$build_dir/usb_dsd_route_spec"
"$build_dir/usb_dsd_route_spec"
