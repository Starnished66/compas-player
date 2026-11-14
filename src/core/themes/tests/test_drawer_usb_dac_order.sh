#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../../" && pwd)"
runtime="$(mktemp -d "${TMPDIR:-/tmp}/drawer_usb_dac_order.XXXXXX")"
trap 'rm -rf "$runtime"' EXIT
cc -std=c11 -DHOST_BUILD=1 -I"$repo" -I"$repo/lvgl/src" -I"$repo/cJSON" \
    "$repo/src/core/themes/tests/test_drawer_usb_dac_order.c" \
    "$repo/src/core/themes/ui_customization.c" -o "$runtime/test"
"$runtime/test"
