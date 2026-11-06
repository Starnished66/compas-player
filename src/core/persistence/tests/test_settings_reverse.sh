#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_settings_reverse"
mkdir -p "$(dirname "$out")"
storage=$(mktemp -d "${TMPDIR:-/tmp}/settings_roundtrip.XXXXXX")
trap 'rm -rf "$storage"' EXIT
cc=${CC:-cc}
"$cc" -std=gnu11 -DHOST_BUILD -O0 -g -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -pthread -I"$repo" -I"$repo/mbedtls/include" \
    "$repo/src/core/persistence/tests/test_settings_reverse.c" \
    "$repo/src/core/themes/ui_customization.c" \
    "$repo/src/core/text/utf8_util.c" \
    "$repo/src/core/persistence/bt_ssc_settings.c" \
    "$repo/src/core/persistence/bt_rc_trust_settings.c" \
    "$repo/src/network/bluetooth/control/bt_rc_trust.c" \
    -Wl,--gc-sections -o "$out"
(cd "$storage" && "$out")
