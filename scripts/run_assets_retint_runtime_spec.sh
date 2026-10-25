#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${ASSETS_RETINT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/assets_retint}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/main.o" ||
      ! -f "$repo_root/build_host/ui/assets.o" ]]; then
    echo "Cached host objects are missing; refusing an implicit host build" >&2
    exit 2
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/library" -I"$repo_root/lvgl" \
    "$repo_root/src/ui/assets_retint_runtime_spec.c" \
    -c -o "$build_dir/assets_retint_runtime_spec.o"

host_link="$(make -s -C "$repo_root" print-host-link BOARD=r1)"
objcopy --redefine-sym main=compas_player_main \
    --redefine-sym install_thread_crash_altstack=compas_player_install_thread_crash_altstack \
    "$repo_root/build_host/main.o" "$build_dir/main_without_entry.o"
read -r -a link_inputs <<< "$host_link"
objects=("$build_dir/main_without_entry.o")
flags=()
for arg in "${link_inputs[@]}"; do
    [[ "$arg" == "build_host/main.o" ]] && continue
    if [[ "$arg" == *.o || "$arg" == *.a ]]; then
        objects+=("$repo_root/$arg")
    else
        flags+=("$arg")
    fi
done

g++ -pthread -Wl,--gc-sections "$build_dir/assets_retint_runtime_spec.o" \
    "${objects[@]}" "${flags[@]}" $(sdl2-config --libs) -lm \
    -o "$build_dir/assets_retint_runtime_spec"

"$build_dir/assets_retint_runtime_spec"
