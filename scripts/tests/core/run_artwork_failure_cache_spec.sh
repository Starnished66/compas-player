#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT="${TMPDIR:-/tmp}/artwork_failure_cache_spec.$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread \
  -I"$ROOT/src/library" \
  "$ROOT/src/library/artwork/coordinator/artwork_failure_cache.c" \
  "$ROOT/src/library/artwork/coordinator/artwork_failure_cache_spec.c" \
  -Wl,--wrap=clock_gettime -o "$OUT"
timeout 5 "$OUT"
echo "Artwork failure cache spec passed"
