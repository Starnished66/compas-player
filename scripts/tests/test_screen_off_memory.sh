#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_screen_off_memory"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -I"$repo" \
    "$repo/scripts/tests/test_screen_off_memory.c" \
    "$repo/src/library/playlists/queue_resume.c" \
    -lm -o "$out"
"$out"
