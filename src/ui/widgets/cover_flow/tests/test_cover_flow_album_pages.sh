#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
out=build/host-tests/test_cover_flow_album_pages
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -Wall -Wextra -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware \
    -Isrc/ui -Isrc/ui/player -Isrc/ui/library -Isrc/core -Isrc/plugins -Isrc/plugins/bindings -Ilvgl \
    src/ui/widgets/cover_flow/tests/test_cover_flow_album_pages.c \
    -Wl,--gc-sections -lpthread -o "$out"
"$out"
