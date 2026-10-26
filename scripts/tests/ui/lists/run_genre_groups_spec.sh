#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${TMPDIR:-/tmp}/hiby-genre-groups-test.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"

set -- -std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 -ffunction-sections -fdata-sections \
  "-I$root" "-I$root/src" "-I$root/src/library" "-I$root/src/core" "-I$root/src/ui" "-I$root/lvgl"
cc "$@" -c "$root/src/library/tagcache/core/tagcache.c" -o "$out/tagcache.o"
cc "$@" -c "$root/src/library/metadata/db/metadata_db.c" -o "$out/metadata_db.o"
cc "$@" "$root/tests/library/metadata_db_genre_groups_spec.c" \
  "$out/metadata_db.o" "$out/tagcache.o" "$root/src/core/system/logging/db_log.c" "$root/src/core/text/utf8_util.c" \
  -Wl,--gc-sections -lpthread -o "$out/genre-groups-test"
mkdir "$out/work"
(cd "$out/work" && "$out/genre-groups-test" "$out/work")
printf '%s\n' 'metadata_db genre groups (issue #191): PASS'
