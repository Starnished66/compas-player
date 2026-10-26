#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="$repo_root/build_ui_test/playlist_bulk_delete"
mkdir -p "$build_dir"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo_root/src/library" \
    "$repo_root/src/library/playlists/playlist_bulk_delete_test.c" \
    "$repo_root/src/library/playlists/playlist_files.c" \
    -Wl,--gc-sections -Wl,--wrap=rename -pthread -o "$build_dir/test"
(cd "$build_dir" && ./test)
echo 'Playlist bulk deletion passed'
