#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../.." && pwd)
build_root=${METADATA_DB_DISPLAY_CACHE_BUILD_ROOT:-$repo/build_host}
mkdir -p "$build_root"
test_root=$(mktemp -d "$build_root/metadata_db_display_cache.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror -pthread -DHOST_BUILD=1 \
    -ffunction-sections -fdata-sections \
    -I"$repo" -I"$repo/src" -I"$repo/src/library" -I"$repo/src/core" \
    -I"$repo/src/ui" -I"$repo/lvgl" \
    "$repo/src/library/metadata/db/metadata_db_display_cache_test.c" \
    "$repo/src/library/tagcache/core/tagcache.c" "$repo/src/core/system/logging/db_log.c" \
    "$repo/src/core/text/utf8_util.c" \
    -Wl,--gc-sections -o "$test_root/metadata_db_display_cache_test"

(cd "$test_root" && ./metadata_db_display_cache_test)
