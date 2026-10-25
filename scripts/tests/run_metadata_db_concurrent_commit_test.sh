#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/hiby-metadata-concurrent.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
cc -std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 \
  -I"$root" -I"$root/src" -I"$root/src/library" -I"$root/src/core" -I"$root/src/ui" \
  "$root/scripts/tests/metadata_db_concurrent_commit_test.c" "$root/src/library/metadata_db.c" \
  "$root/src/library/tagcache.c" "$root/src/core/db_log.c" "$root/src/core/utf8_util.c" \
  -pthread -Wl,--wrap=write -Wl,--wrap=pwrite \
  -o "$out/concurrent-test"
for mode in success failure abort staging-success staging-failure; do
  (cd "$out" && ./concurrent-test "$mode")
done
echo "PASS: metadata queries and user statistics remain safe across concurrent commit, failure, and abort"
