#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${PLAYER_QUEUE_MODEL_SPEC_BUILD_DIR:-$repo_root/build_ui_test/player_queue_model}"
mkdir -p "$build_dir"
cd "$repo_root"
flags=(
  -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-misleading-indentation -Wno-unused-function -fwrapv
  -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1
  -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware
  -Isrc/ui -Isrc/ui/player -Itests/player -Isrc/core -Isrc/plugins
  -Ilvgl -Ithird_party/libwavpack/include
)
link_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  link_flags+=(-fsanitize=address,undefined)
fi
cc=${CC:-cc}
"$cc" "${flags[@]}" -c tests/player/player_queue_model_spec.c -o "$build_dir/spec.o"
"$cc" "${flags[@]}" -c src/ui/player/queue/player_queue_model.c -o "$build_dir/model.o"
"$cc" "${flags[@]}" -c src/ui/player/queue/player_queue_storage.c -o "$build_dir/storage.o"
"$cc" "${flags[@]}" -c src/library/playlists/queue_resume.c -o "$build_dir/queue_resume.o"
"$cc" "${link_flags[@]}" -no-pie -Wl,--gc-sections \
  -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=close \
  -o "$build_dir/player_queue_model_spec" \
  "$build_dir/spec.o" "$build_dir/model.o" "$build_dir/storage.o" "$build_dir/queue_resume.o" \
  -lpthread -lm
"$build_dir/player_queue_model_spec"
