#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_usb_dac_host_presence"
mkdir -p "$(dirname "$out")"
runtime=$(mktemp -d "${TMPDIR:-/tmp}/usb_dac_host_presence.XXXXXX")
trap 'rm -rf "$runtime"' EXIT
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" "$repo/src/hardware/usb/tests/test_usb_dac_host_presence.c" \
    -Wl,--gc-sections -pthread -o "$out"
(cd "$runtime" && "$out")
