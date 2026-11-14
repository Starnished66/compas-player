#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_usb_drive"
mkdir -p "$(dirname "$out")"
runtime=$(mktemp -d "${TMPDIR:-/tmp}/usb_drive.XXXXXX")
trap 'rm -rf "$runtime"' EXIT
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" "$repo/src/core/storage/tests/test_usb_drive.c" \
    "$repo/src/core/storage/sd_fsck.c" "$repo/src/core/text/utf8_util.c" \
    -Wl,--gc-sections -pthread -o "$out"
(cd "$runtime" && "$out")
