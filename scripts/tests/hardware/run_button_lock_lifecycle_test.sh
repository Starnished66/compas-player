#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${BUTTON_LOCK_RUNTIME_BUILD_DIR:-$repo_root/build/button-lock-grok/lifecycle}"
mkdir -p "$build_dir"

# Link the production HOST_BUILD plugin manager and Lua runtime. The
# spec provides its own entry point, so omit the application's app/main.o.
make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path "$repo_root/build_host/libwavpack/*" \
    ! -path "$repo_root/build_host/libsndfile/*" \
    ! -name ogg_demux.o \
    ! -path "$repo_root/build_host/app/main.o" -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware" \
    $(sdl2-config --cflags) \
    "$repo_root/src/plugins/lifecycle/button_lock_lifecycle_test.c" \
    -c -o "$build_dir/button_lock_lifecycle_test.o"

g++ -pthread \
    "$build_dir/button_lock_lifecycle_test.o" \
    "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" \
    "$repo_root/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lpthread -lm \
    -o "$build_dir/button_lock_lifecycle_test"

sandbox=$(mktemp -d "$build_dir/sandbox.XXXXXX")
trap 'rm -rf "$sandbox"' EXIT
(cd "$sandbox" && "$build_dir/button_lock_lifecycle_test")
