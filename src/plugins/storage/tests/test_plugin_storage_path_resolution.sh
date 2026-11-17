#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_plugin_storage_path_resolution"
mkdir -p "$(dirname "$out")"
cd "$repo"
"${CC:-cc}" -std=gnu11 -DHOST_BUILD=1 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -Wl,--gc-sections -I. -Isrc -Isrc/core -Isrc/plugins -I$(pwd)/src/plugins/storage -Imbedtls/include \
  src/plugins/storage/tests/test_plugin_storage_path_resolution.c -lpthread -o "$out"
"$out"
