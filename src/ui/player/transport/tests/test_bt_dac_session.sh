#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_bt_dac_session"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -I"$repo" \
    "$repo/src/ui/player/transport/tests/test_bt_dac_session.c" -o "$out"
"$out"
