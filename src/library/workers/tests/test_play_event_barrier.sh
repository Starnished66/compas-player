#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/play-event-barrier.XXXXXX")
trap 'rm -f "$out"' EXIT
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -pthread -I"$repo" -DHOST_BUILD=1 "$repo/src/library/workers/tests/test_play_event_barrier.c" \
    -Wl,--gc-sections -o "$out"
"$out"
