#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
out="$root/build/host-tests/test_reflection_cache"
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -DHOST_BUILD=1 -I. \
    src/ui/player/artwork/tests/test_reflection_cache.c \
    src/ui/player/artwork/player_artwork_cache.c -pthread -o "$out"
"$out"
