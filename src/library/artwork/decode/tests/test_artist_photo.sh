#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
out="$root/build/host-tests/test_artist_photo"
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl \
    src/library/artwork/decode/tests/test_artist_photo.c \
    src/library/artwork/decode/library_artist_art.c src/library/artwork/albumart/albumart.c \
    src/library/artwork/coordinator/artwork_failure_cache.c \
    src/library/artwork/coordinator/artwork_memory.c \
    -Wl,--gc-sections -pthread -lm -o "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
"$out"
