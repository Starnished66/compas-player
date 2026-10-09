#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${USB_DAC_SPEC_BUILD_DIR:-$repo_root/build_ui_test/usb_dac}"
mkdir -p "$build_dir"
cc -O0 -g -Wall -Wextra -I"$repo_root/src/audio" -I"$repo_root/src/hardware" \
    -I"$repo_root/src/core" -I"$repo_root/src/library" \
    "$repo_root/src/hardware/usb_dac_bridge_lifecycle_regression.c" -pthread \
    -o "$build_dir/usb_dac_bridge_lifecycle_spec"
"$build_dir/usb_dac_bridge_lifecycle_spec"
