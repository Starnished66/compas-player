#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_tagcache_reverse"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function -ffunction-sections -fdata-sections -pthread -I"$repo" -DHOST_BUILD=1 \
    "$repo/src/library/tagcache/tests/test_tagcache_reverse.c" \
    "$repo"/src/library/tagcache/core/tagcache*.c \
    "$repo/src/library/tagcache/reader/tagcache_reader.c" \
    "$repo/src/library/metadata/db/metadata_db_browse.c" \
    "$repo/src/core/text/utf8_util.c" \
    "$repo/src/core/system/logging/db_log.c" \
    -Wl,--gc-sections -o "$out"
# The same checks against both index layouts.
TAGCACHE_FORCE_INTERN=1 "$out"
TAGCACHE_FORCE_COMPACT=1 "$out"
