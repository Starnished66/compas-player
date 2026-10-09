#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${ASSETS_RETINT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/assets_retint}"
mkdir -p "$build_dir"

make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' ! -path '*/main.o' -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/library" -I"$repo_root/lvgl" \
    "$repo_root/src/ui/assets_retint_runtime_spec.c" \
    -c -o "$build_dir/assets_retint_runtime_spec.o"

g++ -pthread "$build_dir/assets_retint_runtime_spec.o" "${objects[@]}" \
    $(sdl2-config --libs) -lm -o "$build_dir/assets_retint_runtime_spec"

"$build_dir/assets_retint_runtime_spec"
