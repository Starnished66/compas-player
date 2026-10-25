#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_base="${TMPDIR:-/tmp}"
build_dir="$(mktemp -d "$tmp_base/player-queue-storage.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
cd "$repo_root"
flags=(
  -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-misleading-indentation -Wno-unused-function -fwrapv
  -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1
  -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware
  -Isrc/ui -Isrc/ui/player -Itests/player -Isrc/core -Isrc/plugins -Ilvgl -Ithird_party/libwavpack/include
)
link_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  link_flags+=(-fsanitize=address,undefined)
fi
cc=${CC:-cc}
"$cc" "${flags[@]}" -c tests/player/player_queue_storage_spec.c -o "$build_dir/spec.o"
"$cc" "${flags[@]}" -c src/ui/player/player_queue_storage.c -o "$build_dir/storage.o"
"$cc" "${flags[@]}" -c src/library/queue_resume.c -o "$build_dir/queue_resume.o"
"$cc" "${flags[@]}" -c src/library/playlist_files.c -o "$build_dir/playlist_files.o"
"$cc" "${link_flags[@]}" -no-pie -Wl,--gc-sections \
  -Wl,--wrap=queue_resume_write_at -Wl,--wrap=pthread_create \
  -Wl,--wrap=syscall \
  -Wl,--wrap=fsync \
  -Wl,--wrap=renameat -Wl,--wrap=openat -Wl,--wrap=close \
  -o "$build_dir/player_queue_storage_spec" \
  "$build_dir/spec.o" "$build_dir/storage.o" "$build_dir/queue_resume.o" \
  "$build_dir/playlist_files.o" -lpthread -lm
PLAYER_QUEUE_STORAGE_SPEC_TMPDIR="$build_dir" "$build_dir/player_queue_storage_spec"
