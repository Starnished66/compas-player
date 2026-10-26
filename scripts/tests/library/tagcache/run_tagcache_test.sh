#!/bin/sh
# Builds and runs one tagcache test: run_tagcache_test.sh <name> [--wrap=<symbols>]
# against the real tagcache implementation and its mapped test source.
set -eu
name=$1
wrap=${2:-}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${TMPDIR:-/tmp}/compas-tagcache-$name.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
case "$name" in
  refs|commit-failure) test_dir=core ;;
  storage|write-volume) test_dir=query ;;
  *) echo "unknown tagcache test: $name" >&2; exit 2 ;;
esac
# shellcheck disable=SC2086
cc -std=gnu11 -O1 -Wall -DHOST_BUILD=1 -I"$root" -I"$root/src/library" -I"$root/src/core" -I"$root/src/ui" \
  -I"$root/dr_libs" ${wrap:+-Wl,$wrap} \
  "$root/src/library/tagcache/$test_dir/tagcache_$(echo "$name" | tr - _)_test.c" "$root/src/library/tagcache/core/tagcache.c" \
  "$root/src/core/text/utf8_util.c" "$root/src/core/system/logging/db_log.c" -lpthread -lm -o "$out/test"
"$out/test"
