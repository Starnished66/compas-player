#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/compas-library-scan.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT

sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"${CC:-cc}" -std=gnu11 -DHOST_BUILD -O1 -g -Wall -Wextra \
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-format-truncation "${sanitize_flags[@]}" \
    -I"$repo_root" -I"$repo_root/src/library" -I"$repo_root/src/core" \
    -I"$repo_root/src/ui" -I"$repo_root/src/audio" -I"$repo_root/src/hardware" \
    -I"$repo_root/src/plugins" \
    "$repo_root/src/ui/library/tests/library_scan_spec.c" -pthread -o "$run_dir/library_scan_spec"

cd "$run_dir"
./library_scan_spec
