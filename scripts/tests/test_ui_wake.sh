#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_ui_wake"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -pthread -I"$repo" \
    "$repo/scripts/tests/test_ui_wake.c" \
    "$repo/src/core/ui/display/ui_wake.c" \
    -o "$out"
"$out"
