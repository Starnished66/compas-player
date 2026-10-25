#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/hiby-file-browser-natural-sort.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
common="-std=gnu11 -O2 -Wall -Wextra -DHOST_BUILD=1 -DINDEX_RUN_SIZE=4 -ffunction-sections -fdata-sections -I$root -I$root/src -I$root/src/library -I$root/src/core -I$root/src/ui -I$root/lvgl"
cc $common \
  "$root/src/library/file_browser_natural_sort_test.c" "$root/src/library/library_walk.c" "$root/src/core/utf8_util.c" \
  -Wl,--gc-sections -pthread -o "$out/index-test"
"$out/index-test"
cc $common -DFILE_BROWSER_TEST_FORCE_MEMORY_INDEX=1 \
  "$root/src/library/file_browser_natural_sort_test.c" "$root/src/library/library_walk.c" "$root/src/core/utf8_util.c" \
  -Wl,--gc-sections -pthread -o "$out/memory-test"
"$out/memory-test"
printf '%s\n' 'file browser natural sort: PASS'
