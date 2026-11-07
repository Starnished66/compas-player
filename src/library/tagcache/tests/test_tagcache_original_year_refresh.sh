#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_tagcache_original_year_refresh"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -I"$repo" -DHOST_BUILD=1 -DTAGCACHE_MAX_ENTRIES=64 \
    "$repo/src/library/tagcache/tests/test_tagcache_original_year_refresh.c" \
    "$repo"/src/library/tagcache/core/tagcache*.c \
    "$repo/src/library/tagcache/reader/tagcache_reader.c" \
    "$repo/src/core/text/utf8_util.c" \
    "$repo/src/core/system/logging/db_log.c" \
    -o "$out"
TAGCACHE_FORCE_INTERN=1 "$out"
TAGCACHE_FORCE_COMPACT=1 "$out"
