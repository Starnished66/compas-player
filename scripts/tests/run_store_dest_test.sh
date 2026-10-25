#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/compas-store-dest.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
cc -std=gnu11 -O2 -Wall -Wextra -DHOST_BUILD=1 -I"$root/src/network" -I"$root/src/ui" -I"$root/mbedtls/include" -I"$root/cJSON" \
  "$root/src/network/store_common_test.c" "$root/src/network/store_common.c" \
  "$root/cJSON/cJSON.c" "$root/mbedtls/library/sha256.c" "$root/mbedtls/library/platform_util.c" -o "$out/store-dest-test"
"$out/store-dest-test"
