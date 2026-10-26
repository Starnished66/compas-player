#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
OUT="${TMPDIR:-/tmp}/player_cover_prefetch_test.$$"
trap 'rm -f "$OUT"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -DPLAYER_COVER_PREFETCH_TEST \
  -DCOVER_RASTER_CACHE_TEST \
  -Wall -Wextra -Werror -pthread \
  -I"$ROOT/src/library" \
  -I"$ROOT/src/audio" -I"$ROOT/src/core" \
  "$ROOT/src/library/artwork/prefetch/player_cover_prefetch.c" \
  "$ROOT/src/library/artwork/prefetch/player_cover_prefetch_test.c" \
  "$ROOT/src/library/artwork/cache/cover_raster_cache.c" \
  "$ROOT/src/library/artwork/coordinator/artwork_coordinator.c" \
  "$ROOT/src/library/artwork/coordinator/artwork_memory.c" \
  "$ROOT/src/library/artwork/coordinator/artwork_failure_cache.c" -o "$OUT"
timeout 8 "$OUT"
