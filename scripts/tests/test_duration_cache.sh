#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_duration_cache"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -pthread -I"$repo" -DHOST_BUILD=1 \
    "$repo/scripts/tests/test_duration_cache.c" \
    "$repo/src/library/metadata/durations/duration_cache.c" \
    "$repo/src/library/metadata/durations/duration_cache_merge.c" \
    "$repo/src/library/metadata/durations/duration_cache_file.c" \
    -o "$out"
"$out"
