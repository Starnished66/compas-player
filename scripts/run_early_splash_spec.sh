#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${EARLY_SPLASH_SPEC_BUILD_DIR:-$repo_root/build_ui_test/early_splash}"
mkdir -p "$build_dir"
cc="${CC:-gcc}"
common=(-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE=1 -DBOARD_R1
  -DEARLY_SPLASH_NO_MAIN -I"$repo_root/src/bootloader" -I"$repo_root/src/core")

"$cc" "${common[@]}" -c "$repo_root/src/bootloader/early_splash.c" -o "$build_dir/early_splash.o"
"$cc" "${common[@]}" "$repo_root/src/bootloader/early_splash_spec.c" \
  "$build_dir/early_splash.o" -o "$build_dir/early_splash_spec"
"$build_dir/early_splash_spec"

if [[ "${SANITIZE:-0}" == 1 ]]; then
  "$cc" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$repo_root/src/bootloader/early_splash.c" \
    "$repo_root/src/bootloader/early_splash_spec.c" -o "$build_dir/early_splash_spec_san"
  "$build_dir/early_splash_spec_san"
fi
