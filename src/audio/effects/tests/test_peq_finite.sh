#!/bin/bash
set -eu
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
cc -O2 -ffunction-sections -fdata-sections -DHOST_BUILD=1 -I"$repo" \
    "$repo/src/audio/effects/tests/test_peq_finite.c" \
    "$repo/src/audio/effects/biquad_cookbook.c" \
    "$repo/src/audio/effects/soundstage.c" "$repo/src/audio/effects/soundstage_dsp.c" \
    "$repo/src/audio/effects/soundstage_room.c" "$repo/src/audio/effects/soundstage_run.c" \
    -Wl,--gc-sections -lm -lpthread -o "$out"
"$out"
