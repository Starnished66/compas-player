#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/compas-playlist-target.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT

sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror "${sanitize_flags[@]}" \
    -I"$repo_root/src/ui" \
    "$repo_root/src/ui/gui_playlist_target_spec.c" \
    "$repo_root/src/ui/gui_playlist_target.c" \
    -o "$run_dir/gui_playlist_target_spec"
"$run_dir/gui_playlist_target_spec"
