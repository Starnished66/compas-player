#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
out=build/host-tests/test_album_facts_genre
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 -DHOST_BUILD=1 \
    src/ui/library/browse/tests/test_album_facts_genre.c \
    src/library/tagcache/core/tagcache*.c src/library/tagcache/reader/tagcache_reader.c \
    src/core/text/utf8_util.c src/core/system/logging/db_log.c \
    -Wl,--gc-sections -o "$out"
"$out"
