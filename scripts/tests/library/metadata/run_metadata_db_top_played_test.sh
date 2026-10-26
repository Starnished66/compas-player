#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${TMPDIR:-/tmp}/hiby-top-played-test.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"

common="-std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 -ffunction-sections -fdata-sections -I$root -I$root/src -I$root/src/library -I$root/src/core -I$root/src/ui -I$root/lvgl"
cc $common -c "$root/src/library/tagcache/core/tagcache.c" -o "$out/tagcache.o"
cc $common -c "$root/src/library/metadata/db/metadata_db.c" -o "$out/metadata_db.o"
cc $common "$root/scripts/tests/library/metadata/metadata_db_top_played_regression.c" \
  "$out/metadata_db.o" "$out/tagcache.o" "$root/src/core/system/logging/db_log.c" \
  "$root/src/core/text/utf8_util.c" \
  -Wl,--gc-sections -lpthread -o "$out/top-played-test"
mkdir "$out/work"
(cd "$out/work" && "$out/top-played-test" "$out/work")
printf '%s\n' 'metadata_db top-played regression: PASS'
