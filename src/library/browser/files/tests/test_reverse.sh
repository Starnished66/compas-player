#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_file_browser_reverse"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Werror -ffunction-sections -fdata-sections \
    -I"$repo" -I"$repo/src/core" -I"$repo/lvgl" -DLV_CONF_INCLUDE_SIMPLE=1 \
    -DHOST_BUILD=1 "$repo/src/library/browser/files/tests/test_reverse.c" \
    -Wl,--gc-sections -pthread -o "$out"
"$out"
