#!/usr/bin/env bash
# File Manager copy/move/delete worker (src/library/file_ops.c).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build_test/file-ops"
mkdir -p "$build_dir"
# musl's default thread stack, to show deep trees fit without the larger
# stack the firmware build gives the worker.
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -DWORKER_STACK_SIZE='(128 * 1024)' \
    -I"$repo_root" -I"$repo_root/src/library" -I"$repo_root/src/hardware" \
    "$repo_root/src/library/file_ops_spec.c" "$repo_root/src/library/file_ops.c" \
    -Wl,--wrap=readdir -Wl,--wrap=fdatasync -Wl,--wrap=rename \
    -o "$build_dir/file_ops_spec"
"$build_dir/file_ops_spec"
