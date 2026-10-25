#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/hiby-album-artist-sort-test.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"

set -- -std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 -ffunction-sections -fdata-sections \
  "-I$root" "-I$root/src" "-I$root/src/library" "-I$root/src/core" "-I$root/src/ui" "-I$root/lvgl"
cc "$@" -c "$root/src/library/tagcache.c" -o "$out/tagcache.o"
cc "$@" -c "$root/src/library/metadata_db.c" -o "$out/metadata_db.o"
cc "$@" "$root/tests/metadata_db_album_artist_sort_spec.c" \
  "$out/metadata_db.o" "$out/tagcache.o" "$root/src/core/db_log.c" "$root/src/core/utf8_util.c" \
  -Wl,--gc-sections -lpthread -o "$out/album-artist-sort-test"
mkdir "$out/work"
(cd "$out/work" && "$out/album-artist-sort-test" "$out/work")
printf '%s\n' 'metadata_db album artist sort regression: PASS'
