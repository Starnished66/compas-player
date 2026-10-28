#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_queue_checkpoint"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -I"$repo" -DHOST_BUILD=1 \
    "$repo/scripts/tests/test_queue_checkpoint.c" \
    "$repo/src/ui/player/queue/player_queue_model.c" \
    "$repo/src/ui/player/queue/player_queue_storage.c" \
    "$repo/src/library/playlists/queue_resume.c" \
    -lm -o "$out"
"$out"
