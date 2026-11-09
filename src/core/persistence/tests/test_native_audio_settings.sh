#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_native_audio_settings"
mkdir -p "$(dirname "$out")"
fixture=$(mktemp -d "$repo/build/host-tests/native-audio.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -D_GNU_SOURCE -DHOST_BUILD=1 -DBOARD_R1 \
    -I"$repo" -I"$repo/src/audio" -I"$repo/cJSON" -pthread \
    "$repo/src/core/persistence/tests/test_native_audio_settings.c" \
    "$repo/src/core/persistence/native_audio_settings.c" \
    "$repo/src/audio/effects/mseb.c" "$repo/src/audio/effects/soundstage.c" \
    "$repo/src/audio/effects/biquad_cookbook.c" \
    -Wl,--gc-sections -lm -o "$out"
"$out" "$fixture"
