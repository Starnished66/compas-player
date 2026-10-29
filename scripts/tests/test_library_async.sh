#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_library_async"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -I"$repo" \
    "$repo/scripts/tests/test_library_async.c" \
    "$repo/src/ui/library/browse/gui_library_async.c" \
    -o "$out"
"$out"
