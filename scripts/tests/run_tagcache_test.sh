#!/bin/sh
# Builds and runs one tagcache test: run_tagcache_test.sh <name> [--wrap=<symbols>]
# against the real tagcache.c (tagcache_<name>_test.c in src/library).
set -eu
name=$1
wrap=${2:-}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/compas-tagcache-$name.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
# shellcheck disable=SC2086
cc -std=gnu11 -O1 -Wall -DHOST_BUILD=1 -I"$root" -I"$root/src/library" -I"$root/src/core" -I"$root/src/ui" \
  -I"$root/dr_libs" ${wrap:+-Wl,$wrap} \
  "$root/src/library/tagcache_$(echo "$name" | tr - _)_test.c" "$root/src/library/tagcache.c" \
  "$root/src/core/utf8_util.c" "$root/src/core/db_log.c" -lpthread -lm -o "$out/test"
"$out/test"
