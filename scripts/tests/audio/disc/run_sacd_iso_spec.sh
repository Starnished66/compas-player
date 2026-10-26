#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="$repo_root/build_ui_test/sacd_iso"
mkdir -p "$build_dir"

# Exercise production CUE, metadata and file-browser objects; the spec supplies
# its own entry point, so omit the application's app/main.o.
make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path "$repo_root/build_host/libwavpack/*" \
    ! -path "$repo_root/build_host/libsndfile/*" \
    ! -name ogg_demux.o ! -path "$repo_root/build_host/app/main.o" -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi
gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I"$repo_root" \
    "$repo_root/tests/audio/sacd_iso_spec.c" -c -o "$build_dir/sacd_iso_spec.o"
g++ -pthread "$build_dir/sacd_iso_spec.o" "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" \
    "$repo_root/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lpthread -lm -o "$build_dir/sacd_iso_spec"
(cd "$repo_root" && "$build_dir/sacd_iso_spec")
