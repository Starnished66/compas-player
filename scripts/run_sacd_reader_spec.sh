#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
make -s build_host/libwavpack/libwavpack.a
sanitizers=()
if [[ "${SANITIZE:-0}" == 1 ]]; then sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror "${sanitizers[@]}" \
  -I"$repo_dir/src/audio" -I"$repo_dir/src/library" -I"$repo_dir/third_party/libwavpack/include" \
  "$repo_dir/tests/sacd_reader_spec.c" \
  "$repo_dir/src/audio/sacd_reader.c" "$repo_dir/src/audio/dst_decoder.c" \
  "$repo_dir/src/audio/dsd_decoder.c" "$repo_dir/src/audio/dsd_filter.c" \
  "$repo_dir/src/library/sacd_iso.c" \
  "$repo_dir/build_host/libwavpack/libwavpack.a" -lm -o "$repo_dir/build_host/sacd_reader_spec"
"$repo_dir/build_host/sacd_reader_spec"
cache_dir="${HOME}/.cache/compas-theme-review/sacd"
if [[ -f "$cache_dir/frame-0.dst" && -f "$cache_dir/frame-9.dst" && -f "$cache_dir/our.raw" ]]; then
  "$repo_dir/build_host/sacd_reader_spec" --cached-dst "$cache_dir"
else
  echo "SKIP: optional real compressed DST fixtures are unavailable ($cache_dir)"
fi
