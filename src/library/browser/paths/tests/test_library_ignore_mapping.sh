#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_library_ignore_mapping"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function -I"$repo" -DHOST_BUILD=1 -DCOMPAS_RUNTIME_SD_ROOT=1 \
    "$repo/src/library/browser/paths/tests/test_library_ignore_mapping.c" -o "$out"
"$out"
