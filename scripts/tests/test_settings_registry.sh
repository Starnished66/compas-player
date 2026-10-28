#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_settings_registry"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -pthread -D_GNU_SOURCE \
    -I"$repo" -I"$repo/cJSON" -I"$repo/src/core" \
    "$repo/scripts/tests/test_settings_registry.c" \
    "$repo/src/core/persistence/settings_registry.c" \
    "$repo/src/core/persistence/settings_reg_audio.c" \
    "$repo/src/core/persistence/settings_reg_device.c" \
    "$repo/src/network/remote/control/remote_settings.c" \
    "$repo/src/network/remote/control/remote_settings_json.c" \
    "$repo/src/network/remote/control/remote_settings_gate.c" \
    "$repo/cJSON/cJSON.c" -lm -o "$out"
"$out"
