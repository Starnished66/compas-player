#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/compas-metadata-catalog.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT

common_flags=(
    -std=gnu11 -DHOST_BUILD -O1 -g -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-format-truncation -Wno-restrict
    -I"$repo_root" -I"$repo_root/src/library" -I"$repo_root/src/core"
    -I"$repo_root/src/ui" -I"$repo_root/src/audio" -I"$repo_root/src/hardware"
    -I"$repo_root/src/plugins" -I"$repo_root/src/network"
)
sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"${CC:-cc}" "${common_flags[@]}" "${sanitize_flags[@]}" -Dpath_cache_drop=path_cache_drop_impl \
    -c "$repo_root/src/library/path_cache.c" -o "$run_dir/path_cache.o"
"${CC:-cc}" "${common_flags[@]}" "${sanitize_flags[@]}" \
    "$repo_root/src/library/metadata_catalog_order_spec.c" \
    "$repo_root/src/library/metadata_db.c" \
    "$repo_root/src/library/tagcache.c" \
    "$repo_root/src/library/albumart.c" \
    "$repo_root/src/library/subsonic_saved_servers.c" \
    "$repo_root/src/network/catalog_source_cache.c" \
    "$repo_root/src/ui/fallback_font.c" \
    "$repo_root/src/core/utf8_util.c" \
    "$repo_root/src/core/db_log.c" \
    "$run_dir/path_cache.o" -pthread -Wl,--gc-sections \
    -o "$run_dir/metadata_catalog_order_spec"

cd "$run_dir"
./metadata_catalog_order_spec
